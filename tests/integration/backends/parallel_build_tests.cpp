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
#include "hot/vfs.h"
#include "support/compiler_wrapper.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
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

} // namespace
