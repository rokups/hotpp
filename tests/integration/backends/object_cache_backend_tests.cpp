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
        , direct_link("HOT_DIRECT_LINK", "0") // the tests count the runs of the compiler (the driver links)
    {
        test_support::unset_env("HOT_CACHE_MAX_MB");
    }
    EnvGuard cache;
    EnvGuard link_cache;
    EnvGuard max_mb;
    EnvGuard direct_link;
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

// ---- the fmt code of the host (-UFMT_HEADER_ONLY)

namespace
{

// A clang compiler with the PCH on must build the shape PCH: a missing PCH is a failure.
bool shape_pch_expected()
{
    const char* pch = std::getenv("HOT_PCH");
    return std::string(HOT_CXX).find("clang") != std::string::npos && !(pch && std::string(pch) == "0");
}

hot::ClangFilesOptions fmt_options(TestScratch& scratch, const std::string& name, const std::vector<std::string>& flags)
{
    hot::ClangFilesOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(name + "_work");
    o.compile_commands_dir = scratch.file(name + "_db");
    o.cache_dir = scratch.file(name + "_cache"); // the database is written with a cache
    o.extra_flags = flags;
    o.host_fmt_probe = [] { return true; }; // the tests act as a host with the fmt code
    return o;
}

} // namespace

// The compile gets -UFMT_HEADER_ONLY after all flags when the host flags have -DFMT_SHARED (ELF). The
// override HOT_FMT_HEADER_ONLY=1 and the sticky flag keep the own fmt code. Other targets are unchanged.
TEST(ObjectCacheBackend, HostFmtFlagRule)
{
    CleanEnv env;
    TestScratch scratch;
    auto last_flag = [](const hot::ClangFilesOptions& o) {
        const std::vector<std::string> cmd = hot::compile_object_command(o, "/x.cpp", "/x.o", "/x.d");
        auto it = std::find(cmd.begin(), cmd.end(), "-MD"); // the flags end before the depfile flags
        return it == cmd.begin() ? std::string() : *(it - 1);
    };
    hot::ClangFilesOptions shared = fmt_options(scratch, "r", {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"});
    hot::ClangFilesOptions plain = fmt_options(scratch, "p", {"-DFMT_HEADER_ONLY"});
#if defined(__linux__)
    EXPECT_EQ(last_flag(shared), "-UFMT_HEADER_ONLY");
    EXPECT_NE(last_flag(plain), "-UFMT_HEADER_ONLY"); // no FMT_SHARED: unchanged
    shared.extra_flags[0] = "-DFMT_SHARED=1"; // a define with a value counts too
    EXPECT_EQ(last_flag(shared), "-UFMT_HEADER_ONLY");
    {
        EnvGuard force("HOT_FMT_HEADER_ONLY", "1");
        EXPECT_NE(last_flag(shared), "-UFMT_HEADER_ONLY");
    }
    shared.fmt_header_only = std::make_shared<std::atomic<bool>>(true); // the sticky flag
    EXPECT_NE(last_flag(shared), "-UFMT_HEADER_ONLY");
    // A target that the check of the fmt symbols cannot read keeps the own fmt copy.
    shared.extra_flags[0] = "-DFMT_SHARED";
    shared.target_triple = "i686-linux-gnu";
    EXPECT_TRUE(hot::fmt_header_only(shared));
    shared.target_triple = "x86_64-w64-mingw32";
    EXPECT_TRUE(hot::fmt_header_only(shared));
    shared.target_triple = "aarch64-linux-gnu";
    shared.fmt_header_only.reset(); // the sticky flag of the checks above is off
    EXPECT_FALSE(hot::fmt_header_only(shared));
#else
    EXPECT_NE(last_flag(shared), "-UFMT_HEADER_ONLY");
#endif
}

// A host without the fmt code: the backend turns the own fmt copy on before the first compile.
TEST(ObjectCacheBackend, HostWithoutFmtCodeUsesTheOwnCopyFromTheStart)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "n", {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"});
    o.host_fmt_probe = [] { return false; };
    o.fmt_header_only = std::make_shared<std::atomic<bool>>(false);
    hot::ClangFilesBackend backend(o);
    EXPECT_TRUE(o.fmt_header_only->load());
    EXPECT_TRUE(hot::fmt_header_only(o));
    EXPECT_EQ(load_and_run(backend, kCalc7), 7.0f);
    EXPECT_FALSE(hot::host_exports_fmt("")); // the test program has no fmt code
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The check of the undefined fmt symbols: a module with an fmt symbol that no library has is found.
TEST(ObjectCacheBackend, MissingFmtSymbolCheckReadsTheModule)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    const fs::path dir = scratch.file("sym");
    fs::create_directories(dir);
    std::ofstream(dir / "bad.cpp") << "namespace fmt { inline namespace v10 { void hot_no_such_fmt_symbol(); } }\n"
                                      "extern \"C\" void hot_use() { fmt::v10::hot_no_such_fmt_symbol(); }\n";
    std::ofstream(dir / "good.cpp") << "#include <cstdio>\nextern \"C\" void hot_use() { std::puts(\"x\"); }\n";
    for (const char* n : {"bad", "good"})
    {
        const std::string cmd = std::string(HOT_CXX) + " -shared -fPIC -o " + (dir / (std::string(n) + ".so")).string()
            + " " + (dir / (std::string(n) + ".cpp")).string();
        ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
    }
    std::string name;
    EXPECT_TRUE(hot::has_missing_fmt_symbol((dir / "bad.so").string(), &name));
    EXPECT_NE(name.find("hot_no_such_fmt_symbol"), std::string::npos) << name;
    EXPECT_FALSE(hot::has_missing_fmt_symbol((dir / "good.so").string()));
    EXPECT_FALSE(hot::has_missing_fmt_symbol((dir / "missing.so").string()));
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The module compiled with -UFMT_HEADER_ONLY loads and runs, and the compile database has the flag.
TEST(ObjectCacheBackend, HostFmtModuleRuns)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "h", {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"});
    {
        hot::ClangFilesBackend backend(o);
        EXPECT_EQ(load_and_run(backend, kCalc7), 7.0f);
    } // the database is written when the backend ends
    const std::string json = read_file(fs::path(o.compile_commands_dir) / "compile_commands.json");
    EXPECT_NE(json.find("-UFMT_HEADER_ONLY"), std::string::npos) << json.substr(0, 1500);
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// A module that needs an fmt symbol the host lacks: the backend logs it, turns the own fmt code on
// (sticky) and compiles again. The module runs and the database has no -U flag.
TEST(ObjectCacheBackend, MissingFmtSymbolFallsBackToTheOwnFmtCode)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "f", {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"});
    o.fmt_header_only = std::make_shared<std::atomic<bool>>(false);
    int calls = 0;
    o.missing_fmt_symbol = [&](const std::string&, std::string* name) {
        if (++calls > 1)
            return false;
        *name = "_ZN3fmt3v10test";
        return true;
    };
    testing::internal::CaptureStderr();
    float result = 0;
    {
        hot::ClangFilesBackend backend(o);
        result = load_and_run(backend, kCalc7);
    } // the database is written when the backend ends
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, 7.0f);
    EXPECT_NE(log.find("needs the fmt symbol _ZN3fmt3v10test"), std::string::npos) << log;
    EXPECT_TRUE(o.fmt_header_only->load());
    EXPECT_EQ(calls, 1); // the second compile does not check: the own code is on
    const std::string json = read_file(fs::path(o.compile_commands_dir) / "compile_commands.json");
    EXPECT_NE(json.find("-DFMT_HEADER_ONLY"), std::string::npos) << json; // the database is not empty
    EXPECT_EQ(json.find("-UFMT_HEADER_ONLY"), std::string::npos) << json;
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// ---- the shape PCH of a .hot unit

// Edits of a method body give the right results with a shape PCH. A damaged shape PCH never fails a
// reload: the unit compiles whole and the PCH builds again.
TEST(ObjectCacheBackend, ShapePchKeepsResultsAndSurvivesDamage)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "s", {});
    const fs::path shape_dir = fs::path(o.cache_dir) / "pch" / "shape";
    auto edit_run = [&](int first, int count) {
        hot::ClangFilesBackend backend(o);
        for (int i = first; i < first + count; ++i)
            EXPECT_EQ(load_and_run(backend, {"calc.hot", script_text((std::to_string(i) + ".0").c_str()), {}, "Calc"}),
                static_cast<float>(i));
    }; // the destructor joins the PCH threads
    edit_run(1, 6);
    edit_run(7, 2);
    std::vector<fs::path> pchs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(shape_dir, ec))
        if (e.path().extension() == ".pch")
            pchs.push_back(e.path());
    if (pchs.empty())
    {
        ASSERT_FALSE(shape_pch_expected()) << "no shape PCH was built";
        GTEST_SKIP() << "no shape PCH was built (no clang or no PCH)";
    }
    for (const fs::path& p : pchs)
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "damaged";
    edit_run(20, 2);
    // A damaged PCH is no reason for a marker: the file is removed and builds again.
    for (const auto& e : fs::directory_iterator(shape_dir, ec))
        EXPECT_NE(e.path().extension(), ".nosplit");
#else
    GTEST_SKIP() << "ELF only";
#endif
}

namespace
{

size_t count_ext(const fs::path& dir, const char* ext)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ext)
            ++n;
    return n;
}

size_t count_text_in(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
        ++n;
    return n;
}

// Loads "count" edits of the Calc script, one backend. The destructor joins the PCH threads.
void run_edits(const hot::ClangFilesOptions& o, int first, int count, bool expect_ok = true)
{
    hot::ClangFilesBackend backend(o);
    for (int i = first; i < first + count; ++i)
    {
        const float r = load_and_run(backend, {"calc.hot", script_text((std::to_string(i) + ".0").c_str()), {}, "Calc"});
        if (expect_ok)
            EXPECT_EQ(r, static_cast<float>(i));
    }
}

} // namespace

// Interpolated text through the fmt flags. A test build has no host that exports fmt (HOT_USE_FMT is
// off: the script uses std::format). So the test checks the text with the host fmt flags on and
// proves the text on each path: host fmt, own fmt after the fallback, or std::format.
TEST(ObjectCacheBackend, InterpolatedTextIsRightWithTheHostFmtFlags)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "i", {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"});
    hot::ClangFilesBackend backend(o);
    const hot::ScriptSource src = {"calc.hot",
        "using <string>;\nusing \"Actor.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
        "    override void Tick(float dt) {\n"
        "        std.string s = \"n=${42} x=${1.5:.2f} i=${-7}\";\n"
        "        this.result = s == \"n=42 x=1.50 i=-7\" ? 1.0 : 0.0;\n    }\n}\n",
        {}, "Calc"};
    EXPECT_EQ(load_and_run(backend, src), 1.0f);
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// A syntax error in a body, then the fix: the shape PCH stays, is used again, and no marker exists.
TEST(ObjectCacheBackend, ErrorInABodyDoesNotDisableTheShapePch)
{
#if defined(__linux__)
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "e", {});
    const fs::path shape_dir = fs::path(o.cache_dir) / "pch" / "shape";
    testing::internal::CaptureStderr();
    run_edits(o, 1, 6);
    {
        hot::ClangFilesBackend backend(o);
        EXPECT_FALSE(backend.load({"calc.hot", script_text("1.0 +"), {}, "Calc"}, 1));
    }
    run_edits(o, 30, 2);
    const std::string log = testing::internal::GetCapturedStderr();
    if (count_ext(shape_dir, ".pch") == 0)
    {
        ASSERT_FALSE(shape_pch_expected()) << "no shape PCH was built";
        GTEST_SKIP() << "no shape PCH was built (no clang or no PCH)";
    }
    EXPECT_EQ(count_ext(shape_dir, ".nosplit"), 0u);
    EXPECT_EQ(count_ext(shape_dir, ".pch"), 1u);
    EXPECT_GE(count_text_in(log, "shape pch hit"), 2u) << log;
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// A base PCH change (a changed header) gives a new shape key. Each script keeps 2 shape PCH files.
// The first hit stores its object (the shape files are no input of the object).
TEST(ObjectCacheBackend, ShapePchFollowsTheBasePchAndKeepsTwoPerScript)
{
#if defined(__linux__)
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    const fs::path inc = scratch.file("eng");
    fs::copy(fs::path(HOT_SOURCE_DIR) / "tests" / "engine", inc, fs::copy_options::recursive);
    hot::ClangFilesOptions o = fmt_options(scratch, "k", {});
    o.include_dirs = {inc.string(), std::string(HOT_SOURCE_DIR) + "/include"};
    const fs::path shape_dir = fs::path(o.cache_dir) / "pch" / "shape";
    std::vector<size_t> counts;
    testing::internal::CaptureStderr();
    for (int phase = 0; phase < 3; ++phase)
    {
        std::ofstream(inc / "Actor.h", std::ios::app) << "\n// phase " << phase << "\n";
        run_edits(o, 10 * phase + 1, 6);
        counts.push_back(count_ext(shape_dir, ".pch"));
    }
    const std::string log = testing::internal::GetCapturedStderr();
    if (counts[0] == 0)
    {
        ASSERT_FALSE(shape_pch_expected()) << "no shape PCH was built";
        GTEST_SKIP() << "no shape PCH was built (no clang or no PCH)";
    }
    EXPECT_EQ(counts[0], 1u);
    EXPECT_EQ(counts[1], 2u); // a new key
    EXPECT_EQ(counts[2], 2u); // the oldest is removed
    EXPECT_EQ(count_text_in(log, "shape/"), 0u) << log; // the shape files are no input of the object
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The eviction works for each script and by the last use: many scripts never evict the PCH of
// the script in work; an old marker and old temp files go.
TEST(ObjectCacheBackend, ShapePchEvictionIsPerScriptAndByLastUse)
{
    TestScratch scratch;
    const fs::path dir = scratch.file("shape");
    fs::create_directories(dir);
    const auto now = fs::file_time_type::clock::now();
    auto make = [&](const std::string& stem, std::chrono::hours age) {
        for (const char* ext : {".pch", ".h"})
        {
            std::ofstream(dir / (stem + ext)) << "x";
            fs::last_write_time(dir / (stem + ext), now - age);
        }
    };
    for (int u = 0; u < 12; ++u)
        for (int k = 0; k < 3; ++k)
            make("u" + std::to_string(u) + "-k" + std::to_string(k), std::chrono::hours(1 + u * 3 + k));
    std::ofstream(dir / "u0-old.nosplit") << "x";
    fs::last_write_time(dir / "u0-old.nosplit", now - std::chrono::hours(30));
    std::ofstream(dir / "x.pch.tmp") << "x";
    fs::last_write_time(dir / "x.pch.tmp", now - std::chrono::hours(3));
    hot::evict_shape_files(dir.string(), "u11", 2, 100);
    EXPECT_EQ(count_ext(dir, ".pch"), 35u); // u11 keeps 2 of 3
    EXPECT_TRUE(fs::exists(dir / "u11-k0.pch"));
    EXPECT_TRUE(fs::exists(dir / "u11-k1.pch"));
    EXPECT_FALSE(fs::exists(dir / "u11-k2.pch"));
    EXPECT_FALSE(fs::exists(dir / "u11-k2.h"));
    EXPECT_FALSE(fs::exists(dir / "u0-old.nosplit"));
    EXPECT_FALSE(fs::exists(dir / "x.pch.tmp"));
    hot::evict_shape_files(dir.string(), "u0", 2, 8); // the global limit: the 8 newest remain
    EXPECT_EQ(count_ext(dir, ".pch"), 7u); // the 8 newest, and u0 has only 2 of its 3
    EXPECT_TRUE(fs::exists(dir / "u0-k0.pch")); // the newest of all
}

// The module from a shape PCH hit exports the same symbols as the module of the whole unit.
TEST(ObjectCacheBackend, ShapePchModuleHasTheSameExports)
{
#if defined(__linux__)
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    auto exports_of = [&](const std::string& tag, bool shape) {
        TestScratch scratch;
        EnvGuard off("HOT_SHAPE_PCH", shape ? "1" : "0");
        hot::ClangFilesOptions o = fmt_options(scratch, tag, {});
        testing::internal::CaptureStderr();
        run_edits(o, 1, 6);
        const std::string log = testing::internal::GetCapturedStderr();
        const bool hit = log.find("shape pch hit") != std::string::npos;
        std::string newest;
        fs::file_time_type best{};
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(o.work_dir, ec))
            if (e.path().extension() == ".so" && (newest.empty() || e.last_write_time(ec) > best))
            {
                newest = e.path().string();
                best = e.last_write_time(ec);
            }
        std::string out;
        if (!newest.empty())
        {
            std::FILE* p = popen(("nm -D --defined-only " + newest + " | awk '{print $2 \" \" $3}' | sort").c_str(), "r");
            char buf[512];
            while (p && std::fgets(buf, sizeof buf, p))
                out += buf;
            if (p)
                pclose(p);
        }
        return std::make_pair(hit, out);
    };
    const auto whole = exports_of("w", false);
    const auto split = exports_of("p", true);
    if (!split.first)
    {
        ASSERT_FALSE(shape_pch_expected()) << "no shape PCH hit";
        GTEST_SKIP() << "no shape PCH hit (no clang or no PCH)";
    }
    EXPECT_FALSE(whole.first);
    ASSERT_FALSE(whole.second.empty());
    EXPECT_EQ(whole.second, split.second);
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// Two processes (two work dirs, one cache dir): the second one hits the shape PCH of the first, and a
// third one loads the same text from the object cache (the store of the first hit worked).
TEST(ObjectCacheBackend, ShapePchAndObjectsAreSharedBetweenProcesses)
{
#if defined(__linux__)
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions first = fmt_options(scratch, "x1", {});
    hot::ClangFilesOptions second = fmt_options(scratch, "x2", {});
    hot::ClangFilesOptions third = fmt_options(scratch, "x3", {});
    second.cache_dir = third.cache_dir = first.cache_dir;
    const fs::path shape_dir = fs::path(first.cache_dir) / "pch" / "shape";
    run_edits(first, 1, 6);
    if (count_ext(shape_dir, ".pch") == 0)
    {
        ASSERT_FALSE(shape_pch_expected()) << "no shape PCH was built";
        GTEST_SKIP() << "no shape PCH (no clang or no PCH)";
    }
    testing::internal::CaptureStderr();
    run_edits(second, 50, 1);
    run_edits(third, 50, 1);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("shape pch hit"), std::string::npos) << log;
    EXPECT_TRUE(log.find("cache hit") != std::string::npos || log.find("link cached") != std::string::npos) << log;
    EXPECT_EQ(count_text_in(log, "shape/"), 0u) << log;
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// ---- two objects for a .hot unit

namespace
{

// The newest .so of the work dir and the number of defined symbols (nm, whole symbol table) whose
// name ends with "suffix".
size_t count_defined_symbols(const fs::path& work_dir, const std::string& suffix)
{
    std::string newest;
    fs::file_time_type best{};
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(work_dir, ec))
        if (e.path().extension() == ".so" && (newest.empty() || e.last_write_time(ec) > best))
        {
            newest = e.path().string();
            best = e.last_write_time(ec);
        }
    if (newest.empty())
        return 0;
    size_t n = 0;
    std::FILE* p = popen(("nm --defined-only " + newest).c_str(), "r");
    char buf[1024];
    while (p && std::fgets(buf, sizeof buf, p))
    {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.size() > suffix.size() && line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0)
            ++n;
    }
    if (p)
        pclose(p);
    return n;
}

} // namespace

// A script with classes, lambdas and handles is edited 8 times in one cache: from the third edit the
// unit uses two objects. Each run gives the right fields (class, state, lambda calls), and the module
// has one copy of the unit state (GEN, hot_live, the slot of the module).
TEST(ObjectCacheBackend, TwoObjectsRunAClassScriptWithLambdas)
{
#if defined(__linux__)
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld: the two object split is off";
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    std::ifstream in(std::string(HOT_SOURCE_DIR) + "/tests/scripts/value_use_v1.hot");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string original = ss.str();
    const std::string tick = "override void Tick(float dt) {";
    ASSERT_NE(original.find(tick), std::string::npos);
    hot::ClangFilesOptions o = fmt_options(scratch, "t", {});
    const fs::path shape_dir = fs::path(o.cache_dir) / "pch" / "shape";
    testing::internal::CaptureStderr();
    {
        hot::ClangFilesBackend backend(o);
        for (int i = 1; i <= 8; ++i)
        {
            std::string text = original;
            text.insert(text.find(tick) + tick.size(), "\n        int hot_edit = " + std::to_string(i) + ";");
            hot::LoadResult r = backend.load({"value_use.hot", text, {}, "Holder"}, 1);
            ASSERT_TRUE(static_cast<bool>(r)) << r.error;
            Session s;
            s.start(std::move(*r.module), "Holder");
            ASSERT_NE(s.actor, nullptr);
            s.world.Tick(kDt);
            auto field = [&](const char* name) { return *hot::field_ptr<int>(s.inst.shell, name); };
            EXPECT_EQ(field("r_a"), 10);
            EXPECT_EQ(field("r_b"), 20);
            EXPECT_EQ(field("r_lam"), 3);
            EXPECT_EQ(field("r_cap"), 5);
            EXPECT_EQ(field("r_var"), 10);
        }
    }
    const std::string log = testing::internal::GetCapturedStderr();
    // Two objects are expected whenever the compiler has a shape PCH and ld.lld exists.
    if (shape_pch_expected())
        ASSERT_NE(log.find("hit, two objects"), std::string::npos) << log;
    EXPECT_EQ(log.find("does not compile with its shape PCH"), std::string::npos) << log;
    EXPECT_EQ(count_defined_symbols(o.work_dir, "GEN"), 1u);
    EXPECT_EQ(count_defined_symbols(o.work_dir, "hot_live"), 1u);
    // The slot variable of the module is not used by this script. The tables of the classes are
    // local constants: each class has one table in the joined module.
    EXPECT_LE(count_defined_symbols(o.work_dir, "HotModule_slot"), 1u);
    EXPECT_EQ(count_defined_symbols(o.work_dir, "Holder_slots"), 1u);
    EXPECT_EQ(count_defined_symbols(o.work_dir, "Pt_slots"), 1u);
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// HOT_SPLIT_UNIT_OBJECTS=0 keeps one object (the split with the shape PCH stays).
TEST(ObjectCacheBackend, TwoObjectsCanBeTurnedOff)
{
#if defined(__linux__)
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    EnvGuard off("HOT_SPLIT_UNIT_OBJECTS", "0");
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "o", {});
    testing::internal::CaptureStderr();
    run_edits(o, 1, 6);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(log.find("hit, two objects"), std::string::npos) << log;
    EXPECT_NE(log.find("shape pch hit"), std::string::npos) << log;
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The guard of the two object split: a local unit state symbol (GEN) in an object is found. A shared
// (inline) variable is not a local symbol, so it is not found.
TEST(ObjectCacheBackend, UnitStateGuardFindsLocalCopiesOfTheState)
{
#if defined(__linux__)
    TestScratch scratch;
    const fs::path dir = scratch.file("guard");
    fs::create_directories(dir);
    std::ofstream(dir / "local.cpp") << "static unsigned GEN = 0;\nstatic unsigned hot_live = 0;\nstatic void* X_slot = nullptr;\n"
                                        "unsigned f() { return ++GEN + ++hot_live + (X_slot != nullptr); }\n";
    std::ofstream(dir / "shared.cpp") << "inline unsigned GEN = 0;\ninline unsigned hot_live = 0;\ninline void* X_slot = nullptr;\n"
                                         "unsigned g() { return ++GEN + ++hot_live + (X_slot != nullptr); }\n";
    for (const char* n : {"local", "shared"})
    {
        const std::string cmd = std::string(HOT_CXX) + " -c -fPIC -o " + (dir / (std::string(n) + ".o")).string() + " "
            + (dir / (std::string(n) + ".cpp")).string();
        ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
    }
    std::set<std::string> local, shared;
    ASSERT_TRUE(hot::unit_state_symbols_of((dir / "local.o").string(), local));
    ASSERT_TRUE(hot::unit_state_symbols_of((dir / "shared.o").string(), shared));
    EXPECT_EQ(local.size(), 3u); // _ZL3GEN, _ZL8hot_live, _ZL6X_slot
    EXPECT_TRUE(shared.empty());
    std::set<std::string> none;
    EXPECT_FALSE(hot::unit_state_symbols_of((dir / "missing.o").string(), none));
#else
    GTEST_SKIP() << "ELF only";
#endif
}

namespace
{

// A unit text in the form of the translator, with the banners.
std::string handmade_unit(const std::string& p_extra, const std::string& bodies_extra)
{
    return "#include <cstdint>\n"                                                       // 1
           "[[maybe_unused]] static uint32_t GEN = 0;\n"                                // 2
           "static uint32_t hot_live = 0;\n"                                            // 3
           + p_extra +
           "// ------------------------------------------------------ record and entry\n"
           "static void* X_slot = nullptr;\n"
           "// ---------------- more of P\n"
           "constexpr int after_line = __LINE__;\n"
           "// ---------------------------------------------------------------- function bodies\n"
           + bodies_extra +
           "extern \"C\" void script_register(hot::Registry& reg)\n{\n}\n";
}

} // namespace

// The shape header has the same __LINE__ as the whole unit for a line after a removed range and
// after the macro block.
TEST(ObjectCacheBackend, SplitHeaderKeepsTheLineNumbers)
{
    const std::string text = handmade_unit("static int f(int x);\n", "static int f(int x) { return x; }\n");
    std::string header, main, shape;
    std::vector<std::string> fns;
    ASSERT_TRUE(hot::split_unit_for_test(text, "u.hot", header, main, shape, fns));
    ASSERT_EQ(fns, std::vector<std::string>{"f"});
    int line = 1;
    for (char c : text.substr(0, text.find("constexpr int after_line")))
        line += c == '\n';
    TestScratch scratch;
    const std::string file = scratch.file("lines.cpp");
    std::ofstream(file) << header << "static_assert(after_line == " << line << ");\n";
    const std::string cmd = std::string(HOT_CXX) + " -std=c++20 -fsyntax-only -x c++ " + file + " 2>&1";
    EXPECT_EQ(std::system(cmd.c_str()), 0) << header;
}

// An overloaded name, two definitions, or a definition in P of a name that B defines: no split.
// A comment after a prototype is allowed.
TEST(ObjectCacheBackend, SplitRefusesOverloadsAndAcceptsTrailingComments)
{
    std::string header, main, shape;
    std::vector<std::string> fns;
    // Overloads with other parameters are different functions: they split, each one with its text.
    ASSERT_TRUE(hot::split_unit_for_test(handmade_unit("static int f(int x);\nstatic int f(double x);\n",
                                             "static int f(int x) { return x; }\nstatic int f(double x) { return 1; }\n"),
        "u.hot", header, main, shape, fns));
    EXPECT_EQ(fns.size(), 2u);
    EXPECT_NE(main.find("HOT_UNIT_FN int f(int x)"), std::string::npos) << main;
    EXPECT_NE(main.find("HOT_UNIT_FN int f(double x)"), std::string::npos) << main;
    // An overload that P defines with a body stays static in P.
    ASSERT_TRUE(hot::split_unit_for_test(handmade_unit("static int f(int x);\nstatic int f(double x) { return 1; }\n",
                                             "static int f(int x) { return x; }\n"),
        "u.hot", header, main, shape, fns));
    EXPECT_EQ(header.find("HOT_UNIT_FN int f(double"), std::string::npos) << header;
    EXPECT_NE(header.find("HOT_UNIT_FN int f(int x);"), std::string::npos) << header;
    // The same prototype twice, or a P definition of a prototype that B defines: no split.
    EXPECT_FALSE(hot::split_unit_for_test(handmade_unit("static int f(int x);\nstatic int f(int x);\n",
                                              "static int f(int x) { return x; }\n"),
        "u.hot", header, main, shape, fns));
    EXPECT_FALSE(hot::split_unit_for_test(handmade_unit("static int f(int x);\nstatic int f(int x) { return 1; }\n",
                                              "static int f(int x) { return x; }\n"),
        "u.hot", header, main, shape, fns));
    fns.clear();
    ASSERT_TRUE(hot::split_unit_for_test(handmade_unit("static int g(int x); // a note\n", "static int g(int x) { return x; }\n"),
        "u.hot", header, main, shape, fns));
    EXPECT_EQ(fns, std::vector<std::string>{"g"});
    EXPECT_NE(header.find("HOT_UNIT_FN int g(int x); // a note"), std::string::npos) << header;
    EXPECT_NE(main.find("HOT_UNIT_FN int g(int x)"), std::string::npos) << main;
}

TEST(ObjectCacheBackend, TwoObjectsRefuseLtoAndOtherLinkers)
{
    EXPECT_TRUE(hot::two_object_flags_ok_of({"-O2", "-fuse-ld=lld", "-g"}));
    EXPECT_FALSE(hot::two_object_flags_ok_of({"-O2", "-flto"}));
    EXPECT_FALSE(hot::two_object_flags_ok_of({"-flto=thin"}));
    EXPECT_FALSE(hot::two_object_flags_ok_of({"-fuse-ld=gold"}));
}

// The check of the joined object finds an internal name that the other object uses, and a missing
// function; a good join has no problem.
TEST(ObjectCacheBackend, JoinCheckFindsInternalNamesAndMissingFunctions)
{
#if defined(__linux__)
    const std::string lld = hot::find_lld_of(HOT_CXX);
    if (lld.empty())
        GTEST_SKIP() << "no ld.lld";
    TestScratch scratch;
    auto join = [&](const char* a, const char* b, const char* tag) {
        const fs::path dir = scratch.file(std::string("join_") + tag);
        fs::create_directories(dir);
        std::ofstream(dir / "a.cpp") << a;
        std::ofstream(dir / "b.cpp") << b;
        for (const char* n : {"a", "b"})
        {
            const std::string cmd = std::string(HOT_CXX) + " -c -fPIC -o " + (dir / (std::string(n) + ".o")).string() + " "
                + (dir / (std::string(n) + ".cpp")).string();
            EXPECT_EQ(std::system(cmd.c_str()), 0) << cmd;
        }
        const std::string out = (dir / "m.o").string();
        const std::string cmd = lld + " -r " + (dir / "a.o").string() + " " + (dir / "b.o").string() + " -o " + out;
        EXPECT_EQ(std::system(cmd.c_str()), 0) << cmd;
        return out;
    };
    // B calls X_create, which R has as an internal function (a name of the record in P).
    const std::string bad = join("static int X_create() { return 1; }\nint other() { return X_create(); }\n",
        "static int X_create();\nint body() { return X_create(); }\n", "bad");
    EXPECT_NE(hot::merged_object_problem_of(bad, {}).find("X_create"), std::string::npos);
    const std::string bad2 = join("static int f() { return 1; }\nint g() { return f(); }\n",
        "int f();\nint h() { return f(); }\n", "bad2");
    EXPECT_FALSE(hot::merged_object_problem_of(bad2, {}).empty());
    const std::string good = join("int g() { return 2; }\n", "int h() { return 1; }\n", "good");
    EXPECT_EQ(hot::merged_object_problem_of(good, {"g", "h"}), "");
    EXPECT_NE(hot::merged_object_problem_of(good, {"nowhere"}).find("nowhere"), std::string::npos);
    EXPECT_FALSE(hot::merged_object_problem_of((scratch.file("join_good") + "/none.o"), {}).empty());
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// A join tool that fails: the load works from the whole unit (or the split with one object), the
// result is right, and the marker for the failed split is written.
TEST(ObjectCacheBackend, FailedJoinFallsBackAndWritesTheMarker)
{
#if defined(__linux__)
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld";
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    const std::string tool = scratch.file("badlld.sh");
    std::ofstream(tool) << "#!/bin/sh\necho join broken >&2\nexit 1\n";
    fs::permissions(tool, fs::perms::owner_all);
    EnvGuard lld("HOT_LLD", tool.c_str());
    hot::ClangFilesOptions o = fmt_options(scratch, "j", {});
    const fs::path shape_dir = fs::path(o.cache_dir) / "pch" / "shape";
    testing::internal::CaptureStderr();
    run_edits(o, 1, 8);
    const std::string log = testing::internal::GetCapturedStderr();
    if (shape_pch_expected())
    {
        EXPECT_NE(log.find("does not compile with its shape PCH"), std::string::npos) << log;
        EXPECT_GE(count_ext(shape_dir, ".nosplit"), 1u);
    }
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// Two scripts with the same file name in two directories do not mix their shape PCH or objects.
TEST(ObjectCacheBackend, SameScriptNameInTwoDirectoriesStaysApart)
{
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "d", {});
    hot::ClangFilesBackend backend(o);
    for (int i = 1; i <= 6; ++i)
    {
        EXPECT_EQ(load_and_run(backend, {"a/calc.hot", script_text((std::to_string(i) + ".0").c_str()), {}, "Calc"}),
            static_cast<float>(i));
        EXPECT_EQ(load_and_run(backend, {"b/calc.hot", script_text((std::to_string(i + 100) + ".0").c_str()), {}, "Calc"}),
            static_cast<float>(i + 100));
    }
}

// The PCH of a cache is built again for other fmt flags and the results stay right.
TEST(ObjectCacheBackend, PchRebuildWithAChangedFmtFlagKeepsResults)
{
#if defined(__linux__)
    CleanEnv env;
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "c", {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"});
    run_edits(o, 1, 5);
    o.extra_flags = {"-DFMT_HEADER_ONLY"};
    o.work_dir = scratch.file("c_work2");
    run_edits(o, 10, 5);
    o.extra_flags = {"-DFMT_SHARED", "-DFMT_HEADER_ONLY"};
    o.work_dir = scratch.file("c_work3");
    run_edits(o, 20, 5);
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// Many real scripts: the split and the join work (two objects), or the unit falls back without an
// error. The module has one copy of GEN and hot_live. Scripts with imports are not used.
TEST(ObjectCacheBackend, TwoObjectsOverManyScripts)
{
#if defined(__linux__)
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld";
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "m", {});
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(std::string(HOT_SOURCE_DIR) + "/tests/scripts"))
        if (e.path().extension() == ".hot")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    size_t used = 0, two = 0;
    std::string fallbacks;
    for (const fs::path& f : files)
    {
        if (used >= 16)
            break;
        std::ifstream in(f);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        if (text.find("using \"") != text.find("using \"Actor.h\"") || text.find("using \"Actor.h\"") == std::string::npos)
            continue;
        const size_t c = text.find("\nclass ");
        if (c == std::string::npos)
            continue;
        size_t e = c + 7;
        while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '_'))
            ++e;
        const std::string cls = text.substr(c + 7, e - c - 7);
        // The first load of the script gets the base PCH, the second builds the shape PCH, the third uses it.
        testing::internal::CaptureStderr();
        bool loaded = true;
        {
            // The last module stays loaded: its file stays in the work dir for the symbol check.
            std::unique_ptr<hot::ClangFilesBackend> backend;
            hot::LoadResult r;
            for (int round = 0; round < 3; ++round)
            {
                r = hot::LoadResult();
                backend.reset();
                backend = std::make_unique<hot::ClangFilesBackend>(o);
                // Only the PCH files stay: the objects and the links are made again.
                std::error_code rm_ec;
                for (const auto& entry : fs::directory_iterator(o.cache_dir, rm_ec))
                    if (entry.path().filename() != "pch")
                        fs::remove_all(entry.path(), rm_ec);
                r = backend->load({f.filename().string(), text, {}, cls}, 1);
                loaded = loaded && static_cast<bool>(r);
            }
            if (loaded)
            {
                EXPECT_EQ(count_defined_symbols(o.work_dir, "GEN"), 1u) << f;
                EXPECT_EQ(count_defined_symbols(o.work_dir, "hot_live"), 1u) << f;
            }
        }
        const std::string log = testing::internal::GetCapturedStderr();
        if (!loaded)
            continue; // a script that does not load alone (its own needs): not a split case
        ++used;
        if (log.find("hit, two objects") != std::string::npos)
            ++two;
        else
            fallbacks += f.filename().string() + " ";
        EXPECT_EQ(log.find("does not compile with its shape PCH"), std::string::npos) << f << "\n" << log;
    }
    EXPECT_GE(used, 8u);
    if (shape_pch_expected())
        EXPECT_GE(two, used / 2) << "scripts without two objects: " << fallbacks;
    std::fprintf(stderr, "two objects: %zu of %zu scripts; others: %s\n", two, used, fallbacks.c_str());
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The object key has the split decision: an object that the split made does not serve a build with
// HOT_SPLIT_UNIT_OBJECTS=0, and the first object serves again when the split is on.
TEST(ObjectCacheBackend, SplitDecisionIsInTheObjectKey)
{
#if defined(__linux__)
    EXPECT_NE(hot::split_object_tag_of(true), hot::split_object_tag_of(false));
    EXPECT_NE(hot::split_object_tag_of(true).find("2obj-v"), std::string::npos); // the version is in the tag
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld";
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "k", {});
    testing::internal::CaptureStderr();
    run_edits(o, 1, 9);
    const std::string on = testing::internal::GetCapturedStderr();
    if (shape_pch_expected())
        ASSERT_NE(on.find("hit, two objects"), std::string::npos) << on;
    std::string off;
    {
        EnvGuard no_split("HOT_SPLIT_UNIT_OBJECTS", "0");
        testing::internal::CaptureStderr();
        run_edits(o, 9, 1);
        off = testing::internal::GetCapturedStderr();
    }
    EXPECT_NE(off.find("cache miss"), std::string::npos) << off; // not the joined object of the first run
    EXPECT_EQ(off.find("cache hit"), std::string::npos) << off;
    EXPECT_EQ(off.find("link cached"), std::string::npos) << off;
    testing::internal::CaptureStderr();
    run_edits(o, 9, 1);
    const std::string again = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(again.find("cache hit") != std::string::npos || again.find("link cached") != std::string::npos) << again;
#else
    GTEST_SKIP() << "ELF only";
#endif
}

namespace
{

// A script with a class and a method of a given text, in a given directory of headers.
hot::ClangFilesOptions header_options(TestScratch& scratch, const std::string& name, const std::string& header)
{
    hot::ClangFilesOptions o = fmt_options(scratch, name, {});
    const fs::path dir = scratch.file(name + "_inc");
    fs::create_directories(dir);
    std::ofstream(dir / "counter.h") << header;
    o.include_dirs.push_back(dir.string());
    return o;
}

float run_counter_script(hot::ClangFilesBackend& backend, int edit)
{
    const std::string text = "using \"Actor.h\";\nusing \"counter.h\";\nclass Calc : Actor {\n    int first = next_id();\n"
                             "    float result = 0.0;\n    override void Tick(float dt) {\n        int edit = "
        + std::to_string(edit) + ";\n        this.result = (float)(next_id() - this.first + 100);\n    }\n}\n";
    return load_and_run(backend, {"calc.hot", text, {}, "Calc"});
}

} // namespace

// A header with a mutable static variable that a field initializer and a body use: the split is
// refused and the module has one counter (the results are right).
TEST(ObjectCacheBackend, MutableHeaderStateInBothObjectsRefusesTheSplit)
{
#if defined(__linux__)
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld";
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions o = header_options(scratch, "h",
        "#pragma once\nstatic int g_counter = 0;\nstatic int next_id() { return ++g_counter; }\n");
    testing::internal::CaptureStderr();
    {
        hot::ClangFilesBackend backend(o);
        for (int i = 1; i <= 7; ++i)
            EXPECT_EQ(run_counter_script(backend, i), 101.0f); // first = 1 (or any), result = next - first + 100
    }
    const std::string log = testing::internal::GetCapturedStderr();
    // The initializer and the body are in different objects and both use the counter: the guard
    // refuses the split (before the guard the results were 100: two counters).
    if (shape_pch_expected())
    {
        EXPECT_NE(log.find("two objects refused"), std::string::npos) << log;
        EXPECT_EQ(log.find("hit, two objects"), std::string::npos) << log;
    }
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// A class without fields: its tables are not state in two objects, the split works.
TEST(ObjectCacheBackend, ClassWithoutFieldsStillSplits)
{
#if defined(__linux__)
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld";
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "n", {});
    testing::internal::CaptureStderr();
    {
        hot::ClangFilesBackend backend(o);
        for (int i = 1; i <= 9; ++i)
        {
            const std::string text = "using \"Actor.h\";\nclass Plain : Actor {\n    override void Tick(float dt) {\n"
                                     "        int edit = " + std::to_string(i) + ";\n    }\n}\n";
            const hot::LoadResult r = backend.load({"plain.hot", text, {}, "Plain"}, 1);
            EXPECT_TRUE(static_cast<bool>(r)) << r.error;
        }
    }
    const std::string log = testing::internal::GetCapturedStderr();
    if (shape_pch_expected())
        EXPECT_NE(log.find("hit, two objects"), std::string::npos) << log;
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The guard counts a mutable local variable that both objects USE (a relocation of code), not one that an
// object only defines.
TEST(ObjectCacheBackend, UnitStateGuardNeedsAUseInBothObjects)
{
#if defined(__linux__)
    TestScratch scratch;
    const fs::path dir = scratch.file("guard2");
    fs::create_directories(dir);
    std::ofstream(dir / "a.cpp") << "static int g_counter = 0;\nint fa() { return ++g_counter; }\n";
    std::ofstream(dir / "b.cpp") << "static int g_counter = 0;\nint fb() { return ++g_counter; }\n";
    std::ofstream(dir / "c.cpp") << "[[gnu::used]] static int g_counter = 0;\nint fc() { return 1; }\n";
    for (const char* n : {"a", "b", "c"})
    {
        const std::string cmd = std::string(HOT_CXX) + " -c -fPIC -o " + (dir / (std::string(n) + ".o")).string() + " "
            + (dir / (std::string(n) + ".cpp")).string();
        ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
    }
    std::set<std::string> a, b, c;
    ASSERT_TRUE(hot::unit_state_symbols_of((dir / "a.o").string(), a));
    ASSERT_TRUE(hot::unit_state_symbols_of((dir / "b.o").string(), b));
    ASSERT_TRUE(hot::unit_state_symbols_of((dir / "c.o").string(), c));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 1u);
    EXPECT_TRUE(c.empty()); // defined, but no code uses it
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// Real compiler output: a local variable of each size, used with an immediate store, an immediate
// compare, an immediate add or a plain read, at -O0 and -O2. Each use is found (the addend of a pc
// relative relocation depends on the size of the immediate), so two objects with such uses are refused.
TEST(ObjectCacheBackend, UnitStateGuardFindsEveryKindOfUse)
{
#if defined(__linux__)
    TestScratch scratch;
    const fs::path dir = scratch.file("guard3");
    fs::create_directories(dir);
    const char* const bodies[] = {
        "void f() { g = 1; }\n",         // store of an immediate
        "bool f() { return g == 0; }\n", // compare with an immediate
        "void f() { g += 1; }\n",        // add of an immediate
        "int f() { return g; }\n",       // plain read
    };
    for (const char* type : {"bool", "char", "short", "int"})
        for (const char* opt : {"-O0", "-O2"})
        {
            std::vector<std::set<std::string>> sets;
            for (size_t k = 0; k < 4; ++k)
            {
                const std::string base = std::string(type) + opt + std::to_string(k);
                std::ofstream(dir / (base + ".cpp")) << "static volatile " << type << " g;\n" << bodies[k];
                const std::string cmd = std::string(HOT_CXX) + " " + opt + " -c -fPIC -o " + (dir / (base + ".o")).string()
                    + " " + (dir / (base + ".cpp")).string();
                ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;
                sets.emplace_back();
                ASSERT_TRUE(hot::unit_state_symbols_of((dir / (base + ".o")).string(), sets.back()));
                EXPECT_EQ(sets.back().size(), 1u) << type << " " << opt << " body " << k;
            }
            for (size_t k = 1; k < sets.size(); ++k)
                EXPECT_EQ(sets[0], sets[k]) << type << " " << opt << " body " << k; // the same name: refused when both objects have it
        }
#else
    GTEST_SKIP() << "ELF only";
#endif
}

// The link runs the linker without the driver (the command comes from one "-###" run), and the
// modules work. HOT_DIRECT_LINK=0 uses the driver.
TEST(ObjectCacheBackend, DirectLinkWorksAndCanBeTurnedOff)
{
#if defined(__linux__)
    CleanEnv env;
    EnvGuard timing("HOT_TIMING", "1");
    TestScratch scratch;
    hot::ClangFilesOptions o = fmt_options(scratch, "dl", {});
    o.linker_flag = "-fuse-ld=lld";
    EnvGuard direct_on("HOT_DIRECT_LINK", "1");
    if (hot::find_lld_of(HOT_CXX).empty())
        GTEST_SKIP() << "no ld.lld";
    testing::internal::CaptureStderr();
    run_edits(o, 1, 3);
    const std::string direct = testing::internal::GetCapturedStderr();
    EXPECT_NE(direct.find("hot: direct link /"), std::string::npos) << direct;
    EXPECT_EQ(direct.find("direct link failed"), std::string::npos) << direct;
    EnvGuard off("HOT_DIRECT_LINK", "0");
    o.work_dir = scratch.file("dl_work2");
    testing::internal::CaptureStderr();
    run_edits(o, 10, 3);
    const std::string driver = testing::internal::GetCapturedStderr();
    EXPECT_NE(driver.find("hot: direct link off"), std::string::npos) << driver;
#else
    GTEST_SKIP() << "ELF only";
#endif
}
