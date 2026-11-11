// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the two phase load (hot::PendingLoad, hot/backend.h) of the clang files backend:
// begin_load, then compile on many threads, then finish.
//
// A test compiler wrapper (support/compiler_wrapper.h) shows that the compiles run at the same time.
// The tests need a POSIX shell, so they skip on Windows.
// CMake defines the macros HOT_SOURCE_DIR and HOT_CXX.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/runtime/process.h"
#include "hot/vfs.h"
#include "support/compiler_wrapper.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;

using test_support::CompilerWrapper;
using test_support::kDt;
using test_support::ScopedEnv;
using test_support::Session;
using test_support::TestScratch;

constexpr unsigned kWaitSeconds = 5;

// The tests must not depend on the environment of the caller.
struct CleanEnv
{
    CleanEnv()
        : cache("HOT_CACHE", "1")
        , link_cache("HOT_LINK_CACHE", "1")
        , max_mb("HOT_CACHE_MAX_MB", "")
    {
        test_support::unset_env("HOT_CACHE_MAX_MB");
    }
    ScopedEnv cache;
    ScopedEnv link_cache;
    ScopedEnv max_mb;
};

// The unit number "i" has the class "Calc<i>" and the result "10 * i + 7".
std::string class_name(int i)
{
    return "Calc" + std::to_string(i);
}

float expected_result(int i)
{
    return static_cast<float>(10 * i + 7);
}

hot::ScriptSource unit_source(int i)
{
    const std::string cls = class_name(i);
    const std::string text = "using \"Actor.h\";\nclass " + cls + " : Actor {\n    float result = 0.0;\n"
                             "    override void Tick(float dt) { this.result = " + std::to_string(10 * i + 7) +
                             ".0; }\n}\n";
    return {"calc" + std::to_string(i) + ".hot", text, {}, cls};
}

std::unique_ptr<hot::ClangFilesBackend> make_backend(const TestScratch& scratch, const std::string& work_name,
    const std::string& cache_dir, const std::string& compiler)
{
    hot::ClangFilesOptions o;
    o.compiler = compiler;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
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

// The library files in the work directory of a backend (the files ".so", ".dll" or ".dylib").
size_t count_libraries(const std::string& work_dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(work_dir, ec))
    {
        const std::string ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".so" || ext == ".dll" || ext == ".dylib"))
            ++n;
    }
    return n;
}

#ifdef _WIN32
#define SKIP_ON_WINDOWS() GTEST_SKIP() << "the compiler wrapper is a POSIX shell script"
#else
#define SKIP_ON_WINDOWS() (void)0
#endif

// The three phases give the same result as load. A second backend that uses load gets a cache hit.
TEST(ParallelBuild, TwoPhaseEqualsLoad)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    {
        auto backend = make_backend(scratch, "work_a", cache, wrapper.path());
        std::unique_ptr<hot::PendingLoad> pending = backend->begin_load(unit_source(1), 1);
        ASSERT_NE(pending, nullptr);
        ASSERT_TRUE(pending->ok) << pending->head.error;
        pending->compile();
        EXPECT_EQ(run_module(pending->finish(), 1), expected_result(1));
        EXPECT_EQ(backend->cache_stats().misses, 1u);
    }
    auto backend = make_backend(scratch, "work_b", cache, wrapper.path());
    EXPECT_EQ(run_module(backend->load(unit_source(1), 1), 1), expected_result(1));
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
}

// Four units: begin_load runs in order, the four compile calls run on four threads at the same time.
TEST(ParallelBuild, CompilesRunAtTheSameTime)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    wrapper.set_wait(kWaitSeconds);
    auto backend = make_backend(scratch, "work", scratch.file("cache"), wrapper.path());
    std::vector<std::unique_ptr<hot::PendingLoad>> pending;
    for (int i = 0; i < 4; ++i)
    {
        pending.push_back(backend->begin_load(unit_source(i), 1));
        ASSERT_NE(pending.back(), nullptr);
        ASSERT_TRUE(pending.back()->ok) << pending.back()->head.error;
    }
    std::vector<std::thread> threads;
    for (auto& p : pending)
        threads.emplace_back([&p] { p->compile(); });
    for (std::thread& t : threads)
        t.join();
    EXPECT_TRUE(wrapper.overlap()) << "two compiles must run at the same time";
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(run_module(pending[i]->finish(), i), expected_result(i)) << "unit " << i;
    EXPECT_EQ(backend->cache_stats().misses, 4u);
}

// The object cache of a build that compiled the four units serves a second backend.
TEST(ParallelBuild, SecondBackendHitsAll)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    {
        auto backend = make_backend(scratch, "work_a", cache, wrapper.path());
        std::vector<std::unique_ptr<hot::PendingLoad>> pending;
        for (int i = 0; i < 4; ++i)
        {
            pending.push_back(backend->begin_load(unit_source(i), 1));
            ASSERT_NE(pending.back(), nullptr);
            ASSERT_TRUE(pending.back()->ok) << pending.back()->head.error;
        }
        std::vector<std::thread> threads;
        for (auto& p : pending)
            threads.emplace_back([&p] { p->compile(); });
        for (std::thread& t : threads)
            t.join();
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(run_module(pending[i]->finish(), i), expected_result(i)) << "unit " << i;
        EXPECT_EQ(backend->cache_stats().misses, 4u);
    }
    wrapper.reset();
    auto backend = make_backend(scratch, "work_b", cache, wrapper.path());
    std::vector<std::unique_ptr<hot::PendingLoad>> pending;
    for (int i = 0; i < 4; ++i)
    {
        pending.push_back(backend->begin_load(unit_source(i), 1));
        ASSERT_NE(pending.back(), nullptr);
        ASSERT_TRUE(pending.back()->ok) << pending.back()->head.error;
    }
    std::vector<std::thread> threads;
    for (auto& p : pending)
        threads.emplace_back([&p] { p->compile(); });
    for (std::thread& t : threads)
        t.join();
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(run_module(pending[i]->finish(), i), expected_result(i)) << "unit " << i;
    EXPECT_EQ(backend->cache_stats().hits, 4u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
    EXPECT_EQ(wrapper.compile_calls(), 0u);
}

// A pending load that the caller drops after compile removes its library file.
TEST(ParallelBuild, DroppedPendingRemovesItsLibrary)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    auto backend = make_backend(scratch, "work", scratch.file("cache"), wrapper.path());
    const std::string work = scratch.file("work");
    {
        std::unique_ptr<hot::PendingLoad> pending = backend->begin_load(unit_source(1), 1);
        ASSERT_NE(pending, nullptr);
        ASSERT_TRUE(pending->ok) << pending->head.error;
        pending->compile();
        EXPECT_EQ(count_libraries(work), 1u) << "compile must make the library";
    }
    EXPECT_EQ(count_libraries(work), 0u) << "a dropped pending load must remove its library";
}

// A file system in memory. A mutex makes it thread safe, as the interface requires.
class MemoryFs final : public hot::FileSystem
{
public:
    bool read(const std::string& path, std::string& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = files_.find(path);
        if (it == files_.end())
            return false;
        out = it->second;
        return true;
    }
    hot::VfsStat stat(const std::string& path) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = files_.find(path);
        if (it == files_.end())
            return {};
        return {true, 1};
    }
    void set(const std::string& path, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = text;
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> files_;
};

// The unit "i" of a host file system test: the header "v.h" has the same name in each script
// directory, and a different text. The class "Calc<i>" has the result "10 * i + 7" from the header.
std::string vfs_dir(int i)
{
    return "/hot_pb_vfs/u" + std::to_string(i);
}

hot::ScriptSource vfs_unit_source(int i)
{
    const std::string cls = class_name(i);
    const std::string text = "using \"Actor.h\";\nusing \"v.h\";\nclass " + cls +
                             " : Actor {\n    float result = 0.0;\n"
                             "    override void Tick(float dt) { this.result = vfs_value(); }\n}\n";
    return {vfs_dir(i) + "/calc" + std::to_string(i) + ".hot", text, {}, cls};
}

void set_vfs_headers(MemoryFs& fs)
{
    for (int i = 0; i < 4; ++i)
        fs.set(vfs_dir(i) + "/v.h", "inline float vfs_value() { return " + std::to_string(10 * i + 7) + ".0f; }\n");
}

std::unique_ptr<hot::ClangFilesBackend> make_vfs_backend(const TestScratch& scratch, const std::string& work_name,
    const std::string& cache_dir, const std::string& compiler, const std::shared_ptr<MemoryFs>& fs)
{
    hot::ClangFilesOptions o;
    o.compiler = compiler;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    o.fs = fs;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

// The count of the files and directories below a directory (0 when it does not exist).
size_t count_entries(const std::string& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    {
        (void)e;
        ++n;
    }
    return n;
}

// The count of the mirror headers "v.h" below a directory.
size_t count_mirror_headers(const std::string& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().filename() == "v.h")
            ++n;
    return n;
}

// With a host file system, four units compile at the same time. Each has its own header text.
TEST(ParallelBuild, VfsCompilesRunAtTheSameTime)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    wrapper.set_wait(kWaitSeconds);
    auto fs = std::make_shared<MemoryFs>();
    set_vfs_headers(*fs);
    auto backend = make_vfs_backend(scratch, "work", scratch.file("cache"), wrapper.path(), fs);
    std::vector<std::unique_ptr<hot::PendingLoad>> pending;
    for (int i = 0; i < 4; ++i)
    {
        pending.push_back(backend->begin_load(vfs_unit_source(i), 1));
        ASSERT_NE(pending.back(), nullptr);
        ASSERT_TRUE(pending.back()->ok) << pending.back()->head.error;
    }
    std::vector<std::thread> threads;
    for (auto& p : pending)
        threads.emplace_back([&p] { p->compile(); });
    for (std::thread& t : threads)
        t.join();
    EXPECT_TRUE(wrapper.overlap()) << "two compiles must run at the same time";
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(run_module(pending[i]->finish(), i), expected_result(i)) << "unit " << i;
}

// After begin_load of four units, the four mirror headers exist at the same time.
TEST(ParallelBuild, VfsBeginLoadIsTwoPhase)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    auto fs = std::make_shared<MemoryFs>();
    set_vfs_headers(*fs);
    auto backend = make_vfs_backend(scratch, "work", scratch.file("cache"), wrapper.path(), fs);
    std::vector<std::unique_ptr<hot::PendingLoad>> pending;
    for (int i = 0; i < 4; ++i)
    {
        pending.push_back(backend->begin_load(vfs_unit_source(i), 1));
        ASSERT_NE(pending.back(), nullptr);
        ASSERT_TRUE(pending.back()->ok) << pending.back()->head.error;
    }
    EXPECT_EQ(count_mirror_headers(scratch.file("work") + "/vfs.job"), 4u);
    // The name of each job directory starts with the process id.
    const std::string pid_prefix = std::to_string(hot::detail::process_id()) + ".";
    size_t named = 0;
    for (const auto& e : fs::directory_iterator(scratch.file("work") + "/vfs.job"))
    {
        EXPECT_EQ(e.path().filename().string().rfind(pid_prefix, 0), 0u) << e.path();
        ++named;
    }
    EXPECT_EQ(named, 4u);
    EXPECT_EQ(wrapper.compile_calls(), 0u) << "begin_load must not compile";
}

// The mirror of a job is gone after its compile. A dropped pending load leaves none too.
TEST(ParallelBuild, VfsMirrorIsGoneAfterCompile)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    auto fs = std::make_shared<MemoryFs>();
    set_vfs_headers(*fs);
    auto backend = make_vfs_backend(scratch, "work", scratch.file("cache"), wrapper.path(), fs);
    const std::string jobs = scratch.file("work") + "/vfs.job";
    {
        std::unique_ptr<hot::PendingLoad> pending = backend->begin_load(vfs_unit_source(1), 1);
        ASSERT_NE(pending, nullptr);
        ASSERT_TRUE(pending->ok) << pending->head.error;
        EXPECT_EQ(count_mirror_headers(jobs), 1u);
        pending->compile();
        EXPECT_EQ(count_entries(jobs), 0u) << "compile must remove the mirror";
        EXPECT_EQ(run_module(pending->finish(), 1), expected_result(1));
        EXPECT_EQ(count_entries(jobs), 0u);
    }
    {
        std::unique_ptr<hot::PendingLoad> pending = backend->begin_load(vfs_unit_source(2), 1);
        ASSERT_NE(pending, nullptr);
        ASSERT_TRUE(pending->ok) << pending->head.error;
        EXPECT_EQ(count_mirror_headers(jobs), 1u);
    }
    EXPECT_EQ(count_entries(jobs), 0u) << "a dropped pending load must remove the mirror";
}

// The object key does not depend on the job: the same unit loaded twice gives a cache hit.
TEST(ParallelBuild, VfsKeyIsStableAcrossJobs)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    auto fs = std::make_shared<MemoryFs>();
    set_vfs_headers(*fs);
    auto backend = make_vfs_backend(scratch, "work", scratch.file("cache"), wrapper.path(), fs);
    for (int round = 0; round < 2; ++round)
    {
        std::unique_ptr<hot::PendingLoad> pending = backend->begin_load(vfs_unit_source(1), 1 + round);
        ASSERT_NE(pending, nullptr);
        ASSERT_TRUE(pending->ok) << pending->head.error;
        pending->compile();
        EXPECT_EQ(run_module(pending->finish(), 1), expected_result(1));
    }
    EXPECT_EQ(backend->cache_stats().misses, 1u);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
}

// The three phases give the same result as load, with a host file system.
TEST(ParallelBuild, VfsTwoPhaseEqualsLoad)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    CompilerWrapper wrapper(scratch.file("wrap"), HOT_CXX);
    auto fs = std::make_shared<MemoryFs>();
    set_vfs_headers(*fs);
    {
        auto backend = make_vfs_backend(scratch, "work_a", scratch.file("cache"), wrapper.path(), fs);
        std::unique_ptr<hot::PendingLoad> pending = backend->begin_load(vfs_unit_source(3), 1);
        ASSERT_NE(pending, nullptr);
        ASSERT_TRUE(pending->ok) << pending->head.error;
        pending->compile();
        EXPECT_EQ(run_module(pending->finish(), 3), expected_result(3));
        EXPECT_EQ(backend->cache_stats().misses, 1u);
    }
    auto backend = make_vfs_backend(scratch, "work_b", scratch.file("cache"), wrapper.path(), fs);
    EXPECT_EQ(run_module(backend->load(vfs_unit_source(3), 1), 3), expected_result(3));
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
}

// ---- begin_load_many (hot/backend.h) ----

// A backend with a stable generated dir and an own slots file, for the equality tests.
std::unique_ptr<hot::ClangFilesBackend> make_many_backend(const TestScratch& scratch, const std::string& name,
    const std::string& cache_dir = "", const std::shared_ptr<hot::FileSystem>& fs = nullptr)
{
    hot::ClangFilesOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(name);
    o.generated_dir = scratch.file(name + "_gen");
    o.cache_dir = cache_dir;
    o.fs = fs;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

bool same_head(const hot::LoadResult& a, const hot::LoadResult& b)
{
    return a.iface_hash == b.iface_hash && a.import_hashes == b.import_hashes && a.dependencies == b.dependencies &&
        a.error == b.error && a.reason == b.reason;
}

// Runs the compiles, finishes each load, accepts each module in order. Gives the slots text.
std::string finish_all(const hot::ClangFilesBackend& backend, std::vector<std::unique_ptr<hot::PendingLoad>>& pending)
{
    std::vector<hot::PendingLoad*> raw;
    for (auto& p : pending)
        raw.push_back(p.get());
    for (const std::string& e : hot::run_compiles(raw, 4))
        EXPECT_TRUE(e.empty()) << e;
    for (auto& p : pending)
    {
        hot::LoadResult r = p->finish();
        EXPECT_TRUE(r) << r.error;
        if (r.module && r.module->on_accept)
            r.module->on_accept();
    }
    std::string slots;
    std::ifstream in(backend.slots_file(), std::ios::binary);
    slots.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return slots;
}

std::map<std::string, std::string> read_tree(const std::string& dir)
{
    std::map<std::string, std::string> texts;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file())
        {
            std::ifstream in(e.path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            // The "#line" name has the directory: the texts of two backends compare without it.
            for (size_t at = text.find(dir); at != std::string::npos; at = text.find(dir, at))
                text.replace(at, dir.size(), "<gen>");
            texts[fs::relative(e.path(), dir).string()] = std::move(text);
        }
    return texts;
}

std::vector<hot::ScriptSource> many_sources(int first, int count)
{
    std::vector<hot::ScriptSource> sources;
    for (int i = first; i < first + count; ++i)
        sources.push_back(unit_source(i));
    return sources;
}

// T5: unit A imports unit B. The order in the wave does not change the heads.
TEST(BeginLoadMany, ImportInTheSameWaveGivesTheSerialHeads)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    const std::string b_text = "using \"Actor.h\";\nclass Base1 : Actor {\n    int n = 1;\n}\n";
    const std::string a_text = "using \"Actor.h\";\nusing \"b.hot\";\nclass Kid1 : Base1 {\n    int m = 2;\n}\n";
    const hot::ScriptSource b{"b.hot", b_text, {}, "b"};
    const hot::ScriptSource a{"a.hot", a_text, {{"b.hot", b_text}}, "a"};
    TestScratch scratch;
    std::map<std::string, hot::LoadResult> serial;
    {
        auto backend = make_many_backend(scratch, "serial");
        for (const hot::ScriptSource* s : {&a, &b})
        {
            auto p = backend->begin_load(*s, 1);
            ASSERT_TRUE(p->ok) << p->head.error;
            serial[s->unit] = p->head;
        }
    }
    ASSERT_NE(serial["a"].iface_hash, 0u);
    ASSERT_FALSE(serial["a"].import_hashes.empty());
    int n = 0;
    for (const std::vector<hot::ScriptSource>& order : {std::vector<hot::ScriptSource>{a, b},
             std::vector<hot::ScriptSource>{b, a}})
    {
        auto backend = make_many_backend(scratch, "many" + std::to_string(n++));
        auto pending = backend->begin_load_many(order, 1);
        ASSERT_EQ(pending.size(), 2u);
        for (size_t i = 0; i < 2; ++i)
        {
            ASSERT_NE(pending[i], nullptr);
            ASSERT_TRUE(pending[i]->ok) << pending[i]->head.error;
            EXPECT_TRUE(same_head(pending[i]->head, serial[order[i].unit])) << order[i].unit;
        }
    }
}

// T6: HOT_JOBS=1 and HOT_JOBS=4 give the same heads, .cpp texts and slots text.
TEST(BeginLoadMany, JobCountDoesNotChangeTheResult)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    std::vector<hot::LoadResult> heads[2];
    std::map<std::string, std::string> cpps[2];
    std::string slots[2];
    const char* jobs_text[2] = {"1", "4"};
    for (int k = 0; k < 2; ++k)
    {
        ScopedEnv jobs("HOT_JOBS", jobs_text[k]);
        auto backend = make_many_backend(scratch, "j" + std::string(jobs_text[k]), scratch.file("cache"));
        auto pending = backend->begin_load_many(many_sources(0, 12), 1);
        ASSERT_EQ(pending.size(), 12u);
        for (auto& p : pending)
        {
            ASSERT_NE(p, nullptr);
            ASSERT_TRUE(p->ok) << p->head.error;
            heads[k].push_back(p->head);
        }
        slots[k] = finish_all(*backend, pending);
        cpps[k] = read_tree(scratch.file("j" + std::string(jobs_text[k]) + "_gen"));
    }
    ASSERT_EQ(heads[0].size(), heads[1].size());
    for (size_t i = 0; i < heads[0].size(); ++i)
        EXPECT_TRUE(same_head(heads[0][i], heads[1][i])) << i;
    EXPECT_EQ(cpps[0].size(), 12u);
    EXPECT_EQ(cpps[0], cpps[1]);
    EXPECT_FALSE(slots[0].empty());
    EXPECT_EQ(slots[0], slots[1]);
}

// T7: a translate error fails only its own entry.
TEST(BeginLoadMany, ErrorFailsOnlyItsEntry)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    TestScratch scratch;
    auto backend = make_many_backend(scratch, "work");
    std::vector<hot::ScriptSource> sources = many_sources(0, 5);
    sources[2].text = "using \"Actor.h\";\nclass Broken : Actor {\n    int = ;\n}\n";
    auto pending = backend->begin_load_many(sources, 1);
    ASSERT_EQ(pending.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
    {
        ASSERT_NE(pending[i], nullptr);
        EXPECT_EQ(pending[i]->ok, i != 2) << i;
    }
    EXPECT_FALSE(pending[2]->head.error.empty());
    EXPECT_EQ(pending[2]->head.reason, hot::ReloadReason::CompileError);
    EXPECT_TRUE(pending[0]->head.error.empty());
}

// A unit name that two entries share fails the second entry.
TEST(BeginLoadMany, SameUnitNameFailsTheLaterEntry)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    TestScratch scratch;
    auto backend = make_many_backend(scratch, "work");
    std::vector<hot::ScriptSource> sources = many_sources(0, 3);
    sources[2].unit = sources[0].unit;
    auto pending = backend->begin_load_many(sources, 1);
    ASSERT_EQ(pending.size(), 3u);
    EXPECT_TRUE(pending[0]->ok);
    EXPECT_TRUE(pending[1]->ok);
    EXPECT_FALSE(pending[2]->ok);
    EXPECT_NE(pending[2]->head.error.find("same unit name"), std::string::npos) << pending[2]->head.error;
}

// T8: Reload mode with an existing slots file gives the slot numbers of the serial build.
// The unit "i" with two script methods. "swapped" changes the order of the methods.
hot::ScriptSource method_source(int i, bool swapped)
{
    const std::string cls = class_name(i);
    const std::string n = std::to_string(i);
    const std::string poke = "    void poke" + n + "(int amount) { this.result = amount; }\n";
    const std::string peek = "    void peek" + n + "(int amount) { this.result = amount + 1; }\n";
    const std::string text = "using \"Actor.h\";\nclass " + cls + " : Actor {\n    float result = 0.0;\n" +
                             (swapped ? peek + poke : poke + peek) +
                             "    override void Tick(float dt) { this.result = " + std::to_string(10 * i + 7) +
                             ".0; }\n}\n";
    return {"calc" + n + ".hot", text, {}, cls};
}

// The reload of units 0 and 1 must reuse their slot numbers. Unit 1 changes its method order.
// The slots file of the first generation holds the last unit, which is unit 1.
TEST(BeginLoadMany, ReloadKeepsTheSlotNumbersOfTheSerialBuild)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    TestScratch scratch;
    std::string old_slots;
    {
        auto backend = make_many_backend(scratch, "first", scratch.file("cache"));
        std::vector<std::unique_ptr<hot::PendingLoad>> pending;
        for (int i = 0; i < 2; ++i)
            pending.push_back(backend->begin_load(method_source(i, false), 1));
        old_slots = finish_all(*backend, pending);
    }
    ASSERT_FALSE(old_slots.empty());
    std::vector<hot::ScriptSource> sources;
    for (int i = 0; i < 4; ++i)
        sources.push_back(method_source(i, i == 1));
    std::string slots[2];
    std::map<std::string, std::string> cpps[2];
    for (int k = 0; k < 2; ++k)
    {
        const std::string name = k == 0 ? "serial" : "many";
        auto backend = make_many_backend(scratch, name, scratch.file("cache"));
        {
            // The work directory does not exist yet: a write without it fails.
            fs::create_directories(fs::path(backend->slots_file()).parent_path());
            std::ofstream out(backend->slots_file(), std::ios::binary);
            out << old_slots;
            ASSERT_TRUE(out.good());
        }
        std::vector<std::unique_ptr<hot::PendingLoad>> pending;
        if (k == 0)
            for (const hot::ScriptSource& s : sources)
                pending.push_back(backend->begin_load(s, 2, hot::LoadMode::Reload));
        else
            pending = backend->begin_load_many(sources, 2, hot::LoadMode::Reload);
        for (auto& p : pending)
            ASSERT_TRUE(p->ok) << p->head.error;
        slots[k] = finish_all(*backend, pending);
        cpps[k] = read_tree(scratch.file(name + "_gen"));
    }
    EXPECT_EQ(slots[0], slots[1]);
    EXPECT_EQ(cpps[0], cpps[1]);
    // The first generation gave poke1 the slot 3 and peek1 the slot 4. The swap must not change them.
    for (int k = 0; k < 2; ++k)
    {
        const std::string& cpp = cpps[k]["Calc1.cpp"];
        EXPECT_NE(cpp.find("SLOT_poke1 = 3"), std::string::npos) << cpp;
        EXPECT_NE(cpp.find("SLOT_peek1 = 4"), std::string::npos);
    }
}

// A file system that fails to read one path.
class ThrowingFs final : public hot::FileSystem
{
public:
    explicit ThrowingFs(std::string bad) : bad_(std::move(bad)) {}
    bool read(const std::string& path, std::string& out) override
    {
        if (path == bad_)
            throw std::runtime_error("the read failed");
        return inner.read(path, out);
    }
    hot::VfsStat stat(const std::string& path) override { return inner.stat(path); }
    MemoryFs inner;

private:
    std::string bad_;
};

// A throw in the first phase of one unit: only that entry fails, and no mirror stays.
TEST(BeginLoadMany, ThrowInOneUnitFailsOnlyThatEntry)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    TestScratch scratch;
    auto fs = std::make_shared<ThrowingFs>(vfs_dir(2) + "/v.h");
    set_vfs_headers(fs->inner);
    auto backend = make_many_backend(scratch, "work", scratch.file("cache"), fs);
    const std::string jobs_dir = scratch.file("work") + "/vfs.job";
    std::vector<hot::ScriptSource> sources;
    for (int i = 0; i < 4; ++i)
        sources.push_back(vfs_unit_source(i));
    {
        auto pending = backend->begin_load_many(sources, 1);
        ASSERT_EQ(pending.size(), 4u);
        for (int i = 0; i < 4; ++i)
        {
            if (i == 2)
            {
                EXPECT_FALSE(pending[i]->ok);
                EXPECT_EQ(pending[i]->head.reason, hot::ReloadReason::CompileError);
            }
            else
                EXPECT_TRUE(pending[i]->ok) << pending[i]->head.error;
        }
    }
    EXPECT_EQ(count_entries(jobs_dir), 0u);
}

// T10: with a host file system each unit has its own mirror directory. None stays after finish.
TEST(BeginLoadMany, VfsMirrorsDifferAndAreGoneAfterFinish)
{
    SKIP_ON_WINDOWS();
    CleanEnv env;
    ScopedEnv jobs("HOT_JOBS", "4");
    TestScratch scratch;
    auto fs = std::make_shared<MemoryFs>();
    set_vfs_headers(*fs);
    auto backend = make_many_backend(scratch, "work", scratch.file("cache"), fs);
    const std::string jobs_dir = scratch.file("work") + "/vfs.job";
    std::vector<hot::ScriptSource> sources;
    for (int i = 0; i < 4; ++i)
        sources.push_back(vfs_unit_source(i));
    auto pending = backend->begin_load_many(sources, 1);
    ASSERT_EQ(pending.size(), 4u);
    for (auto& p : pending)
        ASSERT_TRUE(p->ok) << p->head.error;
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator(jobs_dir))
        names.insert(e.path().filename().string());
    EXPECT_EQ(names.size(), 4u);
    EXPECT_EQ(count_mirror_headers(jobs_dir), 4u);
    finish_all(*backend, pending);
    EXPECT_EQ(count_entries(jobs_dir), 0u);
}

} // namespace
