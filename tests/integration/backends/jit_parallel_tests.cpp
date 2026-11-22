// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the two phase load of the JIT backend (hot::PendingLoad, hot/backend.h): begin_load_many
// translates, the Clang compiles run on many threads, and finish adds the code to ORC.
// CMake defines the macros HOT_SOURCE_DIR, HOT_CXX and HOT_RESOURCE_DIR.
#include "hot/backend.h"
#include "hot/jit_backend.h"
#include "support/compiler_wrapper.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"
#include "support/test_support.h"
#include "jit_engine.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The two phase load of the JIT is on POSIX only (jit_backend.cpp).
#ifndef _WIN32

namespace
{

namespace fs = std::filesystem;

using test_support::kDt;
using test_support::ScopedEnv;
using test_support::Session;
using test_support::TestScratch;

// The tests must not depend on the environment of the caller.
struct CleanEnv
{
    CleanEnv()
        : cache("HOT_CACHE", "1")
        , max_mb("HOT_CACHE_MAX_MB", "")
    {
        test_support::unset_env("HOT_CACHE_MAX_MB");
    }
    ScopedEnv cache;
    ScopedEnv max_mb;
};

// The unit number "i" has the class "Calc<i>" and the result "10 * i + 7".
std::string class_name(int i)
{
    return "Calc" + std::to_string(i);
}

hot::ScriptSource unit_source(int i)
{
    const std::string cls = class_name(i);
    const std::string text = "using \"Actor.h\";\nclass " + cls + " : Actor {\n    float result = 0.0;\n"
                             "    override void Tick(float dt) { this.result = " + std::to_string(10 * i + 7) +
                             ".0; }\n}\n";
    return {"calc" + std::to_string(i) + ".hot", text, {}, cls};
}

std::vector<hot::ScriptSource> many_sources(int first, int count)
{
    std::vector<hot::ScriptSource> sources;
    for (int i = first; i < first + count; ++i)
        sources.push_back(unit_source(i));
    return sources;
}

std::unique_ptr<hot::JitBackend> make_backend(
    const std::string& cache_dir = std::string(), const std::string& extra_include = std::string())
{
    hot::JitOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    if (!extra_include.empty())
        o.include_dirs.push_back(extra_include);
    o.cache_dir = cache_dir;
    return std::make_unique<hot::JitBackend>(std::move(o));
}

// Makes an object of the class, runs one tick and gives the result field. -1 on an error.
float run_module(hot::LoadResult&& r, int i)
{
    if (!r)
    {
        ADD_FAILURE() << r.error;
        return -1.0f;
    }
    Session s;
    s.start(std::move(*r.module), class_name(i).c_str());
    if (!s.actor)
        return -1.0f;
    s.world.Tick(kDt);
    return *s.field("result");
}

// Runs the compiles with build_jobs() jobs and finishes each entry in order.
std::vector<hot::LoadResult> compile_and_finish(std::vector<std::unique_ptr<hot::PendingLoad>>& pending)
{
    std::vector<hot::PendingLoad*> raw;
    for (auto& p : pending)
        raw.push_back(p.get());
    for (const std::string& e : hot::run_compiles(raw, hot::build_jobs()))
        EXPECT_TRUE(e.empty()) << e;
    std::vector<hot::LoadResult> out;
    for (auto& p : pending)
        out.push_back(p->finish());
    return out;
}

std::string read_all(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The cached objects (name and bytes) of a cache directory.
std::map<std::string, std::string> cached_objects(const std::string& cache)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(cache) / "jit" / "obj", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".o")
            out[it->path().filename().string()] = read_all(it->path());
    return out;
}

// Writes the file and keeps its time: the stamp in the manifest still matches.
void overwrite_keep_time(const fs::path& path, const std::string& text)
{
    const fs::file_time_type t = fs::last_write_time(path);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    fs::last_write_time(path, t);
}

// The 24-byte header of the cache file (jit_backend.cpp) with a correct hash for "object".
std::string with_header(const std::string& object)
{
    uint64_t h = 14695981039346656037ULL;
    for (const char c : object)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    uint64_t z = h;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    std::string out = "HOTJITO1";
    for (uint64_t v : {static_cast<uint64_t>(object.size()), z})
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    return out + object;
}

bool same_head(const hot::LoadResult& a, const hot::LoadResult& b)
{
    return a.iface_hash == b.iface_hash && a.import_hashes == b.import_hashes && a.dependencies == b.dependencies &&
        a.error == b.error && a.reason == b.reason;
}

// The C++ texts of the stress test. Each text uses other parts of the standard library.
const char* const kStressTexts[] = {
    "#include <vector>\n#include <algorithm>\n"
    "int sum_sorted(std::vector<int> v) { std::sort(v.begin(), v.end()); int s = 0;\n"
    "  for (size_t i = 0; i < v.size(); ++i) s += v[i] * int(i); return s; }\n",
    "#include <string>\n#include <map>\n"
    "int count_words(const std::string& t) { std::map<std::string, int> m; std::string w;\n"
    "  for (char c : t) { if (c == ' ') { ++m[w]; w.clear(); } else w += c; } ++m[w]; return int(m.size()); }\n",
    "#include <memory>\n#include <functional>\n"
    "struct Node { int v; std::unique_ptr<Node> next; };\n"
    "int walk(int n) { std::unique_ptr<Node> h; for (int i = 0; i < n; ++i) h.reset(new Node{i, std::move(h)});\n"
    "  std::function<int(const Node*)> f = [&](const Node* p) { return p ? p->v + f(p->next.get()) : 0; };\n"
    "  return f(h.get()); }\n",
    "#include <cmath>\n#include <array>\n"
    "double norm3(double x, double y, double z) { std::array<double, 3> a{x, y, z}; double s = 0;\n"
    "  for (double v : a) s += v * v; return std::sqrt(s); }\n",
};

// The IR text of one compile of "text". Empty on failure; "error" gets the diagnostics.
std::string compile_ir_text(size_t index, std::string& error)
{
    const std::string path = "/hot-jit-stress/unit" + std::to_string(index) + ".cpp";
    const std::string resource = std::string("-resource-dir=") + HOT_RESOURCE_DIR;
    const char* args[] = {HOT_CXX, "-std=c++20", "-O1", "-fPIC", resource.c_str(), path.c_str()};
    const char* paths[] = {path.c_str()};
    const char* texts[] = {kStressTexts[index]};
    hot_jit_ir* ir = nullptr;
    char* diagnostics = nullptr;
    const int ok =
        hot_jit_compile(args, std::size(args), paths, texts, 1, &ir, &diagnostics, nullptr);
    error = diagnostics ? diagnostics : "";
    hot_jit_string_free(diagnostics);
    if (!ok)
        return std::string();
    char* text = hot_jit_ir_link_text(ir);
    std::string out = text ? text : "";
    hot_jit_string_free(text);
    hot_jit_ir_free(ir);
    return out;
}

} // namespace

// S3: 8 threads each run 20 compiles. Each IR text equals the text of the serial compile.
TEST(JitParallel, ConcurrentCompilesGiveTheSerialIr)
{
    constexpr size_t kTexts = std::size(kStressTexts);
    std::vector<std::string> serial(kTexts);
    for (size_t i = 0; i < kTexts; ++i)
    {
        std::string error;
        serial[i] = compile_ir_text(i, error);
        ASSERT_FALSE(serial[i].empty()) << error;
    }
    constexpr int kThreads = 8;
    constexpr int kRuns = 20;
    std::vector<std::vector<std::string>> results(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(
            [&, t]
            {
                for (int k = 0; k < kRuns; ++k)
                {
                    std::string error;
                    std::string ir = compile_ir_text(static_cast<size_t>(t + k) % kTexts, error);
                    results[t].push_back(ir.empty() ? "ERROR " + error : std::move(ir));
                }
            });
    for (std::thread& th : threads)
        th.join();
    for (int t = 0; t < kThreads; ++t)
    {
        ASSERT_EQ(results[t].size(), static_cast<size_t>(kRuns));
        for (int k = 0; k < kRuns; ++k)
            EXPECT_EQ(results[t][k], serial[static_cast<size_t>(t + k) % kTexts]) << "thread " << t << " run " << k;
    }
}

// HOT_JOBS=1 and HOT_JOBS=8 give the same heads, the same modules and the same cached objects.
// Unit "kid" imports unit "base" in the same wave.
TEST(JitParallel, JobCountDoesNotChangeTheModules)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string base_text = "using \"Actor.h\";\nclass Base1 : Actor {\n    int n = 1;\n}\n";
    const std::string kid_text = "using \"Actor.h\";\nusing \"base.hot\";\nclass Kid1 : Base1 {\n    int m = 2;\n}\n";
    std::vector<hot::ScriptSource> sources = many_sources(0, 10);
    sources.insert(sources.begin() + 3, hot::ScriptSource{"kid.hot", kid_text, {{"base.hot", base_text}}, "kid"});
    sources.push_back(hot::ScriptSource{"base.hot", base_text, {}, "base"});
    ASSERT_EQ(sources.size(), 12u);
    std::vector<hot::LoadResult> heads[2];
    std::map<std::string, std::string> objects[2];
    const char* jobs_text[2] = {"1", "8"};
    for (int k = 0; k < 2; ++k)
    {
        ScopedEnv jobs("HOT_JOBS", jobs_text[k]);
        const std::string cache = scratch.file(std::string("cache") + jobs_text[k]);
        auto backend = make_backend(cache);
        auto pending = backend->begin_load_many(sources, 1);
        ASSERT_EQ(pending.size(), sources.size());
        for (auto& p : pending)
        {
            ASSERT_NE(p, nullptr);
            ASSERT_TRUE(p->ok) << p->head.error;
            heads[k].push_back(p->head);
        }
        std::vector<hot::LoadResult> results = compile_and_finish(pending);
        EXPECT_EQ(backend->compiles(), sources.size());
        for (size_t i = 0; i < results.size(); ++i)
        {
            ASSERT_TRUE(results[i]) << sources[i].unit << ": " << results[i].error;
            const std::string& unit = sources[i].unit;
            if (unit.rfind("Calc", 0) == 0)
            {
                const int n = std::stoi(unit.substr(4));
                EXPECT_EQ(run_module(std::move(results[i]), n), static_cast<float>(10 * n + 7)) << unit;
            }
        }
        objects[k] = cached_objects(cache);
    }
    for (size_t i = 0; i < heads[0].size(); ++i)
        EXPECT_TRUE(same_head(heads[0][i], heads[1][i])) << sources[i].unit;
    EXPECT_FALSE(heads[0][3].import_hashes.empty()) << "the kid imports the base";
#ifndef __APPLE__ // the JIT object cache is on Linux only (kObjectCacheSupported in jit_backend.cpp)
    EXPECT_EQ(objects[0].size(), sources.size());
    EXPECT_TRUE(objects[0] == objects[1]) << "the cached objects differ";
#endif
}

// A Clang error in one of five units fails only that entry, in finish. The other entries load.
TEST(JitParallel, CompileErrorFailsOnlyItsEntry)
{
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    TestScratch scratch;
    const std::string dir = scratch.file("include");
    fs::create_directories(dir);
    std::ofstream(dir + "/Broken.h") << "#pragma once\n#ifdef __OPTIMIZE__\n#error the compile fails\n#endif\n";
    auto backend = make_backend(std::string(), dir);
    std::vector<hot::ScriptSource> sources = many_sources(0, 5);
    // The translate passes, the compile fails: only the compile has -O1.
    sources[2].text = "using \"Broken.h\";\n" + sources[2].text;
    auto pending = backend->begin_load_many(sources, 1);
    ASSERT_EQ(pending.size(), 5u);
    for (auto& p : pending)
        ASSERT_TRUE(p->ok) << p->head.error;
    std::vector<hot::LoadResult> results = compile_and_finish(pending);
    for (size_t i = 0; i < 5; ++i)
    {
        if (i == 2)
            continue;
        EXPECT_EQ(run_module(std::move(results[i]), static_cast<int>(i)), static_cast<float>(10 * i + 7)) << i;
    }
    EXPECT_FALSE(results[2]);
    EXPECT_EQ(results[2].reason, hot::ReloadReason::CompileError);
    EXPECT_NE(results[2].error.find("the compile fails"), std::string::npos) << results[2].error;
    EXPECT_NE(results[2].error.find("the compile of calc2.hot failed"), std::string::npos) << results[2].error;
}

// One wave has cache hits (no Clang compile) and misses. A damaged object compiles again in
// compile. An object that does not load compiles again in finish, on the caller thread.
TEST(JitParallel, HitsMissesAndBadObjectsInOneWave)
{
#ifdef __APPLE__
    GTEST_SKIP() << "the JIT object cache is on Linux only (kObjectCacheSupported in jit_backend.cpp)";
#endif
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    // The object of each unit: the new file after its load.
    std::map<int, fs::path> object_of;
    {
        auto backend = make_backend(cache);
        for (int i = 0; i < 4; ++i)
        {
            const std::map<std::string, std::string> before = cached_objects(cache);
            ASSERT_TRUE(backend->load(unit_source(i), 1));
            for (const auto& [name, bytes] : cached_objects(cache))
                if (!before.count(name))
                    object_of[i] = fs::path(cache) / "jit" / "obj" / name;
            ASSERT_TRUE(object_of.count(i)) << i;
        }
    }
    const std::string good1 = read_all(object_of[1]);
    const std::string good2 = read_all(object_of[2]);
    std::string damaged = good1;
    damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x5a);
    overwrite_keep_time(object_of[1], damaged); // the hash is not correct
    overwrite_keep_time(object_of[2], with_header(std::string(good2.size() - 24, '\x7f'))); // no link
    auto backend = make_backend(cache);
    auto pending = backend->begin_load_many(many_sources(0, 6), 1);
    ASSERT_EQ(pending.size(), 6u);
    for (auto& p : pending)
        ASSERT_TRUE(p->ok) << p->head.error;
    std::vector<hot::LoadResult> results = compile_and_finish(pending);
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(run_module(std::move(results[i]), i), static_cast<float>(10 * i + 7)) << i;
    // Units 0 and 3 hit. Units 4 and 5 are new. Units 1 and 2 compile again.
    EXPECT_EQ(backend->compiles(), 4u);
    EXPECT_EQ(backend->cache_stats().hits, 2u);
    EXPECT_EQ(backend->cache_stats().misses, 4u);
    EXPECT_EQ(read_all(object_of[1]), good1) << "the store replaces the damaged entry";
    EXPECT_EQ(read_all(object_of[2]), good2) << "the store replaces the entry that does not load";
}

// HOT_JOBS=N gives at most N compiles at the same time.
TEST(JitParallel, HotJobsLimitsTheParallelCompiles)
{
    CleanEnv env;
    for (const char* n : {"1", "3"})
    {
        ScopedEnv jobs("HOT_JOBS", n);
        auto backend = make_backend();
        auto pending = backend->begin_load_many(many_sources(0, 8), 1);
        hot::JitBackend::peak_parallel_compiles(true);
        std::vector<hot::LoadResult> results = compile_and_finish(pending);
        const unsigned peak = hot::JitBackend::peak_parallel_compiles(true);
        for (size_t i = 0; i < results.size(); ++i)
            EXPECT_TRUE(results[i]) << results[i].error;
        EXPECT_EQ(backend->compiles(), 8u);
        EXPECT_LE(peak, static_cast<unsigned>(std::stoi(n))) << n;
        EXPECT_GE(peak, n[0] == '1' ? 1u : 2u) << n;
    }
}

#endif // _WIN32
