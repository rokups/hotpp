// bench_backends: time the compile and the load of the two test scripts.
//
// The tool uses the backend that the build selected (the JIT backend only when HOT_ENABLE_CLANG_FILES is OFF). Run it
// many times for the cold numbers: each process starts cold.
//
// Usage:
//   bench_backends [--warm N] [--cxxflag F]... [--pch FILE] [--make-pch FILE] [--no-cache]
//
// It prints one line: the cold time of generation 1 and 2 (the first load of
// each in this process) and the median time of N warm loads of each. For the
// JIT backend it also prints the phases of the last warm load.
// The warm loads share one translator session (the header parse is cached).
// --no-cache gives each warm backend a new session, so every load parses the headers.
// The tool also prints the time of one translate with a new session (miss) and with
// the same session again (hit).
// --cxxflag adds a compile flag (for example -O1 for the CLANG_FILES backend).
// --make-pch FILE (JIT only) makes a precompiled header and exits.
// --pch FILE (JIT only) compiles with -include-pch FILE.
//
// CMake defines: BENCH_SOURCE_DIR, BENCH_CXX, BENCH_INCLUDES (items split by
// "|"), BENCH_RESOURCE_DIR, BENCH_WORK_DIR and BENCH_JIT or BENCH_CLANG_FILES.
#include "hot/runtime.h"
#include "hot/scratch.h"
#include "hot/hotc_lib.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(BENCH_JIT)
    #include "hot/jit_backend.h"
#else
    #include "hot/clang_files_backend.h"
#endif

namespace
{

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, '|');)
        if (!item.empty())
            v.push_back(item);
    return v;
}

hot::ScriptSource source(const char* file)
{
    const std::string path = std::string(BENCH_SOURCE_DIR) + "/tests/scripts/" + file;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return {path, ss.str()};
}

// One load. Returns the time in ms, or -1 on error (the text goes to stderr).
double timed_load(hot::Backend& b, const hot::ScriptSource& s, uint32_t gen)
{
    const Clock::time_point t = Clock::now();
    hot::LoadResult r = b.load(s, gen);
    const double ms = ms_since(t);
    if (!r)
    {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return -1;
    }
    // The Loader registers the class. This also proves that the module works,
    // and it links the runtime into this executable (the JIT code needs it).
    hot::Loader loader;
    const hot::ReloadResult loaded = loader.load(std::move(*r.module));
    if (!loaded)
    {
        std::fprintf(stderr, "register failed: %s\n", loaded.error.c_str());
        return -1;
    }
    return ms;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    int warm = 5;
    std::vector<std::string> flags;
    std::string pch, make_pch;
    bool no_cache = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--warm" && i + 1 < argc)
            warm = std::atoi(argv[++i]);
        else if (a == "--cxxflag" && i + 1 < argc)
            flags.push_back(argv[++i]);
        else if (a == "--no-cache")
            no_cache = true;
        else if (a == "--pch" && i + 1 < argc)
            pch = argv[++i];
        else if (a == "--make-pch" && i + 1 < argc)
            make_pch = argv[++i];
        else
        {
            std::fprintf(stderr,
                "usage: bench_backends [--warm N] [--cxxflag F]... [--pch FILE] "
                "[--make-pch FILE] [--no-cache]\n");
            return 2;
        }
    }
    const std::vector<std::string> includes = split(BENCH_INCLUDES);

    auto session = std::make_shared<hotc::TranslatorSession>();
    auto make = [&](bool share) -> std::unique_ptr<hot::Backend>
    {
#if defined(BENCH_JIT)
        hot::JitOptions o;
        o.compiler = BENCH_CXX;
        o.include_dirs = includes;
        o.resource_dir = BENCH_RESOURCE_DIR;
        o.extra_flags = flags;
        o.pch_path = pch;
        if (share)
            o.session = session;
        return std::make_unique<hot::JitBackend>(std::move(o));
#else
        hot::ClangFilesOptions o;
        o.compiler = BENCH_CXX;
        o.include_dirs = includes;
        o.resource_dir = BENCH_RESOURCE_DIR;
        o.extra_flags = flags;
        o.work_dir = std::string(BENCH_WORK_DIR) + "/bench" + std::to_string(getpid());
        std::error_code ec;
        std::filesystem::remove_all(o.work_dir, ec);
        o.remove_work_dir_at_exit = !hot::keep_temp();
        if (share)
            o.session = session;
        return std::make_unique<hot::ClangFilesBackend>(std::move(o));
#endif
    };

#if defined(BENCH_JIT)
    if (!make_pch.empty())
    {
        hot::JitOptions o;
        o.compiler = BENCH_CXX;
        o.include_dirs = includes;
        o.resource_dir = BENCH_RESOURCE_DIR;
        o.extra_flags = flags;
        hot::JitBackend b(std::move(o));
        const std::string header_path = make_pch + ".hpp";
        const char* header =
            "#include <algorithm>\n#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
            "#include <new>\n#include <string>\n#include <type_traits>\n#include <typeinfo>\n"
            "#include <utility>\n#include \"hot/runtime.h\"\n#include \"Actor.h\"\n"
            "#include \"Math.h\"\n";
        std::ofstream(header_path) << header;
        std::string error;
        const Clock::time_point t = Clock::now();
        const bool ok = b.make_pch(header_path, make_pch, error);
        std::printf("make_pch ok=%d ms=%.1f\n%s", ok, ms_since(t), error.c_str());
        return ok ? 0 : 1;
    }
#else
    if (!make_pch.empty() || !pch.empty())
    {
        std::fprintf(stderr, "--pch and --make-pch need the JIT backend\n");
        return 2;
    }
#endif

    const hot::ScriptSource v1 = source("player_v1.hot");
    const hot::ScriptSource v2 = source("player_v2.hot");
    std::unique_ptr<hot::Backend> backend = make(false);
    const double cold1 = timed_load(*backend, v1, 1);
    const double cold2 = timed_load(*backend, v2, 2);
    if (cold1 < 0 || cold2 < 0)
        return 1;

    // Warm: each round has a new backend, so the slot file starts clean.
    // The process keeps its LLVM state, its page cache and its file cache.
    std::vector<double> w1, w2;
    double tr = 0, co = 0, ji = 0;
    for (int i = 0; i < warm; ++i)
    {
        std::unique_ptr<hot::Backend> b = make(!no_cache);
        const double a = timed_load(*b, v1, 1);
        const double c = timed_load(*b, v2, 2);
        if (a < 0 || c < 0)
            return 1;
        w1.push_back(a);
        w2.push_back(c);
#if defined(BENCH_JIT)
        const hot::JitTimings t = static_cast<hot::JitBackend&>(*b).last_timings();
        tr = t.translate_ms, co = t.compile_ms, ji = t.jit_ms;
#endif
    }
    // One translate alone: a new session (miss) and then the same session (hit).
    double miss = 0, hit = 0;
    {
        hotc::TranslateOptions to;
        to.script_text = v1.text;
        to.script_name = v1.name;
        to.generation = 1;
        to.include_dirs = includes;
        to.resource_dir = BENCH_RESOURCE_DIR;
        hotc::TranslatorSession ts;
        Clock::time_point t = Clock::now();
        const bool ok1 = hotc::translate(to, ts).ok;
        miss = ms_since(t);
        t = Clock::now();
        const bool ok2 = hotc::translate(to, ts).ok;
        hit = ms_since(t);
        if (!ok1 || !ok2)
            return 1;
    }
    std::printf("translate_miss=%.1f translate_hit=%.1f\n", miss, hit);
    std::printf("backend=%s cold_v1=%.1f cold_v2=%.1f warm_v1=%.1f warm_v2=%.1f", backend->name(), cold1, cold2,
        median(w1), median(w2));
    if (tr > 0)
        std::printf(" (last v2: translate=%.1f compile=%.1f jit=%.1f)", tr, co, ji);
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    std::printf(" peak_rss_mb=%.0f\n", static_cast<double>(ru.ru_maxrss) / 1024.0);
    return 0;
}
