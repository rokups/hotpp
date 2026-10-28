// sppc: the command line tool of S++. It has two modes.
//
// Build mode (default): compile .spp files ahead of time into one shared library for each file.
//   sppc <file.spp>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]
//        [--cxx command] [--cxxflag flag]... [--slots file]
//   sppc <file.spp> --gen N -o x.so [-I dir]... [--resource-dir dir]
//        [--cxx command] [--cxxflag flag]... [--slots file]
// For each file the tool writes DIR/<name>.g<N>.cpp and DIR/<name>.g<N>.so.
// <name> is the file name without the extension. With -o, the tool writes the one
// library to x.so and the C++ file to the directory of x.so.
// A program loads the .so with spp::module_from_library and gives the Module to the Loader.
// The slot file (default DIR/slots.txt) keeps the slot order between generations.
// Build the generations of one class in order: 1, then 2, and so on.
// The tool needs no CMake.
//
// Emit mode (--emit-cpp): translate one .spp file to C++. The tool does not compile it.
//   sppc --emit-cpp <file.spp> --gen N [-o out.cpp] [-I dir]... [--prelude file]...
//        [--slots file] [--resource-dir dir]
// Without -o, the tool prints the C++ to the standard output.
//
// Exit code: 0 success, 1 error, 2 bad usage.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "spp/clang_files_backend.h"
#include "spp/sppc_lib.h"

namespace fs = std::filesystem;

namespace {

[[noreturn]] void usage()
{
    std::fprintf(stderr,
                 "usage: sppc <file.spp>... --gen N (--out-dir DIR | -o x.so) [-I dir]... [--resource-dir dir]\n"
                 "            [--cxx command] [--cxxflag flag]... [--slots file] [--no-strict-borrowed] [--format=std|fmt]\n"
                 "       sppc --emit-cpp <file.spp> --gen N [-o out.cpp] [-I dir]... [--prelude file]... [--no-strict-borrowed] [--format=std|fmt]\n"
                 "            [--slots file] [--resource-dir dir]\n");
    std::exit(2);
}

bool read_file(const std::string& path, std::string& text)
{
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

struct Args {
    bool emit_cpp = false;
    std::vector<std::string> inputs;
    std::string out_dir, output, slots, resource_dir;
    std::vector<std::string> include_dirs, preludes, cxxflags;
    std::string cxx;
    int gen = 0;
    // Options that only one mode accepts.
    bool has_build_only = false;  // --out-dir, --cxx, --cxxflag
    bool has_emit_only = false;   // --prelude
    std::string format_lib = "std";  // --format=std|fmt
    bool strict_borrowed = true;  // --no-strict-borrowed turns it off
};

Args parse_args(int argc, char** argv)
{
    Args r;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--emit-cpp") r.emit_cpp = true;
        else if (a == "--gen") r.gen = std::atoi(next().c_str());
        else if (a == "-o") r.output = next();
        else if (a == "--slots") r.slots = next();
        else if (a == "--resource-dir") r.resource_dir = next();
        else if (a == "--out-dir") { r.out_dir = next(); r.has_build_only = true; }
        else if (a == "--cxx") { r.cxx = next(); r.has_build_only = true; }
        else if (a == "--cxxflag") { r.cxxflags.push_back(next()); r.has_build_only = true; }
        else if (a == "--prelude") { r.preludes.push_back(next()); r.has_emit_only = true; }
        else if (a == "--strict-borrowed") r.strict_borrowed = true;
        else if (a == "--no-strict-borrowed") r.strict_borrowed = false;
        else if (a == "--format=std") r.format_lib = "std";
        else if (a == "--format=fmt") r.format_lib = "fmt";
        else if (a == "-I") r.include_dirs.push_back(next());
        else if (a.rfind("-I", 0) == 0) r.include_dirs.push_back(a.substr(2));
        else if (!a.empty() && a[0] == '-') usage();
        else r.inputs.push_back(a);
    }
    return r;
}

int run_emit(const Args& args)
{
    if (args.has_build_only || args.inputs.size() != 1 || args.gen < 1) usage();
    sppc::TranslateOptions opt;
    opt.generation = args.gen;
    opt.strict_borrowed = args.strict_borrowed;
    opt.format_lib = args.format_lib;
    opt.resource_dir = args.resource_dir;
    opt.include_dirs = args.include_dirs;
    for (const std::string& file : args.preludes) {
        std::string text;
        if (!read_file(file, text)) {
            std::fprintf(stderr, "sppc: error: cannot read %s\n", file.c_str());
            return 1;
        }
        opt.preludes.emplace_back(file, std::move(text));
    }
    const std::string& input = args.inputs[0];
    opt.script_name = input;
    opt.output_name = args.output;  // empty: the default name
    if (!read_file(input, opt.script_text)) {
        std::fprintf(stderr, "sppc: error: cannot read %s\n", input.c_str());
        return 1;
    }
    if (!args.slots.empty()) read_file(args.slots, opt.slots_in);  // a missing slot file is empty

    sppc::TranslateResult r = sppc::translate(opt);
    std::fputs(r.diagnostics.c_str(), stderr);
    std::fputs(r.warnings.c_str(), stderr);
    if (!r.ok) return 1;

    if (args.output.empty()) {
        std::fputs(r.cpp.c_str(), stdout);
    } else {
        std::ofstream of(args.output);
        if (!of) {
            std::fprintf(stderr, "sppc: error: cannot write %s\n", args.output.c_str());
            return 1;
        }
        of << r.cpp;
    }
    if (!args.slots.empty()) {
        std::ofstream mf(args.slots);
        if (!mf) {
            std::fprintf(stderr, "sppc: error: cannot write %s\n", args.slots.c_str());
            return 1;
        }
        mf << r.slots_out;
    }
    return 0;
}

int run_build(const Args& args)
{
    const bool single_so = !args.output.empty();
    if (args.has_emit_only || args.inputs.empty() || args.gen < 1) usage();
    if (single_so == !args.out_dir.empty()) usage();  // exactly one of -o and --out-dir
    if (single_so && args.inputs.size() != 1) usage();

    spp::ClangFilesOptions opt;
    opt.slots_file = args.slots;
    opt.resource_dir = args.resource_dir;
    if (!args.cxx.empty()) opt.compiler = args.cxx;
    opt.extra_flags = args.cxxflags;
    opt.include_dirs = args.include_dirs;
    opt.strict_borrowed = args.strict_borrowed;
    opt.format_lib = args.format_lib;
    std::string out_dir = args.out_dir;
    if (single_so) {
        out_dir = fs::path(args.output).parent_path().string();
        if (out_dir.empty()) out_dir = ".";
    }
    opt.work_dir = out_dir;

    std::set<std::string> seen;
    for (const std::string& s : args.inputs) {
        if (!seen.insert(fs::path(s).stem().string()).second) {
            std::fprintf(stderr, "sppc: error: two scripts have the name %s\n",
                         fs::path(s).stem().string().c_str());
            return 1;
        }
    }

    spp::ClangFilesBackend backend(opt);
    bool failed = false;
    for (const std::string& path : args.inputs) {
        std::ifstream in(path);
        if (!in) {
            std::fprintf(stderr, "sppc: error: cannot read %s\n", path.c_str());
            failed = true;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string stem = fs::path(path).stem().string();
        const std::string so = single_so ? args.output
                                         : out_dir + "/" + stem + ".g" + std::to_string(args.gen) + ".so";
        std::string error;
        if (!backend.build({path, ss.str()}, static_cast<uint32_t>(args.gen), so, error)) {
            std::fputs(error.c_str(), stderr);
            failed = true;
        }
    }
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const Args args = parse_args(argc, argv);
    return args.emit_cpp ? run_emit(args) : run_build(args);
}
