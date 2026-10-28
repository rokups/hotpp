// sppc: compile scripts ahead of time into one shared library for each script.
//
// Usage:
//   sppc <script>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]
//        [--cxx command] [--cxxflag flag]... [--slots file]
//
// For each script the tool writes DIR/<name>.g<N>.cpp and DIR/<name>.g<N>.so.
// <name> is the script file name without the extension. A program loads the
// .so with script::module_from_library and gives the Module to the Loader.
// The slot file (default DIR/slots.txt) keeps the slot order between generations.
// Build the generations of one class in order: 1, then 2, and so on.
// The tool needs no CMake. Exit code: 0 success, 1 error, 2 bad usage.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "spp/clang_files_backend.h"

namespace fs = std::filesystem;

namespace {

[[noreturn]] void usage()
{
    std::fprintf(stderr,
                 "usage: sppc <script>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]\n"
                 "            [--cxx command] [--cxxflag flag]... [--slots file]\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv)
{
    script::ClangFilesOptions opt;
    std::vector<std::string> scripts;
    std::string out_dir;
    int gen = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--gen") gen = std::atoi(next().c_str());
        else if (a == "--out-dir") out_dir = next();
        else if (a == "--slots") opt.slots_file = next();
        else if (a == "--resource-dir") opt.resource_dir = next();
        else if (a == "--cxx") opt.compiler = next();
        else if (a == "--cxxflag") opt.extra_flags.push_back(next());
        else if (a == "-I") opt.include_dirs.push_back(next());
        else if (a.rfind("-I", 0) == 0) opt.include_dirs.push_back(a.substr(2));
        else if (!a.empty() && a[0] == '-') usage();
        else scripts.push_back(a);
    }
    if (scripts.empty() || out_dir.empty() || gen < 1) usage();
    opt.work_dir = out_dir;

    std::set<std::string> seen;
    for (const std::string& s : scripts) {
        if (!seen.insert(fs::path(s).stem().string()).second) {
            std::fprintf(stderr, "sppc: error: two scripts have the name %s\n",
                         fs::path(s).stem().string().c_str());
            return 1;
        }
    }

    script::ClangFilesBackend backend(opt);
    bool failed = false;
    for (const std::string& path : scripts) {
        std::ifstream in(path);
        if (!in) {
            std::fprintf(stderr, "sppc: error: cannot read %s\n", path.c_str());
            failed = true;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string stem = fs::path(path).stem().string();
        const std::string so = out_dir + "/" + stem + ".g" + std::to_string(gen) + ".so";
        std::string error;
        if (!backend.build({path, ss.str()}, static_cast<uint32_t>(gen), so, error)) {
            std::fputs(error.c_str(), stderr);
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
