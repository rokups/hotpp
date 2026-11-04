// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hotc: the command line tool of Hot++. It has two modes.
//
// Build mode (default): compile .hot files ahead of time into one shared library for each file.
//   hotc <file.hot>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]
//        [--cxx command] [--cxxflag flag]... [--slots file] [--raw-diagnostics]
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
//        [--no-strict-borrowed] [--null-warnings] [--format=std|fmt] [--resource-dir dir] [--aot-optimized]
// The flag --aot-optimized builds optimized AOT code. It needs --aot.
// For each file the tool writes DIR/<name>.aot.cpp (generation 1). It also writes
// DIR/hot_aot_registry.cpp, which defines the constant table hot::aot_table
// (include/hot/aot.h). The tool does not compile. The CMake function
// hot_add_scripts with AOT builds a static library from these files. The program links the
// library and hot_aot. It has no loader.
//
// The flag --format=std|fmt is a flag for build systems. In build mode the build of hotc fixes
// the format library (include/hot/config.h), and a different --format is an error (exit code 2).
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
// Exit code: 0 success, 1 error, 2 bad usage.
#include "hot/clang_files_backend.h"
#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

[[noreturn]] void usage()
{
    std::fprintf(stderr,
        "usage: hotc <file.hot>... --gen N (--out-dir DIR | -o x.so) [-I dir]... [--resource-dir dir]\n"
        "            [--cxx command] [--cxxflag flag]... [--slots file] [--no-strict-borrowed] [--null-warnings] [--format=std|fmt]\n"
        "            [--raw-diagnostics] [--compile-commands dir] [--std=c++17|c++20|c++23]\n"
        "       hotc --emit-cpp <file.hot> --gen N [-o out.cpp] [-I dir]... [--prelude file]... [--no-strict-borrowed] "
        "[--format=std|fmt]\n"
        "            [--slots file] [--resource-dir dir] [--std=c++17|c++20|c++23]\n"
        "       hotc --aot <file.hot>... --out-dir DIR [-I dir]... [--prelude file]... [--no-strict-borrowed]\n"
        "            [--format=std|fmt] [--resource-dir dir] [--std=c++17|c++20|c++23] [--aot-optimized]\n"
        "Each mode also accepts:\n"
        "  --depfile <path>     write the headers and imported scripts that the translation read (Make format)\n"
        "  --target <triple>    the target triple of the parse and of the modules (-target)\n"
        "  --sysroot <dir>      the sysroot of the parse and of the modules (--sysroot=)\n"
        "  --host-implib <path> the import library of the host for the link of a module DLL (build mode)\n");
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
    std::string out_dir, output, slots, resource_dir;
    std::string target, sysroot; // --target, --sysroot: empty means the default of the build
    std::string host_implib; // --host-implib: the import library of the host (build mode)
    std::string std_version; // --std=: empty means the standard of the host
    std::vector<std::string> include_dirs, preludes, cxxflags;
    std::string cxx;
    int gen = 0;
    // Options that only one mode accepts.
    bool has_build_only = false; // --out-dir, --cxx, --cxxflag
    bool has_emit_only = false; // --prelude
    std::string format_lib = "std"; // --format=std|fmt
    bool format_given = false; // the flag --format is present
    bool strict_borrowed = true; // --no-strict-borrowed turns it off
    bool null_warnings = false; // --null-warnings: a nullability finding is a warning
    bool raw_diagnostics = false; // --raw-diagnostics: the compiler text as it is
    std::string compile_commands; // --compile-commands: the directory of compile_commands.json
    std::string depfile; // --depfile: the dependency file of the build system
};

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
        else if (a == "--std=c++17" || a == "--std=c++20" || a == "--std=c++23")
            r.std_version = a.substr(6);
        else if (a == "-I")
            r.include_dirs.push_back(next());
        else if (a.rfind("-I", 0) == 0)
            r.include_dirs.push_back(a.substr(2));
        else if (!a.empty() && a[0] == '-')
            usage();
        else
            r.inputs.push_back(a);
    }
    return r;
}

// Prints the errors and the warnings of the translator. The default form is the form of a
// rewritten compiler diagnostic. --raw-diagnostics prints the text as it is.
void print_diagnostics(const Args& args, const hotc::TranslateOptions& opt, const hotc::TranslateResult& r)
{
    hotc::ScriptTexts scripts = hotc::script_texts(opt.script_name, opt.script_text, r);
    scripts.insert(scripts.end(), opt.modules.begin(), opt.modules.end());
    for (const std::string* text : {&r.diagnostics, &r.warnings})
    {
        const std::string out = args.raw_diagnostics ? *text : hotc::rewrite_translator_diagnostic(*text, scripts);
        std::fputs(out.c_str(), stderr);
    }
}

// The C++ identifier of a script name: letters, digits and "_".
std::string identifier(const std::string& stem)
{
    std::string id;
    for (char c : stem)
        id += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return id;
}

bool write_file(const std::string& path, const std::string& text)
{
    std::ofstream of(path, std::ios::binary); // binary: LF line ends on each platform
    if (!of)
    {
        std::fprintf(stderr, "hotc: error: cannot write %s\n", path.c_str());
        return false;
    }
    of << text;
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
    if (args.has_compiler_option || args.out_dir.empty() || args.inputs.empty())
        usage();
    hotc::TranslateOptions base;
    base.generation = 1;
    base.aot = true;
    base.aot_optimized = args.aot_optimized;
    base.strict_borrowed = args.strict_borrowed;
    base.null_warnings = args.null_warnings;
    base.format_lib = args.format_lib;
    // Without --resource-dir the translator needs the default. A portable build finds it at run time.
    base.resource_dir = args.resource_dir.empty() ? hot::default_paths().resource_dir : args.resource_dir;
    base.target = args.target;
    base.sysroot = args.sysroot;
    base.std_version = args.std_version;
    base.include_dirs = args.include_dirs;
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
    std::set<std::string> ids;
    for (const std::string& s : args.inputs)
    {
        const std::string stem = fs::path(s).stem().string();
        if (!ids.insert(identifier(stem)).second)
        {
            std::fprintf(stderr, "hotc: error: two scripts have the name %s\n", stem.c_str());
            return 1;
        }
    }
    std::error_code ec;
    fs::create_directories(args.out_dir, ec);

    hotc::TranslatorSession session;
    std::vector<std::string> names;
    std::vector<DepRule> dep_rules;
    std::string slots; // the manifests of all scripts: a class belongs to one script
    bool failed = false;
    for (const std::string& path : args.inputs)
    {
        const std::string stem = fs::path(path).stem().string();
        const std::string id = identifier(stem);
        const std::string out = args.out_dir + "/" + stem + ".aot.cpp";
        hotc::TranslateOptions opt = base;
        opt.script_name = path;
        opt.output_name = out;
        if (!read_file(path, opt.script_text))
        {
            std::fprintf(stderr, "hotc: error: cannot read %s\n", path.c_str());
            failed = true;
            continue;
        }
        hotc::TranslateResult r = hotc::translate(opt, session);
        print_diagnostics(args, opt, r);
        if (!r.ok)
        {
            failed = true;
            continue;
        }
        // Each script gets its own register function, so the scripts link into one program.
        std::string cpp = r.cpp;
        const size_t eol = cpp.find('\n');
        cpp.insert(eol == std::string::npos ? cpp.size() : eol + 1,
            "#define script_register hot_script_register_" + id + "\n");
        cpp += "#undef script_register\n";
        if (!write_file(out, cpp))
            failed = true;
        slots += r.slots_out + r.sigs_out;
        names.push_back(stem);
        dep_rules.push_back({out, r.dependencies});
    }
    if (failed)
        return 1;

    std::string reg = "// Generated by hotc --aot. Do not edit.\n#include \"hot/aot.h\"\n\n";
    for (const std::string& n : names)
        reg += "extern \"C\" void hot_script_register_" + identifier(n) + "(hot::Registry&);\n";
    reg += "\nnamespace hot {\nnamespace {\nconst AotScript kAotScripts[] = {\n";
    for (const std::string& n : names)
        reg += "    {\"" + n + "\", &hot_script_register_" + identifier(n) + "},\n";
    reg += "};\n\n// The slot manifest of all scripts. A run-time backend seeds its slot file with it.\n";
    // Optimized AOT code has no slots: the manifest is empty.
    reg += "const char kAotSlots[] =\n" + cpp_string_literal(args.aot_optimized ? std::string() : slots) + ";\n";
    reg += "}  // namespace\n\nextern const AotTable aot_table;\n";
    reg += "const AotTable aot_table = {kAotScripts, " + std::to_string(names.size()) + ", kAotSlots"
        + (args.aot_optimized ? ", true" : "") + "};\n";
    reg += "}  // namespace hot\n";
    if (!write_file(args.out_dir + "/hot_aot_registry.cpp", reg))
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
    if (args.has_build_only || args.inputs.size() != 1 || args.gen < 1)
        usage();
    hotc::TranslateOptions opt;
    opt.generation = args.gen;
    opt.strict_borrowed = args.strict_borrowed;
    opt.null_warnings = args.null_warnings;
    opt.format_lib = args.format_lib;
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
    if (!args.depfile.empty() && !write_depfile(args.depfile, {{args.output, r.dependencies}}))
        return 1;
    return 0;
}

int run_build(const Args& args)
{
    const bool single_so = !args.output.empty();
    if (args.has_emit_only || args.inputs.empty() || args.gen < 1)
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

    std::set<std::string> seen;
    for (const std::string& s : args.inputs)
    {
        if (!seen.insert(fs::path(s).stem().string()).second)
        {
            std::fprintf(stderr, "hotc: error: two scripts have the name %s\n", fs::path(s).stem().string().c_str());
            return 1;
        }
    }

    // The tool writes the .so and the .cpp next to it. Neither may be an input file.
    for (const std::string& s : args.inputs)
    {
        const std::string so = single_so
            ? args.output
            : out_dir + "/" + fs::path(s).stem().string() + ".g" + std::to_string(args.gen) + hot::module_suffix();
        if (!check_not_input(args, so) || !check_not_input(args, fs::path(so).replace_extension(".cpp").string()))
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
            const std::string abs_so = fs::absolute(so).string();
            db.push_back({fs::current_path().string(), abs_cpp, hot::compile_command(opt, abs_cpp, abs_so), abs_so});
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

} // namespace

int main(int argc, char** argv)
{
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
