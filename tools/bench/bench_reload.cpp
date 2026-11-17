// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// bench_reload: measure the warm reload after a body edit of one script, in the clang files backend.
//
// The tool loads one script, then edits one number in the text and reloads it, many times. The
// native headers of the script can be heavy (the rbfx headers). The compile flags come from a file,
// one flag in each line (tools/bench/bench_reload.py makes it from a compile_commands.json).
//
// Usage:
//   bench_reload --script FILE [--flags FILE] [--include DIR]... [--preload LIB]... [--prelude FILE]... [--reps N]
//                [--work DIR] [--cache DIR] [--marker TEXT] [--no-swap]
//
//   --script   the script to reload (.hot or .cpp). A "using" line is read from the directory of the
//              script and from --include.
//   --flags    the compile flags of the script, one in each line. A "-I" flag is an include directory.
//   --include  an include directory for the imports and the headers.
//   --preload  a shared library that the tool opens with RTLD_GLOBAL before the first load, so the
//              module finds the native symbols (the engine library).
//   --prelude  a prelude file of the host (stub-only script), read before the script. Repeat it.
//   --reps     the number of timed reloads (default 10). One load before them is not timed.
//   --marker   the text that the tool changes in each rep. The text must have the number
//              "/*BENCH_EDIT*/ 1" once. Default: the first line "// BENCH_EDIT <n>" is added.
//   --no-swap  do not run request_reload and commit_pending.
//
// The tool prints one line for each rep on stdout:
//   rep N total T begin B compile C finish F swap S      (milliseconds)
// "begin" is the translate, "compile" is the compiler and the linker, "finish" is the load of the
// library, and "swap" is request_reload and commit_pending with no live object. HOT_TIMING=1 also
// prints the phases of the backend on stderr.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/runtime/loader.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
    #include <dlfcn.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> read_lines(const std::string& path)
{
    std::vector<std::string> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            out.push_back(line);
    return out;
}

// The text of generation g: a comment line at the top changes the hash of the text. Each rep also
// changes one number in a method body when the script has the marker "/*BENCH_EDIT*/ <n>".
std::string edited_text(const std::string& base, int g)
{
    std::string text = base;
    const std::string marker = "/*BENCH_EDIT*/ ";
    const size_t at = text.find(marker);
    if (at != std::string::npos)
    {
        const size_t num = at + marker.size();
        size_t end = num;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
            ++end;
        text.replace(num, end - num, std::to_string(1000 + g));
    }
    else
        text = "// BENCH_EDIT " + std::to_string(g) + "\n" + text;
    return text;
}
} // namespace

int main(int argc, char** argv)
{
    std::string script, flags_file, work, cache;
    std::vector<std::string> includes, preloads, preludes;
    int reps = 10;
    bool swap = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--script")
            script = next();
        else if (a == "--flags")
            flags_file = next();
        else if (a == "--include")
            includes.push_back(next());
        else if (a == "--preload")
            preloads.push_back(next());
        else if (a == "--prelude")
            preludes.push_back(next());
        else if (a == "--reps")
            reps = std::atoi(next().c_str());
        else if (a == "--work")
            work = next();
        else if (a == "--cache")
            cache = next();
        else if (a == "--no-swap")
            swap = false;
        else
        {
            std::fprintf(stderr, "bench_reload: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (script.empty())
    {
        std::fprintf(stderr, "usage: bench_reload --script FILE [--flags FILE] [--include DIR]... [--preload LIB]...\n");
        return 2;
    }
#ifndef _WIN32
    // The wrapper gives the engine libraries by LD_PRELOAD. The loader has read it already. The
    // compiler and linker processes must not inherit it: each one would load the engine libraries
    // again (about 0.8 s for each process) and the phase times would be wrong.
    unsetenv("LD_PRELOAD");
    for (const std::string& lib : preloads)
        if (!dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL))
        {
            std::fprintf(stderr, "bench_reload: cannot open %s: %s\n", lib.c_str(), dlerror());
            return 2;
        }
#endif
    hot::DefaultBackendOptions d;
    d.kind = hot::BackendKind::ClangFiles;
    d.include_dirs = includes;
    d.include_dirs.push_back(script.substr(0, script.find_last_of('/') == std::string::npos ? 0 : script.find_last_of('/')));
    if (!flags_file.empty())
        for (const std::string& f : read_lines(flags_file))
        {
            // A "-I" flag is an include directory of the host: the translator needs it too.
            if (f.rfind("-I", 0) == 0 && f.size() > 2)
                d.include_dirs.push_back(f.substr(2));
            else
                d.extra_flags.push_back(f);
        }
    for (const std::string& p : preludes)
        d.preludes.emplace_back(p, read_file(p));
    d.work_dir = work;
    d.cache_dir = cache;
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend(d);
    auto* files = dynamic_cast<hot::ClangFilesBackend*>(backend.get());
    if (!files)
    {
        std::fprintf(stderr, "bench_reload: the build has no clang files backend\n");
        return 2;
    }
    const std::string base = read_file(script);
    hot::Loader loader;
    uint32_t generation = 1;

    // One reload step: the translate, the compile and link, the library load, then the swap.
    struct Step
    {
        double begin = 0, compile = 0, finish = 0, swap = 0, total = 0;
        bool ok = false;
        std::string error;
        hot::LoadResult result;
    };
    const auto step = [&](int g, bool first) -> Step
    {
        Step s;
        hot::ScriptSource src{script, edited_text(base, g), {}};
        const Clock::time_point t0 = Clock::now();
        auto many = files->begin_load_many({src}, generation, hot::LoadMode::Reload);
        s.begin = ms_since(t0);
        const Clock::time_point t1 = Clock::now();
        many[0]->compile();
        s.compile = ms_since(t1);
        const Clock::time_point t2 = Clock::now();
        s.result = many[0]->finish();
        s.finish = ms_since(t2);
        if (!s.result)
        {
            s.error = s.result.error;
            return s;
        }
        const Clock::time_point t3 = Clock::now();
        if (swap)
        {
            hot::ReloadResult r = first ? loader.load(std::move(*s.result.module))
                                        : loader.request_reload(std::move(s.result));
            if (r && !first)
                r = loader.commit_pending();
            if (!r)
            {
                s.error = "swap: " + r.error;
                return s;
            }
            loader.frame_boundary();
        }
        s.swap = ms_since(t3);
        s.total = ms_since(t0);
        s.ok = true;
        return s;
    };

    // The first load is the first flush of the process (not timed). The next waits for the PCH build.
    Step first = step(0, true);
    if (!first.ok)
    {
        std::fprintf(stderr, "bench_reload: first load failed: %s\n", first.error.substr(0, 2000).c_str());
        return 1;
    }
    files->wait_for_pch();
    std::printf("first load total %.0f ms (not timed)\n", first.total);
    std::fflush(stdout);
    for (int g = 1; g <= reps + 1; ++g)
    {
        ++generation;
        Step s = step(g, false);
        if (!s.ok)
        {
            std::fprintf(stderr, "bench_reload: rep %d failed: %s\n", g, s.error.substr(0, 2000).c_str());
            return 1;
        }
        // Rep 1 is a warm-up of the second load (it can build the edit-batch PCH). It is printed as "warmup".
        std::printf("%s %d total %.1f begin %.1f compile %.1f finish %.1f swap %.1f\n", g == 1 ? "warmup" : "rep",
            g, s.total, s.begin, s.compile, s.finish, s.swap);
        std::fflush(stdout);
        files->wait_for_pch();
    }
    return 0;
}
