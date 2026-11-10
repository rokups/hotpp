// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the object cache in the clang files backend (hot/object_cache.h). A new backend on the
// same cache directory finds the object file of an unchanged script and skips the compiler.
//
// CMake defines the macros HOT_SOURCE_DIR and HOT_CXX.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/runtime/process.h"
#include "hot/vfs.h"
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
#include <vector>

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
        , link_cache("HOT_LINK_CACHE", "1")
        , max_mb("HOT_CACHE_MAX_MB", "")
    {
        test_support::unset_env("HOT_CACHE_MAX_MB");
    }
    EnvGuard cache;
    EnvGuard link_cache;
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

std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
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

// The generation is not in the text. A build of the same script at a later generation is a hit.
TEST(ObjectCacheBackend, LaterGenerationHitsTheObjectCache)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto backend = make_backend(scratch, "work_a", cache);
        hot::LoadResult r = backend->load(kCalc7, 1);
        ASSERT_TRUE(r) << r.error;
        EXPECT_EQ(backend->cache_stats().misses, 1u);
    }
    auto backend = make_backend(scratch, "work_b", cache);
    hot::LoadResult r = backend->load(kCalc7, 2);
    ASSERT_TRUE(r) << r.error;
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
    EXPECT_EQ(count_objects(cache), 1u);
    Session s;
    s.start(std::move(*r.module), "Calc");
    ASSERT_TRUE(s.actor);
    s.world.Tick(kDt);
    EXPECT_EQ(*s.field("result"), 7.0f);
    EXPECT_EQ(s.loader.stats().active_generation, 2u);
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

// An object file with bad bytes does not link. The backend removes the entry, compiles again and
// links. The bad file has the size and the time of the good file, so the lookup is a hit.
TEST(ObjectCacheBackend, DamagedObjectRecovers)
{
    CleanEnv env;
    EnvGuard no_link_cache("HOT_LINK_CACHE", "0"); // else the module hit skips the link
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
    const uintmax_t object_size = fs::file_size(object);
    const auto object_time = fs::last_write_time(object);
    std::ofstream(object, std::ios::binary | std::ios::trunc) << std::string(object_size, 'x');
    fs::last_write_time(object, object_time);
    auto backend = make_backend(scratch, "work_b", cache);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u); // the lookup found the entry
    ASSERT_TRUE(fs::exists(object)); // the compile stored the entry again
    EXPECT_NE(read_file(object), std::string(object_size, 'x'));
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

// Two backends (two processes) run at the same time and add different units. Each write reads
// the file again and merges it, so the file has the entries of both.
TEST(ObjectCacheBackend, CompileCommandsMergesTwoLiveBackends)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string db = scratch.file("db");
    const hot::ScriptSource a = {"calc.hot", script_text("7.0"), {}, "UnitA"};
    const hot::ScriptSource b = {"calc.hot", script_text("8.0"), {}, "UnitB"};
    const hot::ScriptSource c = {"calc.hot", script_text("9.0"), {}, "UnitC"};
    auto first = make_backend(scratch, "work_a", cache, db);
    auto second = make_backend(scratch, "work_b", cache, db);
    EXPECT_EQ(load_and_run(*first, a), 7.0f);
    EXPECT_EQ(load_and_run(*second, b), 8.0f);
    EXPECT_EQ(load_and_run(*first, c), 9.0f);
    const std::string json = read_file(fs::path(db) / "compile_commands.json");
    EXPECT_EQ(count_text(json, "\"file\":"), 3u) << json;
    EXPECT_NE(json.find("UnitA.cpp"), std::string::npos);
    EXPECT_NE(json.find("UnitB.cpp"), std::string::npos);
    EXPECT_NE(json.find("UnitC.cpp"), std::string::npos);
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

// ---- the module cache ----

size_t count_modules(const fs::path& cache_dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cache_dir / "obj", ec))
        if (e.path().extension() == ".so")
            ++n;
    return n;
}

#ifndef _WIN32
// A compiler wrapper. It appends its arguments to a log and runs the real compiler.
std::string make_wrapper(const TestScratch& scratch, const std::string& log)
{
    const std::string path = scratch.file("cxx.sh");
    std::ofstream(path, std::ios::binary) << "#!/bin/sh\necho \"$*\" >> '" << log << "'\nexec " << HOT_CXX << " \"$@\"\n";
    fs::permissions(path, fs::perms::owner_all);
    return path;
}

std::unique_ptr<hot::ClangFilesBackend> make_wrapped_backend(const TestScratch& scratch, const std::string& work_name,
    const std::string& cache_dir, const std::string& compiler, const std::vector<std::string>& extra_flags = {})
{
    hot::ClangFilesOptions o;
    o.compiler = compiler;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    o.extra_flags = extra_flags;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

size_t count_runs(const std::string& log, const std::string& what)
{
    return count_text(read_file(log), what);
}

// A new backend on the same cache directory copies the linked module: no compile and no link
// process runs. The wrapper log shows only the "--version" runs.
TEST(ObjectCacheBackend, ModuleHitRunsNoLinkProcess)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string log = scratch.file("runs.log");
    const std::string cxx = make_wrapper(scratch, log);
    {
        auto backend = make_wrapped_backend(scratch, "work_a", cache, cxx);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
        EXPECT_EQ(backend->cache_stats().module_misses, 1u);
        EXPECT_EQ(count_modules(cache), 1u);
    }
    EXPECT_EQ(count_runs(log, "-shared"), 1u);
    EXPECT_EQ(count_runs(log, " -c "), 1u);
    auto backend = make_wrapped_backend(scratch, "work_b", cache, cxx);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().module_hits, 1u);
    EXPECT_EQ(backend->cache_stats().module_misses, 0u);
    EXPECT_EQ(count_runs(log, "-shared"), 1u); // no new link
    EXPECT_EQ(count_runs(log, " -c "), 1u); // no new compile
    EXPECT_EQ(count_modules(cache), 1u);
}

// A changed link input is a module miss. A change of the flags changes the object too, so the
// test checks the whole chain: the new build compiles and links, and a third run hits.
TEST(ObjectCacheBackend, ChangedFlagIsAModuleMiss)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string log = scratch.file("runs.log");
    const std::string cxx = make_wrapper(scratch, log);
    {
        auto backend = make_wrapped_backend(scratch, "work_a", cache, cxx);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    {
        auto backend = make_wrapped_backend(scratch, "work_b", cache, cxx, {"-Wno-unused-variable"});
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
        EXPECT_EQ(backend->cache_stats().module_hits, 0u);
        EXPECT_EQ(backend->cache_stats().module_misses, 1u);
    }
    EXPECT_EQ(count_runs(log, "-shared"), 2u);
    EXPECT_EQ(count_modules(cache), 2u);
    auto backend = make_wrapped_backend(scratch, "work_c", cache, cxx, {"-Wno-unused-variable"});
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().module_hits, 1u);
    EXPECT_EQ(count_runs(log, "-shared"), 2u);
}

// HOT_LINK_CACHE=0 turns only the module cache off: the object cache works, the link runs.
TEST(ObjectCacheBackend, LinkCacheEnvironmentTurnsOnlyThatLevelOff)
{
    CleanEnv env;
    EnvGuard off("HOT_LINK_CACHE", "0");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string log = scratch.file("runs.log");
    const std::string cxx = make_wrapper(scratch, log);
    {
        auto backend = make_wrapped_backend(scratch, "work_a", cache, cxx);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    auto backend = make_wrapped_backend(scratch, "work_b", cache, cxx);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u); // the object cache works
    EXPECT_EQ(backend->cache_stats().module_hits, 0u);
    EXPECT_EQ(backend->cache_stats().module_misses, 0u);
    EXPECT_EQ(count_modules(cache), 0u);
    EXPECT_EQ(count_runs(log, "-shared"), 2u);
    EXPECT_EQ(count_runs(log, " -c "), 1u);
}
#endif

// A cached .so of the right size and time with bad bytes does not load. The backend removes the entry,
// links again and stores a good module. A later backend hits that module.
TEST(ObjectCacheBackend, CorruptModuleRecovers)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto backend = make_backend(scratch, "work_a", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    fs::path module;
    for (const auto& e : fs::directory_iterator(fs::path(cache) / "obj"))
        if (e.path().extension() == ".so")
            module = e.path();
    ASSERT_FALSE(module.empty());
    const uintmax_t size = fs::file_size(module);
    const auto module_time = fs::last_write_time(module);
    std::ofstream(module, std::ios::binary | std::ios::trunc) << std::string(size, 'x');
    fs::last_write_time(module, module_time);
    {
        auto backend = make_backend(scratch, "work_b", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
        EXPECT_EQ(backend->cache_stats().module_hits, 1u); // the bad file was a hit, then the load failed
        EXPECT_EQ(backend->cache_stats().module_misses, 0u); // the retry does not read the cache
    }
    ASSERT_TRUE(fs::exists(module));
    EXPECT_NE(fs::file_size(module), 0u);
    auto again = make_backend(scratch, "work_c", cache);
    EXPECT_EQ(load_and_run(*again, kCalc7), 7.0f);
    EXPECT_EQ(again->cache_stats().module_hits, 1u);
    EXPECT_EQ(again->cache_stats().module_misses, 0u);
}

// A header with a struct. The script gives the size of the struct.
void write_layout_header(const fs::path& path, int pad, std::chrono::minutes age)
{
    std::ofstream(path, std::ios::binary)
        << "struct LayoutProbe { char pad[" << pad << "]; };\n"
        << "inline float layout_size() { return static_cast<float>(sizeof(LayoutProbe)); }\n";
    // An old time: the cache does not store an object whose header is younger than the compile.
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

std::unique_ptr<hot::ClangFilesBackend> make_layout_backend(
    const TestScratch& scratch, const std::string& work_name, const std::string& cache_dir, const std::string& inc)
{
    hot::ClangFilesOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include", inc};
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

const hot::ScriptSource kLayout = {"calc.hot",
    "using \"Actor.h\";\nusing \"layout.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
    "    override void Tick(float dt) { this.result = layout_size(); }\n}\n",
    {}, "Calc"};

// The link cache is on. A changed struct in a header is a miss of the object and of the module:
// the module key has no header data, so a module hit needs a valid object entry.
TEST(ObjectCacheBackend, ChangedHeaderLayoutMissesTheModule)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const fs::path inc = scratch.file("inc");
    fs::create_directories(inc);
    write_layout_header(inc / "layout.h", 4, std::chrono::minutes(60));
    {
        auto backend = make_layout_backend(scratch, "work_a", cache, inc.string());
        EXPECT_EQ(load_and_run(*backend, kLayout), 4.0f);
        EXPECT_EQ(count_modules(cache), 1u);
    }
    write_layout_header(inc / "layout.h", 16, std::chrono::minutes(30));
    {
        auto backend = make_layout_backend(scratch, "work_b", cache, inc.string());
        EXPECT_EQ(load_and_run(*backend, kLayout), 16.0f);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().module_hits, 0u);
    }
    auto backend = make_layout_backend(scratch, "work_c", cache, inc.string());
    EXPECT_EQ(load_and_run(*backend, kLayout), 16.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().module_hits, 1u);
}

// A new "layout.h" in an earlier include dir has the same declarations, so the translate gives
// the same text. The inline body is different, so the object entry of the old header is wrong.
TEST(ObjectCacheBackend, NewShadowHeaderMissesTheObject)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const fs::path early = scratch.file("early");
    const fs::path inc = scratch.file("inc");
    fs::create_directories(early);
    fs::create_directories(inc);
    write_layout_header(inc / "layout.h", 4, std::chrono::minutes(60));
    const auto make = [&](const std::string& work)
    {
        hot::ClangFilesOptions o;
        o.compiler = HOT_CXX;
        o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include",
            early.string(), inc.string()};
        o.work_dir = scratch.file(work);
        o.cache_dir = cache;
        return std::make_unique<hot::ClangFilesBackend>(std::move(o));
    };
    {
        auto backend = make("work_a");
        EXPECT_EQ(load_and_run(*backend, kLayout), 4.0f);
    }
    write_layout_header(early / "layout.h", 16, std::chrono::minutes(30));
    {
        auto backend = make("work_b");
        EXPECT_EQ(load_and_run(*backend, kLayout), 16.0f);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().module_hits, 0u);
    }
    auto backend = make("work_c");
    EXPECT_EQ(load_and_run(*backend, kLayout), 16.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().translate_hits, 1u);
}

// The compiler reads CPATH and other variables. A changed variable changes the key: a miss.
TEST(ObjectCacheBackend, CompilerEnvironmentIsPartOfTheKey)
{
    CleanEnv env;
    EnvGuard cpath("CPATH", "");
    test_support::unset_env("CPATH");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const fs::path extra = scratch.file("extra_inc");
    fs::create_directories(extra);
    {
        auto backend = make_backend(scratch, "work_a", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    {
        EnvGuard changed("CPATH", extra.string().c_str());
        auto backend = make_backend(scratch, "work_b", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().misses, 1u);
    }
    auto backend = make_backend(scratch, "work_c", cache);
    EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
}

// The MSVC tools and the Apple targets read more variables. Each one changes the object key.
TEST(ObjectCacheBackend, ToolchainVariablesArePartOfTheObjectKey)
{
    CleanEnv env;
    const char* const names[] = {"INCLUDE", "EXTERNAL_INCLUDE", "LIB", "VCToolsInstallDir", "VCINSTALLDIR",
        "WindowsSdkDir", "WindowsSDKVersion", "IPHONEOS_DEPLOYMENT_TARGET", "TVOS_DEPLOYMENT_TARGET",
        "WATCHOS_DEPLOYMENT_TARGET", "XROS_DEPLOYMENT_TARGET"};
    std::vector<std::unique_ptr<EnvGuard>> saved;
    for (const char* name : names)
    {
        saved.push_back(std::make_unique<EnvGuard>(name, ""));
        test_support::unset_env(name);
    }
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    {
        auto backend = make_backend(scratch, "work_base", cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
    }
    for (const char* name : names)
    {
        EnvGuard changed(name, "hot-test-value");
        auto backend = make_backend(scratch, std::string("work_") + name, cache);
        EXPECT_EQ(load_and_run(*backend, kCalc7), 7.0f);
        EXPECT_EQ(backend->cache_stats().hits, 0u) << name;
        EXPECT_EQ(backend->cache_stats().misses, 1u) << name;
    }
}

// A header that is younger than the start of the compile can change during the compile. The
// build works, but the cache has no object entry and no module entry for it.
TEST(ObjectCacheBackend, YoungHeaderIsNotCached)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const fs::path inc = scratch.file("inc");
    fs::create_directories(inc);
    write_layout_header(inc / "layout.h", 8, std::chrono::minutes(0));
    {
        auto backend = make_layout_backend(scratch, "work_a", cache, inc.string());
        EXPECT_EQ(load_and_run(*backend, kLayout), 8.0f);
    }
    EXPECT_EQ(count_objects(cache), 0u);
    EXPECT_EQ(count_modules(cache), 0u);
    auto backend = make_layout_backend(scratch, "work_b", cache, inc.string());
    EXPECT_EQ(load_and_run(*backend, kLayout), 8.0f);
    EXPECT_EQ(backend->cache_stats().hits, 0u);
    EXPECT_EQ(backend->cache_stats().module_hits, 0u);
}

// A file system that has no file. It turns on the mirror directory "<work_dir>/vfs".
class EmptyFs final : public hot::FileSystem
{
public:
    bool read(const std::string&, std::string&) override { return false; }
    hot::VfsStat stat(const std::string&) override { return {}; }
};

// The mirror directory belongs to the process. The database has no "-I" argument under it.
TEST(ObjectCacheBackend, CompileCommandsHasNoMirrorDirectory)
{
    CleanEnv env;
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string db = scratch.file("db");
    hot::ClangFilesOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file("work_a");
    o.cache_dir = cache;
    o.compile_commands_dir = db;
    o.fs = std::make_shared<EmptyFs>();
    hot::ClangFilesBackend backend(std::move(o));
    EXPECT_EQ(load_and_run(backend, kCalc7), 7.0f);
    const std::string json = read_file(fs::path(db) / "compile_commands.json");
    EXPECT_EQ(json.find("/vfs/"), std::string::npos) << json;
    EXPECT_EQ(json.find(scratch.file("work_a")), std::string::npos) << json;
    EXPECT_NE(json.find("-I" + std::string(HOT_SOURCE_DIR) + "/tests/engine"), std::string::npos) << json;
}

// The directories "p<pid>" of dead processes go at the creation of a backend. A directory has
// the file "owner" (the host and the pid namespace). Only a directory of the same owner goes: the
// pid of another host or namespace says nothing. A directory without an owner file goes only when
// it is older than one hour. A directory of another owner goes when it is older than one day. The
// directory of a running process (pid 1 here), a name that is not "p<digits>" and a file stay.
TEST(ObjectCacheBackend, StaleWorkDirectoriesGoAndLiveOnesStay)
{
#ifdef _WIN32
    GTEST_SKIP() << "no cleanup on Windows";
#else
    CleanEnv env;
    EnvGuard keep("HOT_KEEP_TEMP", "");
    TestScratch scratch;
    const fs::path base = scratch.file("default_work");
    auto first = hot::make_default_backend(default_options(scratch, std::string()));
    ASSERT_TRUE(first);
    const fs::path own_owner = base / ("p" + std::to_string(hot::detail::process_id())) / "owner";
    ASSERT_TRUE(fs::exists(own_owner));
    const std::string owner = read_file(own_owner);
    EXPECT_FALSE(owner.empty());
    for (const char* name : {"p2000000000", "p2000000003", "p2000000004", "p2000000005", "p2000000006", "p1",
             "pabc", "q2000000001", "p"})
    {
        fs::create_directories(base / name / "vfs");
        std::ofstream(base / name / "x.txt") << "x";
    }
    for (const char* name : {"p2000000000", "p1"})
        std::ofstream(base / name / "owner", std::ios::binary) << owner;
    std::ofstream(base / "p2000000003" / "owner", std::ios::binary) << "another-host\npid:[1]\n";
    std::ofstream(base / "p2000000006" / "owner", std::ios::binary) << "another-host\npid:[1]\n";
    fs::last_write_time(base / "p2000000003", fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(base / "p2000000005", fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(base / "p2000000006", fs::file_time_type::clock::now() - std::chrono::hours(25));
    std::ofstream(base / "p2000000002") << "a file";
    {
        auto backend = hot::make_default_backend(default_options(scratch, std::string()));
        ASSERT_TRUE(backend);
        EXPECT_FALSE(fs::exists(base / "p2000000000")); // dead, the same owner
        EXPECT_TRUE(fs::exists(base / "p2000000003" / "x.txt")); // another owner, young
        EXPECT_FALSE(fs::exists(base / "p2000000006")); // another owner, old
        EXPECT_TRUE(fs::exists(base / "p2000000004" / "x.txt")); // no owner, young
        EXPECT_FALSE(fs::exists(base / "p2000000005")); // no owner, old
        EXPECT_TRUE(fs::exists(base / "p1" / "x.txt")); // running (init)
        EXPECT_TRUE(fs::exists(base / "pabc"));
        EXPECT_TRUE(fs::exists(base / "q2000000001"));
        EXPECT_TRUE(fs::exists(base / "p"));
        EXPECT_TRUE(fs::exists(base / "p2000000002")); // not a directory
    }
#endif
}

} // namespace
