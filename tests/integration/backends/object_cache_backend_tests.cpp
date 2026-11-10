// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the object cache in the clang files backend (hot/object_cache.h). A new backend on the
// same cache directory finds the object file of an unchanged script and skips the compiler.
//
// CMake defines the macros HOT_SOURCE_DIR and HOT_CXX.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace
{

namespace fs = std::filesystem;

using test_support::kDt;
using test_support::Session;
using test_support::TestScratch;

// Sets an environment variable for the life of the object. The destructor restores the old value.
class EnvGuard
{
public:
    EnvGuard(const char* name, const char* value)
        : name_(name)
    {
        if (const char* old = std::getenv(name))
        {
            had_ = true;
            old_ = old;
        }
        test_support::set_env(name, value);
    }
    ~EnvGuard()
    {
        if (had_)
            test_support::set_env(name_.c_str(), old_.c_str());
        else
            test_support::unset_env(name_.c_str());
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    std::string name_;
    std::string old_;
    bool had_ = false;
};

// The tests must not depend on the environment of the caller.
struct CleanEnv
{
    CleanEnv()
        : cache("HOT_CACHE", "1")
        , max_mb("HOT_CACHE_MAX_MB", "")
    {
        test_support::unset_env("HOT_CACHE_MAX_MB");
    }
    EnvGuard cache;
    EnvGuard max_mb;
};

std::string script_text(const char* value)
{
    return std::string("using \"Actor.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
                       "    override void Tick(float dt) { this.result = ") +
           value + "; }\n}\n";
}

std::unique_ptr<hot::ClangFilesBackend> make_backend(const TestScratch& scratch, const std::string& work_name,
    const std::string& cache_dir, const std::string& commands_dir = std::string())
{
    hot::ClangFilesOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    o.compile_commands_dir = commands_dir;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

// Loads the script, makes a Calc object, runs one tick and gives the result field.
float load_and_run(hot::Backend& backend, const hot::ScriptSource& source)
{
    hot::LoadResult r = backend.load(source, 1);
    if (!r)
    {
        ADD_FAILURE() << r.error;
        return -1.0f;
    }
    Session s;
    s.start(std::move(*r.module), "Calc");
    if (!s.actor)
        return -1.0f;
    s.world.Tick(kDt);
    return *s.field("result");
}

size_t count_objects(const fs::path& cache_dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cache_dir / "obj", ec))
        if (e.path().extension() == ".o")
            ++n;
    return n;
}

const hot::ScriptSource kCalc7 = {"calc.hot", script_text("7.0"), {}, "Calc"};

// The first build is a miss. A new backend on the same directory gets a hit, and its module runs.
TEST(ObjectCacheBackend, SecondBackendHitsAndRuns)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto backend = make_backend(scratch, "work_a", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().misses, 1u);
        EXPECT_EQ(count_objects(cache), 1u);
    }
    auto backend = make_backend(scratch, "work_b", cache);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
    EXPECT_EQ(count_objects(cache), 1u);
}

// The statistics are available through the Backend interface, and the default is zero.
TEST(ObjectCacheBackend, StatsThroughTheBackendInterface)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto files = make_backend(scratch, "work_a", cache);
        hot::Backend& backend = *files;
        EXPECT_EQ(backend.cache_stats().hits, 0u);
        EXPECT_EQ(load_and_run(backend, kCalc7), 7.0f);
        EXPECT_EQ(backend.cache_stats().misses, 1u);
    }
    auto files = make_backend(scratch, "work_b", cache);
    hot::Backend& backend = *files;
    EXPECT_EQ(load_and_run(backend, kCalc7), 7.0f);
    EXPECT_EQ(backend.cache_stats().hits, 1u);
    EXPECT_EQ(backend.cache_stats().misses, 0u);
}

// A new script text is a miss and a new entry. The result is the new value.
TEST(ObjectCacheBackend, EditedScriptMisses)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto backend = make_backend(scratch, "work_a", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    auto backend = make_backend(scratch, "work_b", cache);
    EXPECT_EQ(load_and_run(*backend, {"calc.hot", script_text("9.0"), {}, "Calc"}), 9.0f);
    EXPECT_EQ(backend->cache_stats().hits, 0u);
    EXPECT_EQ(backend->cache_stats().misses, 1u);
    EXPECT_EQ(count_objects(cache), 2u);
}

// HOT_CACHE=0 turns the cache off: the build works and nothing goes under "obj/".
TEST(ObjectCacheBackend, EnvironmentTurnsTheCacheOff)
{
    CleanEnv env;
    EnvGuard off("HOT_CACHE", "0");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    auto backend = make_backend(scratch, "work_a", cache);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 0u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
    EXPECT_FALSE(fs::exists(fs::path(cache) / "obj"));
}

// A truncated object file does not link. The backend removes the entry, compiles again and links.
TEST(ObjectCacheBackend, DamagedObjectRecovers)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto backend = make_backend(scratch, "work_a", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    fs::path object;
    for (const auto& e : fs::directory_iterator(fs::path(cache) / "obj"))
        if (e.path().extension() == ".o")
            object = e.path();
    ASSERT_FALSE(object.empty());
    fs::resize_file(object, 16);
    auto backend = make_backend(scratch, "work_b", cache);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u); // the lookup found the entry
    ASSERT_TRUE(fs::exists(object)); // the compile stored the entry again
    EXPECT_GT(fs::file_size(object), 16u);
    auto again = make_backend(scratch, "work_c", cache);
    EXPECT_EQ(load_and_run(*again, kCalc7), 7.0f);
    EXPECT_EQ(again->cache_stats().hits, 1u);
}

// Two units with equal script text and different unit names. The unit name is in the translated
// text: the "#line" lines name "<generated_dir>/<unit>.cpp". So the texts differ, the keys differ
// and the units do not share an entry. Each unit has its own hit later.
TEST(ObjectCacheBackend, UnitNameIsPartOfTheKey)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const hot::ScriptSource a = {"calc.hot", script_text("7.0"), {}, "UnitA"};
    const hot::ScriptSource b = {"calc.hot", script_text("7.0"), {}, "UnitB"};
    {
        auto backend = make_backend(scratch, "work_a", cache);
        EXPECT_EQ(load_and_run(*backend, a), 7.0f);
        EXPECT_EQ(load_and_run(*backend, b), 7.0f);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().misses, 2u);
        EXPECT_EQ(count_objects(cache), 2u);
    }
    auto backend = make_backend(scratch, "work_b", cache);
    EXPECT_EQ(load_and_run(*backend, a), 7.0f);
    EXPECT_EQ(load_and_run(*backend, b), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 2u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
}

// Two backends (two processes in real use) have their own work dirs and share the cache dir and
// the generated dir. They build the same unit at the same time. The compile reads a private copy
// of the .cpp, so neither build sees a file of the other. The stable file stays for the IDE.
TEST(ObjectCacheBackend, SharedGeneratedDirIsSafe)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    auto first = make_backend(scratch, "work_a", cache);
    auto second = make_backend(scratch, "work_b", cache);
    hot::LoadResult r1;
    hot::LoadResult r2;
    std::thread t1([&] { r1 = first->load(kCalc7, 1); });
    std::thread t2([&] { r2 = second->load(kCalc7, 1); });
    t1.join();
    t2.join();
    ASSERT_TRUE(r1) << r1.error;
    ASSERT_TRUE(r2) << r2.error;
    for (hot::LoadResult* r : {&r1, &r2})
    {
        Session s;
        s.start(std::move(*r->module), "Calc");
        ASSERT_NE(s.actor, nullptr);
        s.world.Tick(kDt);
        EXPECT_EQ(*s.field("result"), 7.0f);
    }
    EXPECT_TRUE(fs::exists(fs::path(cache) / "src" / "Calc.cpp"));
    EXPECT_TRUE(fs::exists(fs::path(scratch.file("work_a")) / "Calc.cpp"));
    EXPECT_TRUE(fs::exists(fs::path(scratch.file("work_b")) / "Calc.cpp"));
}

// ---- compile_commands.json ----

std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

size_t count_text(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        ++n;
    return n;
}

// The JSON file has one entry for each unit. The array "arguments" has the compile command.
TEST(ObjectCacheBackend, CompileCommandsHasTheUnitEntry)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string db = scratch.file("db");
    auto backend = make_backend(scratch, "work_a", cache, db);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    const std::string json = read_file(fs::path(db) / "compile_commands.json");
    EXPECT_EQ(count_text(json, "\"file\":"), 1u);
    const fs::path stable = fs::absolute(fs::path(cache) / "src" / "Calc.cpp").lexically_normal();
    EXPECT_TRUE(fs::exists(stable));
    EXPECT_NE(json.find("\"file\": \"" + stable.string() + "\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"directory\": \"" + fs::absolute(cache).lexically_normal().string() + "\""),
        std::string::npos)
        << json;
    EXPECT_NE(json.find("\"-c\""), std::string::npos);
    EXPECT_EQ(count_text(json, "\"" + stable.string() + "\""), 2u); // "file" and the argument
    EXPECT_EQ(json.find("-MD"), std::string::npos);
    EXPECT_NE(json.find("\"output\": \"" + fs::absolute(cache).lexically_normal().string() + "/obj/"),
        std::string::npos)
        << json;
}

// A new backend (a new run) keeps the entries of the old run that it does not touch.
TEST(ObjectCacheBackend, CompileCommandsKeepsOldEntries)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string db = scratch.file("db");
    const hot::ScriptSource a = {"calc.hot", script_text("7.0"), {}, "UnitA"};
    const hot::ScriptSource b = {"calc.hot", script_text("8.0"), {}, "UnitB"};
    {
        auto backend = make_backend(scratch, "work_a", cache, db);
        EXPECT_EQ(load_and_run(*backend, a), 7.0f);
    }
    auto backend = make_backend(scratch, "work_b", cache, db);
    EXPECT_EQ(load_and_run(*backend, b), 8.0f);
    const std::string json = read_file(fs::path(db) / "compile_commands.json");
    EXPECT_EQ(count_text(json, "\"file\":"), 2u) << json;
    EXPECT_NE(json.find("UnitA.cpp"), std::string::npos);
    EXPECT_NE(json.find("UnitB.cpp"), std::string::npos);
}

// An entry whose file is gone does not stay. A damaged database file gives an empty start.
TEST(ObjectCacheBackend, CompileCommandsDropsMissingFiles)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string db = scratch.file("db");
    const hot::ScriptSource a = {"calc.hot", script_text("7.0"), {}, "UnitA"};
    const hot::ScriptSource b = {"calc.hot", script_text("8.0"), {}, "UnitB"};
    {
        auto backend = make_backend(scratch, "work_a", cache, db);
        EXPECT_EQ(load_and_run(*backend, a), 7.0f);
        EXPECT_EQ(load_and_run(*backend, b), 8.0f);
    }
    EXPECT_EQ(count_text(read_file(fs::path(db) / "compile_commands.json"), "\"file\":"), 2u);
    fs::remove(fs::path(cache) / "src" / "UnitA.cpp");
    {
        auto backend = make_backend(scratch, "work_b", cache, db);
        EXPECT_EQ(load_and_run(*backend, {"calc.hot", script_text("9.0"), {}, "UnitC"}), 9.0f);
    }
    const std::string json = read_file(fs::path(db) / "compile_commands.json");
    EXPECT_EQ(count_text(json, "\"file\":"), 2u) << json;
    EXPECT_EQ(json.find("UnitA.cpp"), std::string::npos);
    {
        std::ofstream(fs::path(db) / "compile_commands.json", std::ios::binary) << "[ {\"file\": ";
    }
    auto backend = make_backend(scratch, "work_c", cache, db);
    EXPECT_EQ(load_and_run(*backend, b), 8.0f);
    EXPECT_EQ(count_text(read_file(fs::path(db) / "compile_commands.json"), "\"file\":"), 1u);
}

// The text does not change on a hit: the file keeps its time stamp.
TEST(ObjectCacheBackend, CompileCommandsIsNotRewrittenWhenSame)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string db = scratch.file("db");
    auto backend = make_backend(scratch, "work_a", cache, db);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    const fs::path json = fs::path(db) / "compile_commands.json";
    const auto before = fs::last_write_time(json);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(fs::last_write_time(json), before);
    // A new backend that builds the same unit does not write the file either.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto second = make_backend(scratch, "work_b", cache, db);
    EXPECT_EQ(load_and_run(*second, kCalc7), 7.0f);
    EXPECT_EQ(fs::last_write_time(json), before);
}

// An empty option writes nothing.
TEST(ObjectCacheBackend, CompileCommandsOffByDefault)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    auto backend = make_backend(scratch, "work_a", cache);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_FALSE(fs::exists(fs::path(cache) / "compile_commands.json"));
}

hot::DefaultBackendOptions default_options(const TestScratch& scratch, const std::string& commands_dir)
{
    hot::DefaultBackendOptions o;
    o.kind = hot::BackendKind::ClangFiles;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine"};
    o.work_dir = scratch.file("default_work");
    o.cache_dir = scratch.file("default_cache");
    o.compile_commands_dir = commands_dir;
    return o;
}

// The variable HOT_COMPILE_COMMANDS_DIR decides over the option. An empty value turns it off.
TEST(ObjectCacheBackend, CompileCommandsEnvironmentOverridesTheOption)
{
    if (!hot::backend_available(hot::BackendKind::ClangFiles))
        GTEST_SKIP();
    CleanEnv env;
    EnvGuard cache_dir("HOT_CACHE_DIR", "");
    test_support::unset_env("HOT_CACHE_DIR");
    TestScratch scratch;
    const std::string option_dir = scratch.file("option_db");
    const std::string env_dir = scratch.file("env_db");
    {
        EnvGuard guard("HOT_COMPILE_COMMANDS_DIR", env_dir.c_str());
        auto backend = hot::make_default_backend(default_options(scratch, option_dir));
        hot::LoadResult r = backend->load(kCalc7, 1);
        ASSERT_TRUE(r) << r.error;
    }
    EXPECT_TRUE(fs::exists(fs::path(env_dir) / "compile_commands.json"));
    EXPECT_FALSE(fs::exists(fs::path(option_dir) / "compile_commands.json"));
    {
        EnvGuard guard("HOT_COMPILE_COMMANDS_DIR", "");
        auto backend = hot::make_default_backend(default_options(scratch, option_dir));
        hot::LoadResult r = backend->load(kCalc7, 1);
        ASSERT_TRUE(r) << r.error;
    }
    EXPECT_FALSE(fs::exists(fs::path(option_dir) / "compile_commands.json"));
    {
        test_support::unset_env("HOT_COMPILE_COMMANDS_DIR");
        auto backend = hot::make_default_backend(default_options(scratch, option_dir));
        hot::LoadResult r = backend->load(kCalc7, 1);
        ASSERT_TRUE(r) << r.error;
    }
    EXPECT_TRUE(fs::exists(fs::path(option_dir) / "compile_commands.json"));
}

} // namespace
