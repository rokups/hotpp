// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hotc: the command line tool of Hot++. It has two modes.
//
// Build mode (default): compile .hot files ahead of time into one shared library for each file.
//   hotc <file.hot>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]
//        [--cxx command] [--cxxflag flag]... [--slots file] [--raw-diagnostics] [--cache-dir dir]
//   hotc <file.hot> --gen N -o x.so [-I dir]... [--resource-dir dir]
//        [--cxx command] [--cxxflag flag]... [--slots file]
// For each file the tool writes DIR/<name>.g<N>.cpp and DIR/<name>.g<N>.so.
// <name> is the file name without the extension. With -o, the tool writes the one
// library to x.so and the C++ file to the directory of x.so.
// A program loads the .so with hot::module_from_library and gives the Module to the Loader.
// The slot file (default DIR/slots.txt) keeps the slot order between generations.
// Build the generations of one class in order: 1, then 2, and so on.
// The tool needs no CMake.
// A compile error speaks script names (design 4.2). --raw-diagnostics prints the text of the
// compiler as it is. Emit mode has no compiler, so the option has no effect there.
//
// Build mode can write a compile database for clangd (design M14):
//   hotc <file.hot>... --gen N --out-dir DIR --compile-commands CDIR
// The tool writes CDIR/compile_commands.json with one entry for each generated .cpp. The entry
// has the compile command of the backend (the compiler, the include paths and the flags).
// The tool writes the entries also when a compile fails, because the .cpp exists then.
//
// Emit mode (--emit-cpp): translate one .hot file to C++. The tool does not compile it.
//   hotc --emit-cpp <file.hot> --gen N [-o out.cpp] [-I dir]... [--prelude file]...
//        [--slots file] [--resource-dir dir]
// Without -o, the tool prints the C++ to the standard output.
//
// AOT mode (--aot): translate scripts for an AOT build (design M7).
//   hotc --aot <file.hot>... --out-dir DIR [-I dir]... [--prelude file]...
//        [--no-strict-borrowed] [--null-warnings] [--format=std|fmt] [--view=hot|std] [--resource-dir dir] [--aot-optimized]
// The flag --aot-optimized builds optimized AOT code. It needs --aot.
// The option --unit-name NAME gives the unit name of a script (doc/module-level.md, "Unit names").
// The n-th option is for the n-th file. Without it, the unit name of a file is its stem. The
// option is for --aot and --emit-cpp. The AOT table names each script by its unit name.
// The option --scripts-file FILE reads scripts from a list file. Each line is PATH or
// PATH<TAB>UNIT_NAME. The tool drops empty lines. A relative PATH is relative to the current
// directory. The list has no comments. A line without a unit name uses the stem. The scripts of
// the list and the scripts of the command line keep the order of the command line. The option
// --unit-name with --scripts-file is an error (exit code 1).
// The option --jobs N (1..4096) sets the count of threads that translate the scripts. Without it,
// the environment variable HOT_JOBS sets the count. Else the count is min(cores, 16). On Windows the count is 1 and HOT_JOBS has no effect. The tool
// prints the progress line "[i/N] <unit name>" and the diagnostics of each script in the order
// of the scripts, so the output is the same for each count of threads.
// For each file the tool writes DIR/<name>.aot.cpp (generation 1). It also writes
// DIR/hot_aot_registry.cpp, which defines the constant table hot::aot_table
// (include/hot/aot.h). The tool does not compile. The CMake function
// hot_add_scripts with AOT builds a static library from these files. The program links the
// library and hot_aot. It has no loader.
//
// The flag --format=std|fmt is a flag for build systems. In build mode the build of hotc fixes
// the format library (include/hot/config.h), and a different --format is an error (exit code 2).
//
// The flag --view=hot|std is also a flag for build systems. It names the view type of "T[]"
// (hot::span or std::span). In build mode the build of hotc fixes the view type, and a different
// --view is an error (exit code 2). --view=std with --std=c++17 is a translation error.
//
// The flag --std=c++17|c++20|c++23 sets the C++ standard of the parse of the native headers and
// of the compile of the generated code. The default is the C++ standard of the host (HOT_CXX_STANDARD
// in include/hot/config.h). The flag is useful when hotc is built apart from the host.
//
// The flag --depfile <path> writes a dependency file in the Make format after a success:
// "<output>: dep1 dep2 ...". The dependencies are the native headers and the imported scripts
// that the translation read. A space in a path gets a backslash. In AOT mode the file has one
// rule for each script, and the output of the rule is the .aot.cpp file. Each rule lists the
// dependencies of all scripts: one command makes all outputs. In build mode the
// output is the .so file. In emit mode it needs -o. CMake and Ninja read the file.
//
// Mask mode (--mask-embedded): mask the script blocks of a host file (HOT-60, include/hot/embedded_script.h).
//   hotc --mask-embedded <in.rml> <prefix-file> -o <out.hot> [--tag script]
// The tool reads the input and the prefix file as bytes (no line end translation). It writes
// hot::mask_embedded_scripts(input, tag, &found, prefix) to the output as bytes. The prefix is the
// content of the prefix file without one final line end (LF or CRLF), because the prefix has no
// line end. The default tag is "script".
// A file with no block gives no output. The tool deletes an old file at the output path, so a
// build rule does not keep a stale unit. It prints "hotc: no script block in <in>" on stdout and
// exits with code 0. CMake checks the output file, so the exit code 0 suits a custom command.
// A read or write error prints a message and exits with code 1.
//
// Exit code: 0 success, 1 error, 2 bad usage.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/config.h"
#include "hot/embedded_script.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "hot/runtime/hashing.h"
#include "hot/runtime/process.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

[[noreturn]] void usage()
{
    std::fprintf(stderr,
        "usage: hotc <file.hot>... --gen N (--out-dir DIR | -o x.so) [-I dir]... [--resource-dir dir]\n"
        "            [--cxx command] [--cxxflag flag]... [--slots file] [--no-strict-borrowed] [--null-warnings] [--format=std|fmt] [--view=hot|std]\n"
        "            [--raw-diagnostics] [--compile-commands dir] [--std=c++17|c++20|c++23]\n"
        "       hotc --emit-cpp <file.hot> --gen N [-o out.cpp] [-I dir]... [--prelude file]... [--no-strict-borrowed] "
        "[--format=std|fmt] [--view=hot|std]\n"
        "            [--slots file] [--resource-dir dir] [--std=c++17|c++20|c++23]\n"
        "       hotc --aot <file.hot>... --out-dir DIR [-I dir]... [--prelude file]... [--no-strict-borrowed]\n"
        "            [--format=std|fmt] [--view=hot|std] [--resource-dir dir] [--std=c++17|c++20|c++23] [--aot-optimized]\n"
        "            [-D<name>[=value]]... [-U<name>]... [--defines-file file] [--includes-file file]\n"
        "            [--scripts-file file] [--jobs N]\n"
        "       hotc --mask-embedded <in.rml> <prefix-file> -o <out.hot> [--tag script]\n"
        "  --defines-file <file> (--aot) one define for each line: NAME or NAME=value; -D and -U lines also work\n"
        "  --includes-file <file> (--aot) one include directory for each line, as -I\n"
        "  --scripts-file <file> (--aot) one script for each line: PATH or PATH<TAB>UNIT_NAME; not with --unit-name\n"
        "  --jobs <N>           (--aot) translate on N threads, 1..4096 (default: HOT_JOBS or min(cores, 16); 1 on Windows)\n"
        "  --unit-name <name>   (--aot, --emit-cpp) the unit name of the n-th script (default: the stem)\n"
        "  --mask-embedded      mask the <script> blocks of <in>, write <out.hot>; no block: no file, exit code 0\n"
        "Each mode also accepts:\n"
        "  --depfile <path>     write the headers and imported scripts that the translation read (Make format)\n"
        "  --target <triple>    the target triple of the parse and of the modules (-target)\n"
        "  --sysroot <dir>      the sysroot of the parse and of the modules (--sysroot=)\n"
        "  --host-implib <path> the import library of the host for the link of a module DLL (build mode)\n"
        "  --cache-dir <dir>    (build mode) reuse the object files of the directory; HOT_CACHE=0 turns it off\n");
    std::exit(2);
}

bool read_file(const std::string& path, std::string& text)
{
    std::ifstream f(path);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

struct Args
{
    bool emit_cpp = false;
    bool aot = false;
    bool aot_optimized = false; // --aot-optimized: needs --aot
    bool has_compiler_option = false; // --cxx, --cxxflag, --slots, --gen, -o
    std::vector<std::string> inputs;
    std::vector<std::string> unit_names; // --unit-name: one for each input, in the order of the inputs
    std::vector<std::string> input_units; // one for each input: the unit name from --scripts-file, or empty
    unsigned jobs = 0; // --jobs: the count of threads of --aot (0: HOT_JOBS or the default)
    bool has_scripts_file = false; // the flag --scripts-file is present
    std::string out_dir, output, slots, resource_dir;
    std::string target, sysroot; // --target, --sysroot: empty means the default of the build
    std::string host_implib; // --host-implib: the import library of the host (build mode)
    std::string cache_dir; // --cache-dir: the object cache (build mode)
    std::string std_version; // --std=: empty means the standard of the host
    std::vector<std::string> include_dirs, preludes, cxxflags;
    std::string cxx;
    int gen = 0;
    // Options that only one mode accepts.
    bool has_build_only = false; // --out-dir, --cxx, --cxxflag
    bool has_emit_only = false; // --prelude
    bool has_aot_only = false; // -D, -U, --defines-file, --includes-file
    std::vector<std::string> defines; // "-DNAME[=value]" and "-UNAME", in the order of the command line
    bool bad_list_file = false; // a --defines-file, --includes-file or --scripts-file that cannot be read
    std::string format_lib = "std"; // --format=std|fmt
    std::string view_type = hot::view_type_name(); // --view=hot|std (default: the view type of this build)
    bool view_given = false; // the flag --view is present
    bool format_given = false; // the flag --format is present
    bool strict_borrowed = true; // --no-strict-borrowed turns it off
    bool null_warnings = false; // --null-warnings: a nullability finding is a warning
    bool raw_diagnostics = false; // --raw-diagnostics: the compiler text as it is
    std::string compile_commands; // --compile-commands: the directory of compile_commands.json
    std::string depfile; // --depfile: the dependency file of the build system
};

// The lines of a list file without the empty lines and the line end characters. Gives false when
// the file cannot be read.
bool read_lines(const std::string& path, std::vector<std::string>& lines)
{
    std::string text;
    if (!read_file(path, text))
    {
        std::fprintf(stderr, "hotc: error: cannot read %s\n", path.c_str());
        return false;
    }
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
    }
    return true;
}

Args parse_args(int argc, char** argv)
{
    Args r;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string
        {
            if (i + 1 >= argc)
                usage();
            return argv[++i];
        };
        if (a == "--emit-cpp")
            r.emit_cpp = true;
        else if (a == "--aot")
            r.aot = true;
        else if (a == "--aot-optimized")
            r.aot_optimized = true;
        else if (a == "--gen")
        {
            r.gen = std::atoi(next().c_str());
            r.has_compiler_option = true;
        }
        else if (a == "-o")
        {
            r.output = next();
            r.has_compiler_option = true;
        }
        else if (a == "--slots")
        {
            r.slots = next();
            r.has_compiler_option = true;
        }
        else if (a == "--resource-dir")
            r.resource_dir = next();
        else if (a == "--depfile")
            r.depfile = next();
        else if (a == "--target")
            r.target = next();
        else if (a == "--sysroot")
            r.sysroot = next();
        else if (a == "--host-implib")
        {
            r.host_implib = next();
            r.has_build_only = true;
        }
        else if (a == "--cache-dir")
        {
            r.cache_dir = next();
            r.has_build_only = true;
        }
        else if (a == "--out-dir")
        {
            r.out_dir = next();
            r.has_build_only = true;
        }
        else if (a == "--cxx")
        {
            r.cxx = next();
            r.has_build_only = true;
            r.has_compiler_option = true;
        }
        else if (a == "--cxxflag")
        {
            r.cxxflags.push_back(next());
            r.has_build_only = true;
            r.has_compiler_option = true;
        }
        else if (a == "--prelude")
        {
            r.preludes.push_back(next());
            r.has_emit_only = true;
        }
        else if (a == "--unit-name")
        {
            r.unit_names.push_back(next());
            r.has_emit_only = true;
        }
        else if (a == "--strict-borrowed")
            r.strict_borrowed = true;
        else if (a == "--no-strict-borrowed")
            r.strict_borrowed = false;
        else if (a == "--null-warnings")
            r.null_warnings = true;
        else if (a == "--raw-diagnostics")
            r.raw_diagnostics = true;
        else if (a == "--compile-commands")
        {
            r.compile_commands = next();
            r.has_build_only = true;
        }
        else if (a == "--format=std")
        {
            r.format_lib = "std";
            r.format_given = true;
        }
        else if (a == "--format=fmt")
        {
            r.format_lib = "fmt";
            r.format_given = true;
        }
        else if (a == "--view=hot")
        {
            r.view_type = "hot";
            r.view_given = true;
        }
        else if (a == "--view=std")
        {
            r.view_type = "std";
            r.view_given = true;
        }
        else if (a == "--std=c++17" || a == "--std=c++20" || a == "--std=c++23")
            r.std_version = a.substr(6);
        else if (a == "-D" || a == "-U")
        {
            r.defines.push_back(a + next());
            r.has_aot_only = true;
        }
        else if (a.rfind("-D", 0) == 0 || a.rfind("-U", 0) == 0)
        {
            r.defines.push_back(a);
            r.has_aot_only = true;
        }
        else if (a == "--defines-file" || a == "--includes-file")
        {
            const std::string file = next();
            std::vector<std::string> lines;
            if (!read_lines(file, lines))
                r.bad_list_file = true;
            for (const std::string& l : lines)
            {
                if (a == "--includes-file")
                    r.include_dirs.push_back(l);
                else
                    r.defines.push_back(l.rfind("-D", 0) == 0 || l.rfind("-U", 0) == 0 ? l : "-D" + l);
            }
            r.has_aot_only = true;
        }
        else if (a == "--jobs")
        {
            const std::string v = next();
            char* end = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || n < 1 || n > 4096)
                usage();
            r.jobs = static_cast<unsigned>(n);
            r.has_aot_only = true;
        }
        else if (a == "--scripts-file")
        {
            const std::string file = next();
            std::vector<std::string> lines;
            if (!read_lines(file, lines))
                r.bad_list_file = true;
            for (const std::string& l : lines)
            {
                const size_t tab = l.find('\t');
                r.inputs.push_back(l.substr(0, tab));
                r.input_units.push_back(tab == std::string::npos ? std::string() : l.substr(tab + 1));
            }
            r.has_scripts_file = true;
            r.has_aot_only = true;
        }
        else if (a == "-I")
            r.include_dirs.push_back(next());
        else if (a.rfind("-I", 0) == 0)
            r.include_dirs.push_back(a.substr(2));
        else if (!a.empty() && a[0] == '-')
            usage();
        else
        {
            r.inputs.push_back(a);
            r.input_units.push_back(std::string());
        }
    }
    return r;
}

// Prints the errors and the warnings of the translator. The default form is the form of a
// rewritten compiler diagnostic. --raw-diagnostics prints the text as it is.
std::string diagnostics_text(const Args& args, const hotc::TranslateOptions& opt, const hotc::TranslateResult& r)
{
    std::string all;
    hotc::ScriptTexts scripts = hotc::script_texts(opt.script_name, opt.script_text, r);
    scripts.insert(scripts.end(), opt.modules.begin(), opt.modules.end());
    for (const std::string* text : {&r.diagnostics, &r.warnings})
    {
        const std::string out = args.raw_diagnostics ? *text : hotc::rewrite_translator_diagnostic(*text, scripts);
        all += out;
    }
    return all;
}

void print_diagnostics(const Args& args, const hotc::TranslateOptions& opt, const hotc::TranslateResult& r)
{
    std::fputs(diagnostics_text(args, opt, r).c_str(), stderr);
}

// The C++ identifier of a script name: letters, digits and "_".
std::string identifier(const std::string& stem)
{
    std::string id;
    for (char c : stem)
        id += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return id;
}

// Writes the file. With "only_if_changed" an existing file with the same content stays as it is,
// so its time stamp stays. With "err" an error text goes to "err" and not to stderr.
bool write_file(const std::string& path, const std::string& text, bool only_if_changed = false,
    std::string* err = nullptr)
{
    auto report = [&](const std::string& message)
    {
        if (err)
            *err += message;
        else
            std::fputs(message.c_str(), stderr);
    };
    if (only_if_changed)
    {
        std::string old;
        if (read_file(path, old) && old == text)
            return true;
    }
    // The text goes to "path.<pid>.tmp" first, then the rename replaces the file. A reader never sees half a file.
    const std::string tmp = path + "." + std::to_string(hot::detail::process_id()) + ".tmp";
    {
        std::ofstream of(tmp, std::ios::binary); // binary: LF line ends on each platform
        if (of)
            of << text;
        if (!of || (of.close(), of.fail()))
        {
            report("hotc: error: cannot write " + path + "\n");
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        report("hotc: error: cannot write " + path + ": " + ec.message() + "\n");
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// One rule of a dependency file: an output and the files that it depends on.
struct DepRule
{
    std::string target;
    std::vector<std::string> deps;
};

// A path in the Make format. A space and a '#' get a backslash. A '$' is doubled.
std::string depfile_path(const std::string& path)
{
    std::string r;
    for (char c : path)
    {
        if (c == ' ' || c == '#')
            r += '\\';
        else if (c == '$')
            r += '$';
        r += c;
    }
    return r;
}

// Writes the dependency file: one line for each rule. A dependency is an absolute path, so the
// working directory of the build system does not matter. All outputs of one call come from one
// command, and a change of any dependency must run it again. Some build systems (the CMake
// Makefile generators) read only the first rule. So each rule lists the dependencies of all rules.
bool write_depfile(const std::string& path, const std::vector<DepRule>& rules)
{
    std::vector<std::string> all;
    std::set<std::string> seen;
    for (const DepRule& rule : rules)
        for (const std::string& d : rule.deps)
        {
            // The file system resolves ".." here. A lexical form is wrong when a directory is a link.
            std::error_code ec;
            fs::path p = fs::weakly_canonical(fs::absolute(d, ec), ec);
            if (ec)
                p = fs::path(d);
            if (seen.insert(p.generic_string()).second)
                all.push_back(p.generic_string());
        }
    std::string text;
    for (const DepRule& rule : rules)
    {
        text += depfile_path(rule.target) + ":";
        for (const std::string& d : all)
            text += " " + depfile_path(d);
        text += "\n";
    }
    return write_file(path, text);
}

// Writes the text as a C++ string literal, one literal for each line (adjacent literals join).
std::string cpp_string_literal(const std::string& text)
{
    std::string out;
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t eol = text.find('\n', pos);
        const size_t end = eol == std::string::npos ? text.size() : eol + 1;
        out += "    \"";
        for (size_t i = pos; i < end; ++i)
        {
            const char c = text[i];
            if (c == '\n')
                out += "\\n";
            else if (c == '\\' || c == '"')
            {
                out += '\\';
                out += c;
            }
            else
                out += c;
        }
        out += "\"\n";
        pos = end;
    }
    return out.empty() ? "    \"\"\n" : out;
}

int run_aot(const Args& args)
{
    if (args.has_compiler_option || args.out_dir.empty())
        usage();
    if (args.bad_list_file)
        return 1;
    if (args.inputs.empty())
        usage();
    hotc::TranslateOptions base;
    base.generation = 1;
    base.aot = true;
    base.aot_optimized = args.aot_optimized;
    base.strict_borrowed = args.strict_borrowed;
    base.null_warnings = args.null_warnings;
    base.format_lib = args.format_lib;
    base.view_type = args.view_type;
    // Without --resource-dir the translator needs the default. A portable build finds it at run time.
    base.resource_dir = args.resource_dir.empty() ? hot::default_paths().resource_dir : args.resource_dir;
    base.target = args.target;
    base.sysroot = args.sysroot;
    base.std_version = args.std_version;
    base.include_dirs = args.include_dirs;
    base.defines = args.defines;
    for (const std::string& file : args.preludes)
    {
        std::string text;
        if (!read_file(file, text))
        {
            std::fprintf(stderr, "hotc: error: cannot read %s\n", file.c_str());
            return 1;
        }
        base.preludes.emplace_back(file, std::move(text));
    }
    if (args.has_scripts_file && !args.unit_names.empty())
    {
        std::fprintf(stderr, "hotc: error: --unit-name does not work with --scripts-file\n");
        return 1;
    }
    if (!args.unit_names.empty() && args.unit_names.size() != args.inputs.size())
    {
        std::fprintf(stderr, "hotc: error: give one --unit-name for each script or none\n");
        return 1;
    }
    // The unit name of each input. The C++ names of two units must differ, and so must the file
    // names of the outputs (the stems).
    std::vector<std::string> unit_names;
    std::set<std::string> ids, stems; // "stems" holds the lower-case stems
    auto fold = [](std::string text)
    {
        for (char& c : text)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return text;
    };
    for (size_t i = 0; i < args.inputs.size(); ++i)
    {
        const std::string stem = fs::path(args.inputs[i]).stem().string();
        if (!args.unit_names.empty())
            unit_names.push_back(args.unit_names[i]);
        else
            unit_names.push_back(args.input_units[i].empty() ? stem : args.input_units[i]);
        if (!ids.insert(identifier(unit_names.back())).second || !stems.insert(fold(stem)).second)
        {
            std::fprintf(stderr, "hotc: error: two scripts have the name %s\n", unit_names.back().c_str());
            return 1;
        }
    }
    std::error_code ec;
    fs::create_directories(args.out_dir, ec);

    hotc::TranslatorSession session;
    session.set_tu_cache_limit(0); // the batch parses each script once
    std::vector<std::string> names;
    // The interface hash and the imports of each script, in the order of "names".
    std::vector<std::pair<uint64_t, std::vector<std::pair<std::string, uint64_t>>>> units;
    std::vector<DepRule> dep_rules;
    std::vector<uint64_t> source_hashes; // the hash of the text of each script, in the order of "names"
    std::vector<hot::IfaceInfo> iface_infos; // the fine data of each script, in the order of "names"
    std::vector<hot::UseInfo> use_infos;
    std::string slots; // the manifests of all scripts: a class belongs to one script
    std::set<std::string> seen_slot_lines;
    bool failed = false;
    // The result of one script. A worker fills its own slot only.
    struct Slot
    {
        hotc::TranslateOptions opt;
        hotc::TranslateResult r;
        std::string out; // the path of the .aot.cpp file
        std::string diag; // the text for stderr: a read error or the diagnostics
        bool read_ok = false;
        bool write_failed = false;
        uint64_t source_hash = 0;
    };
    const size_t count = args.inputs.size();
    std::vector<Slot> results(count);
    std::vector<char> done(count, 0);
    std::mutex mutex;
    size_t printed = 0; // the scripts that have a progress line; guarded by "mutex"
    std::atomic<size_t> next_index{0};

    auto translate_one = [&](size_t input)
    {
        Slot& slot = results[input];
        const std::string& path = args.inputs[input];
        const std::string stem = fs::path(path).stem().string();
        const std::string id = identifier(unit_names[input]);
        slot.out = args.out_dir + "/" + stem + ".aot.cpp";
        hotc::TranslateOptions& opt = slot.opt;
        opt = base;
        opt.script_name = path;
        opt.unit_name = unit_names[input];
        opt.output_name = slot.out;
        if (!read_file(path, opt.script_text))
        {
            slot.diag = "hotc: error: cannot read " + path + "\n";
            return;
        }
        slot.read_ok = true;
        slot.r = hotc::translate(opt, session);
        slot.diag = diagnostics_text(args, opt, slot.r);
        if (!slot.r.ok)
            return;
        // Each script gets its own register function, so the scripts link into one program.
        std::string cpp = slot.r.cpp;
        const size_t eol = cpp.find('\n');
        cpp.insert(eol == std::string::npos ? cpp.size() : eol + 1,
            "#define script_register hot_script_register_" + id + "\n");
        cpp += "#undef script_register\n";
        if (!write_file(slot.out, cpp, true, &slot.diag))
            slot.write_failed = true;
        // The interface header of the script: the importers include it. The TU does not include it.
        if (!write_file(args.out_dir + "/" + stem + ".iface.h", slot.r.iface_header, true, &slot.diag))
            slot.write_failed = true;
        slot.source_hash = hot::hash_script_text(opt.script_text);
    };
    // Prints the progress line and the text of each finished script, in the order of the scripts.
    auto print_ready = [&]()
    {
        while (printed < count && done[printed])
        {
            std::fprintf(stderr, "[%zu/%zu] %s\n", printed + 1, count, unit_names[printed].c_str());
            std::fputs(results[printed].diag.c_str(), stderr);
            ++printed;
        }
        std::fflush(stderr);
    };
    auto work = [&]()
    {
        for (size_t input; (input = next_index.fetch_add(1)) < count;)
        {
            try
            {
                translate_one(input);
            }
            catch (const std::exception& e)
            {
                results[input].read_ok = false;
                results[input].diag += std::string("hotc: error: internal error: ") + e.what() + "\n";
            }
            catch (...)
            {
                results[input].read_ok = false;
                results[input].diag += "hotc: error: internal error\n";
            }
            std::lock_guard<std::mutex> lock(mutex);
            done[input] = 1;
            print_ready();
        }
    };
    {
        const unsigned jobs = args.jobs ? args.jobs : hot::build_jobs();
        const size_t threads = std::min<size_t>(jobs, count);
        // The guard joins all threads on each path.
        struct Pool
        {
            std::vector<std::thread> threads;
            ~Pool()
            {
                for (std::thread& t : threads)
                    if (t.joinable())
                        t.join();
            }
        } pool;
        for (size_t t = 1; t < threads; ++t)
        {
            try
            {
                pool.threads.emplace_back(work);
            }
            catch (const std::system_error&)
            {
                break; // the caller thread runs the remaining work
            }
        }
        work();
    }
    // The serial merge, in the order of the scripts.
    std::vector<std::string> kept_tables; // the units that keep the function table in optimized AOT
    for (size_t input = 0; input < count; ++input)
    {
        Slot& slot = results[input];
        if (!slot.read_ok || !slot.r.ok || slot.write_failed)
        {
            failed = true;
            continue;
        }
        const hotc::TranslateResult& r = slot.r;
        // An importer has the lines of the classes of its imports too. A line is in the manifest
        // once: a backend seeds its slot file with it and makes one enumerator for each line.
        {
            std::istringstream lines(r.slots_out + r.sigs_out);
            for (std::string line; std::getline(lines, line);)
                if (seen_slot_lines.insert(line).second)
                    slots += line + "\n";
        }
        if (r.keeps_fn_table)
            kept_tables.push_back(r.unit);
        names.push_back(r.unit);
        source_hashes.push_back(slot.source_hash);
        iface_infos.push_back(r.iface_info);
        use_infos.push_back(r.use_info);
        units.push_back({r.iface_hash, r.import_hashes});
        dep_rules.push_back({slot.out, r.dependencies});
    }
    if (failed)
        return 1;
    if (!kept_tables.empty())
    {
        std::string line = "hotc: optimized AOT keeps the function table of " + std::to_string(kept_tables.size()) + " unit(s): ";
        for (size_t i = 0; i < kept_tables.size(); ++i)
            line += (i ? ", " : "") + kept_tables[i];
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    // An import must be a script of the input list: the table has to hold its unit.
    for (size_t i = 0; i < names.size(); ++i)
        for (const auto& imp : units[i].second)
            if (std::find(names.begin(), names.end(), imp.first) == names.end())
            {
                std::fprintf(stderr, "hotc: error: the script %s imports the script %s, which is not in the input list\n",
                    names[i].c_str(), imp.first.c_str());
                failed = true;
            }
    if (failed)
        return 1;
    std::vector<std::string> prelude_texts;
    for (const auto& p : base.preludes)
        prelude_texts.push_back(p.second);
    const uint64_t env_hash = hot::aot_env_hash(prelude_texts, base.format_lib, base.view_type, base.std_version);

    std::string reg = "// Generated by hotc --aot. Do not edit.\n#include \"hot/aot.h\"\n\n";
    for (const std::string& n : names)
        reg += "extern \"C\" void hot_script_register_" + identifier(n) + "(hot::Registry&);\n";
    reg += "\nnamespace hot {\nnamespace {\n";
    // The imports of each script: the loader checks the interface hashes (Module::imports).
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (units[i].second.empty())
            continue;
        reg += "const AotImport kImports_" + identifier(names[i]) + "[] = {\n";
        for (const auto& [unit, hash] : units[i].second)
            reg += "    {\"" + unit + "\", " + std::to_string(hash) + "ull},\n";
        reg += "};\n";
    }
    // The fine interface data: constant arrays (AotIface, AotUse). Unknown data gives no table: the
    // field of AotScript stays null.
    auto u64_array = [&](const std::string& var, const std::vector<uint64_t>& v)
    {
        if (v.empty())
            return;
        reg += "const uint64_t " + var + "[] = {";
        size_t n = 0;
        for (uint64_t x : v)
            reg += std::to_string(x) + (++n % 64 == 0 ? "ull,\n" : "ull, ");
        reg += "};\n";
    };
    for (size_t i = 0; i < names.size(); ++i)
    {
        const std::string id = identifier(names[i]);
        const hot::IfaceInfo& f = iface_infos[i];
        if (f.known)
        {
            if (!f.members.empty())
            {
                reg += "const AotMember kMembers_" + id + "[] = {";
                size_t n = 0;
                for (const hot::IfaceMember& m : f.members)
                    reg += "{" + std::to_string(m.name) + "ull, " + std::to_string(m.decl) + "ull}"
                        + (++n % 64 == 0 ? ",\n" : ", ");
                reg += "};\n";
            }
            u64_array("kIfaceNames_" + id, f.names);
            u64_array("kCopiedNames_" + id, f.copied_names);
            const auto ptr = [&](const char* prefix, bool has)
            { return has ? std::string(prefix) + id : std::string("nullptr"); };
            reg += "const AotIface kIface_" + id + " = {" + std::to_string(f.shape) + "ull, "
                + ptr("kMembers_", !f.members.empty()) + ", " + std::to_string(f.members.size()) + ", "
                + ptr("kIfaceNames_", !f.names.empty()) + ", " + std::to_string(f.names.size()) + ", "
                + ptr("kCopiedNames_", !f.copied_names.empty()) + ", " + std::to_string(f.copied_names.size())
                + "};\n";
        }
        const hot::UseInfo& u = use_infos[i];
        if (u.known)
        {
            u64_array("kUseNames_" + id, u.names);
            if (!u.strong.empty())
            {
                reg += "const char* const kUseStrong_" + id + "[] = {";
                size_t n = 0;
                for (const std::string& s : u.strong)
                    reg += "\"" + s + "\"" + (++n % 64 == 0 ? ",\n" : ", ");
                reg += "};\n";
            }
            reg += "const AotUse kUse_" + id + " = {" + (u.names.empty() ? std::string("nullptr") : "kUseNames_" + id)
                + ", " + std::to_string(u.names.size()) + ", "
                + (u.strong.empty() ? std::string("nullptr") : "kUseStrong_" + id) + ", "
                + std::to_string(u.strong.size()) + "};\n";
        }
    }
    reg += "const AotScript kAotScripts[] = {\n";
    for (size_t i = 0; i < names.size(); ++i)
    {
        const std::string& n = names[i];
        const std::string id = identifier(n);
        reg += "    {\"" + n + "\", &hot_script_register_" + id + ", " + std::to_string(units[i].first) + "ull, "
            + (units[i].second.empty() ? std::string("nullptr, 0")
                                       : "kImports_" + id + ", " + std::to_string(units[i].second.size()))
            + ", " + std::to_string(source_hashes[i]) + "ull, "
            + (iface_infos[i].known ? "&kIface_" + id : std::string("nullptr")) + ", "
            + (use_infos[i].known ? "&kUse_" + id : std::string("nullptr")) + "},\n";
    }
    reg += "};\n\n// The slot manifest of all scripts. A run-time backend seeds its slot file with it.\n";
    // Optimized AOT code has no slots: the manifest is empty.
    reg += "const char kAotSlots[] =\n" + cpp_string_literal(args.aot_optimized ? std::string() : slots) + ";\n";
    reg += "}  // namespace\n\nextern const AotTable aot_table;\n";
    reg += "const AotTable aot_table = {kAotScripts, " + std::to_string(names.size()) + ", kAotSlots, "
        + (args.aot_optimized ? "true" : "false") + ", " + std::to_string(env_hash) + "ull};\n";
    reg += "}  // namespace hot\n";
    if (!write_file(args.out_dir + "/hot_aot_registry.cpp", reg, true))
        return 1;
    if (!args.depfile.empty() && !write_depfile(args.depfile, dep_rules))
        return 1;
    return 0;
}

// True when the two paths name the same existing file. A missing file is never the same file.
bool same_file(const std::string& a, const std::string& b)
{
    std::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec);
}

// Fails when the path "out" is the same file as one input. The tool must never overwrite a script.
bool check_not_input(const Args& args, const std::string& out)
{
    for (const std::string& in : args.inputs)
        if (same_file(in, out))
        {
            std::fprintf(stderr, "hotc: error: the output file %s is the input file %s\n", out.c_str(), in.c_str());
            return false;
        }
    return true;
}

int run_emit(const Args& args)
{
    if (args.has_build_only || args.has_aot_only || args.inputs.size() != 1 || args.gen < 1)
        usage();
    hotc::TranslateOptions opt;
    opt.generation = args.gen;
    opt.export_generation = true; // "--gen N" is explicit: the module names its generation
    opt.strict_borrowed = args.strict_borrowed;
    opt.null_warnings = args.null_warnings;
    opt.format_lib = args.format_lib;
    opt.view_type = args.view_type;
    opt.resource_dir = args.resource_dir.empty() ? hot::default_paths().resource_dir : args.resource_dir;
    opt.target = args.target;
    opt.sysroot = args.sysroot;
    opt.std_version = args.std_version;
    opt.include_dirs = args.include_dirs;
    for (const std::string& file : args.preludes)
    {
        std::string text;
        if (!read_file(file, text))
        {
            std::fprintf(stderr, "hotc: error: cannot read %s\n", file.c_str());
            return 1;
        }
        opt.preludes.emplace_back(file, std::move(text));
    }
    const std::string& input = args.inputs[0];
    opt.script_name = input;
    if (args.unit_names.size() > 1)
    {
        std::fprintf(stderr, "hotc: error: --emit-cpp takes one --unit-name\n");
        return 1;
    }
    if (!args.unit_names.empty())
        opt.unit_name = args.unit_names[0];
    opt.output_name = args.output; // empty: the default name
    if (!args.output.empty() && !check_not_input(args, args.output))
        return 1;
    if (!args.depfile.empty() && args.output.empty())
    {
        std::fprintf(stderr, "hotc: error: --depfile needs -o in the mode --emit-cpp\n");
        return 2;
    }
    if (!read_file(input, opt.script_text))
    {
        std::fprintf(stderr, "hotc: error: cannot read %s\n", input.c_str());
        return 1;
    }
    if (!args.slots.empty())
        read_file(args.slots, opt.slots_in); // a missing slot file is empty

    hotc::TranslateResult r = hotc::translate(opt);
    print_diagnostics(args, opt, r);
    if (!r.ok)
        return 1;

    if (args.output.empty())
    {
        std::fputs(r.cpp.c_str(), stdout);
    }
    else
    {
        std::ofstream of(args.output, std::ios::binary);
        if (!of)
        {
            std::fprintf(stderr, "hotc: error: cannot write %s\n", args.output.c_str());
            return 1;
        }
        of << r.cpp;
    }
    if (!args.slots.empty())
    {
        std::ofstream mf(args.slots, std::ios::binary);
        if (!mf)
        {
            std::fprintf(stderr, "hotc: error: cannot write %s\n", args.slots.c_str());
            return 1;
        }
        mf << r.slots_out << r.sigs_out;
    }
    const std::vector<DepRule> dep_rules{{args.output, r.dependencies}};
    if (!args.depfile.empty() && !write_depfile(args.depfile, dep_rules))
        return 1;
    return 0;
}

int run_build(const Args& args)
{
    const bool single_so = !args.output.empty();
    if (args.has_emit_only || args.has_aot_only || args.inputs.empty() || args.gen < 1)
        usage();
    if (single_so == !args.out_dir.empty())
        usage(); // exactly one of -o and --out-dir
    if (single_so && args.inputs.size() != 1)
        usage();

    // Build mode uses the format library that the build of hotc selected (include/hot/config.h).
    // The flag --format must agree with it. This catches a build system that mixes two choices.
    if (args.format_given && args.format_lib != hot::format_lib_name())
    {
        std::fprintf(stderr, "hotc: error: this hotc is built for --format=%s, not --format=%s\n",
            hot::format_lib_name(), args.format_lib.c_str());
        return 2;
    }

    // The same rule holds for the view type of "T[]" (--view=hot|std).
    if (args.view_given && args.view_type != hot::view_type_name())
    {
        std::fprintf(stderr, "hotc: error: this hotc is built for --view=%s, not --view=%s\n", hot::view_type_name(),
            args.view_type.c_str());
        return 2;
    }

    hot::ClangFilesOptions opt;
    opt.slots_file = args.slots;
    opt.resource_dir = args.resource_dir;
    opt.target_triple = args.target;
    opt.sysroot = args.sysroot;
    opt.host_import_library = args.host_implib;
    opt.std_version = args.std_version;
    if (!args.cxx.empty())
        opt.compiler = args.cxx;
    opt.extra_flags = args.cxxflags;
    opt.include_dirs = args.include_dirs;
    opt.strict_borrowed = args.strict_borrowed;
    opt.null_warnings = args.null_warnings;
    opt.raw_diagnostics = args.raw_diagnostics;
    std::string out_dir = args.out_dir;
    if (single_so)
    {
        out_dir = fs::path(args.output).parent_path().string();
        if (out_dir.empty())
            out_dir = ".";
    }
    opt.work_dir = out_dir;
    opt.cache_dir = args.cache_dir;

    std::set<std::string> seen;
    for (const std::string& s : args.inputs)
    {
        if (!seen.insert(fs::path(s).stem().string()).second)
        {
            std::fprintf(stderr, "hotc: error: two scripts have the name %s\n", fs::path(s).stem().string().c_str());
            return 1;
        }
    }

    // The tool writes the .so and the .cpp, .o and .d next to it. None may be an input file.
    for (const std::string& s : args.inputs)
    {
        const std::string so = single_so
            ? args.output
            : out_dir + "/" + fs::path(s).stem().string() + ".g" + std::to_string(args.gen) + hot::module_suffix();
        for (const char* ext : {"", ".cpp", ".o", ".d"})
            if (!check_not_input(args, *ext ? fs::path(so).replace_extension(ext).string() : so))
                return 1;
        // The cache compiles a private copy "<out_dir>/<unit>.cpp".
        if (!args.cache_dir.empty() && !check_not_input(args, out_dir + "/" + fs::path(s).stem().string() + ".cpp"))
            return 1;
    }

    hot::ClangFilesBackend backend(opt);
    std::vector<hot::CompileCommandEntry> db;
    std::vector<DepRule> dep_rules;
    bool failed = false;
    for (const std::string& path : args.inputs)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "hotc: error: cannot read %s\n", path.c_str());
            failed = true;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string stem = fs::path(path).stem().string();
        const std::string so =
            single_so ? args.output : out_dir + "/" + stem + ".g" + std::to_string(args.gen) + hot::module_suffix();
        std::string error;
        std::vector<std::string> deps;
        const bool built = backend.build({path, ss.str(), {}}, static_cast<uint32_t>(args.gen), so, error, &deps);
        if (!built)
        {
            std::fputs(error.c_str(), stderr);
            failed = true;
        }
        else
            dep_rules.push_back({so, std::move(deps)});
        // The .cpp exists after a failed compile too. A translation error leaves no .cpp.
        const fs::path cpp = fs::path(so).replace_extension(".cpp");
        if (!args.compile_commands.empty() && fs::exists(cpp))
        {
            const std::string abs_cpp = fs::absolute(cpp).string();
            const std::string abs_obj = fs::absolute(fs::path(so).replace_extension(".o")).string();
            const std::string abs_dep = fs::absolute(fs::path(so).replace_extension(".d")).string();
            db.push_back({fs::current_path().string(), abs_cpp,
                hot::compile_object_command(opt, abs_cpp, abs_obj, abs_dep), abs_obj});
        }
    }
    if (!args.compile_commands.empty())
    {
        std::error_code ec;
        fs::create_directories(args.compile_commands, ec);
        if (!write_file(args.compile_commands + "/compile_commands.json", hot::compile_commands_json(db)))
            failed = true;
    }
    if (!failed && !args.depfile.empty() && !write_depfile(args.depfile, dep_rules))
        failed = true;
    return failed ? 1 : 0;
}

// Reads a whole file as bytes. A false result means a read error.
bool read_bytes(const std::string& path, std::string& text)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad())
        return false;
    text = ss.str();
    return true;
}

// The mode --mask-embedded. The arguments after the flag are: <in> <prefix-file> -o <out> [--tag T].
int run_mask(int argc, char** argv)
{
    std::vector<std::string> positional;
    std::string out, tag = "script";
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--mask-embedded")
            continue;
        if (a == "-o" || a == "--tag")
        {
            if (i + 1 >= argc)
                usage();
            (a == "-o" ? out : tag) = argv[++i];
        }
        else if (!a.empty() && a[0] == '-')
            usage();
        else
            positional.push_back(a);
    }
    if (positional.size() != 2 || out.empty() || tag.empty())
        usage();
    std::error_code same_ec;
    for (const std::string& in : positional)
    {
        // A missing output file gives an error code: it is not the same file.
        if (fs::equivalent(fs::path(out), fs::path(in), same_ec) && !same_ec)
        {
            std::fprintf(stderr, "hotc: error: output path is the same as an input file\n");
            return 2;
        }
    }
    std::string text, prefix;
    if (!read_bytes(positional[0], text))
    {
        std::fprintf(stderr, "hotc: error: cannot read %s\n", positional[0].c_str());
        return 1;
    }
    if (!read_bytes(positional[1], prefix))
    {
        std::fprintf(stderr, "hotc: error: cannot read %s\n", positional[1].c_str());
        return 1;
    }
    if (!prefix.empty() && prefix.back() == '\n')
    {
        prefix.pop_back();
        if (!prefix.empty() && prefix.back() == '\r')
            prefix.pop_back();
    }
    bool found = false;
    const std::string masked = hot::mask_embedded_scripts(text, tag, &found, prefix);
    std::error_code ec;
    if (!found)
    {
        fs::remove(fs::path(out), ec); // a stale output must not stay
        if (ec)
        {
            std::fprintf(stderr, "hotc: error: cannot remove %s: %s\n", out.c_str(), ec.message().c_str());
            return 1;
        }
        std::printf("hotc: no script block in %s\n", positional[0].c_str());
        return 0;
    }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(masked.data(), static_cast<std::streamsize>(masked.size()));
    f.close();
    if (!f)
    {
        std::fprintf(stderr, "hotc: error: cannot write %s\n", out.c_str());
        fs::remove(fs::path(out), ec); // no partial output; the error is ignored
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--mask-embedded")
            return run_mask(argc, argv);
    const Args args = parse_args(argc, argv);
    if (args.aot && args.emit_cpp)
        usage();
    if (args.aot_optimized && !args.aot)
    {
        std::fprintf(stderr, "hotc: --aot-optimized needs --aot\n");
        usage();
    }
    if (args.aot)
        return run_aot(args);
    return args.emit_cpp ? run_emit(args) : run_build(args);
}
