// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A sample runner: sample_runner [-I dir]... [--backend=files|jit] file.hot [args...]
// It loads the script and calls its "main". A real project builds its own runner with its engine.
#include "hot/launcher.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    hot::launcher::Options options;
    std::string script;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (!script.empty())
            args.push_back(a);
        else if (a == "-I" && i + 1 < argc)
            options.include_dirs.push_back(argv[++i]);
        else if (a.rfind("-I", 0) == 0 && a.size() > 2)
            options.include_dirs.push_back(a.substr(2));
        else if (a.rfind("--backend=", 0) == 0)
            options.backend = a.substr(10);
        else
            script = a;
    }
    if (script.empty())
    {
        std::fprintf(stderr, "usage: sample_runner [-I dir]... [--backend=files|jit] file.hot [args...]\n");
        return 64;
    }
    std::string error;
    const auto program = hot::launcher::load(options, script, error);
    if (!program)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return hot::launcher::run_main(*program, args);
}
