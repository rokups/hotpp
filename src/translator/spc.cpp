// spc: command line wrapper of spc_lib. It reads and writes the files.
//
// Usage: spc player.script --gen 2 -o out.cpp [-I dir]... [--slots file] [--resource-dir dir]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "spp/spc_lib.h"

namespace {

[[noreturn]] void usage()
{
    std::fprintf(stderr,
                 "usage: spc <file.script> --gen N -o <out.cpp> [-I dir]... [--slots file] [--resource-dir dir]\n");
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

}  // namespace

int main(int argc, char** argv)
{
    spc::TranslateOptions opt;
    std::string input, output, slots;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--gen") opt.generation = std::atoi(next().c_str());
        else if (a == "-o") output = next();
        else if (a == "--slots") slots = next();
        else if (a == "--resource-dir") opt.resource_dir = next();
        else if (a == "-I") opt.include_dirs.push_back(next());
        else if (a.rfind("-I", 0) == 0) opt.include_dirs.push_back(a.substr(2));
        else if (!a.empty() && a[0] == '-') usage();
        else if (input.empty()) input = a;
        else usage();
    }
    if (input.empty() || output.empty() || opt.generation < 1) usage();
    opt.script_name = input;
    opt.output_name = output;
    if (!read_file(input, opt.script_text)) {
        std::fprintf(stderr, "spc: error: cannot read %s\n", input.c_str());
        return 1;
    }
    if (!slots.empty()) read_file(slots, opt.slots_in);  // a missing slot file is empty

    spc::TranslateResult r = spc::translate(opt);
    std::fputs(r.diagnostics.c_str(), stderr);
    if (!r.ok) return 1;

    std::ofstream of(output);
    if (!of) {
        std::fprintf(stderr, "spc: error: cannot write %s\n", output.c_str());
        return 1;
    }
    of << r.cpp;
    if (!slots.empty()) {
        std::ofstream mf(slots);
        if (!mf) {
            std::fprintf(stderr, "spc: error: cannot write %s\n", slots.c_str());
            return 1;
        }
        mf << r.slots_out;
    }
    return 0;
}
