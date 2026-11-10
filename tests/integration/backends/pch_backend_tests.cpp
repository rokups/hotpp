// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the precompiled header (PCH) of the clang files backend. The PCH has the include
// lines at the start of the translated text. The second miss of an include set builds it, and
// a later miss compiles with "-include-pch". The tests read a log of the compiler runs.
//
// CMake defines the macros HOT_SOURCE_DIR and HOT_CXX.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32

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
        if (value)
            test_support::set_env(name, value);
        else
            test_support::unset_env(name);
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
    EnvGuard cache{"HOT_CACHE", "1"};
    EnvGuard link_cache{"HOT_LINK_CACHE", "1"};
    EnvGuard max_mb{"HOT_CACHE_MAX_MB", nullptr};
    EnvGuard pch{"HOT_PCH", nullptr};
};

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
    for (size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + part.size()))
        ++n;
    return n;
}

// A compiler wrapper that writes each command line to "log".
std::string make_wrapper(const TestScratch& scratch, const std::string& log)
{
    const std::string path = scratch.file("cxx.sh");
    std::ofstream(path, std::ios::binary) << "#!/bin/sh\necho \"$*\" >> '" << log << "'\nexec " << HOT_CXX << " \"$@\"\n";
    fs::permissions(path, fs::perms::owner_all);
    return path;
}

std::unique_ptr<hot::ClangFilesBackend> make_backend(const TestScratch& scratch, const std::string& work_name,
    const std::string& cache_dir, const std::string& compiler, const std::string& inc = std::string(),
    const std::string& commands_dir = std::string())
{
    hot::ClangFilesOptions o;
    o.compiler = compiler;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    if (!inc.empty())
        o.include_dirs.push_back(inc);
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    o.compile_commands_dir = commands_dir;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

// A header with a struct. The script gives the size of the struct plus a constant.
void write_layout_header(const fs::path& path, int pad, std::chrono::minutes age)
{
    std::ofstream(path, std::ios::binary)
        << "#pragma once\nstruct LayoutProbe { char pad[" << pad << "]; };\n"
        << "inline float layout_size() { return static_cast<float>(sizeof(LayoutProbe)); }\n";
    // An old time: the cache does not store an entry whose header is younger than the compile.
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

// The include set is the same for each value, so all the scripts share one PCH.
hot::ScriptSource layout_script(int add)
{
    return {"calc.hot",
        "using \"Actor.h\";\nusing \"layout.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
        "    override void Tick(float dt) { this.result = layout_size() + " +
            std::to_string(add) + ".0; }\n}\n",
        {}, "Calc"};
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

// The PCH files of the cache.
std::vector<fs::path> pch_files(const fs::path& cache)
{
    std::vector<fs::path> r;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cache / "pch" / "obj", ec))
        if (e.path().extension() == ".o")
            r.push_back(e.path());
    return r;
}

struct Fixture
{
    CleanEnv env;
    TestScratch scratch;
    std::string cache = scratch.file("cache");
    std::string log = scratch.file("runs.log");
    std::string cxx = make_wrapper(scratch, log);
    fs::path inc = scratch.file("inc");
    int work = 0;

    Fixture()
    {
        fs::create_directories(inc);
        write_layout_header(inc / "layout.h", 4, std::chrono::minutes(60));
    }
    // Each load uses a new backend, so the module cache of a backend does not hide a compile.
    float run(int add, const std::string& commands_dir = std::string())
    {
        auto backend = make_backend(scratch, "work" + std::to_string(work++), cache, cxx, inc.string(), commands_dir);
        return load_and_run(*backend, layout_script(add));
    }
    size_t pch_builds() const { return count_text(read_file(log), "c++-header"); }
    size_t pch_compiles() const { return count_text(read_file(log), "-include-pch"); }
};

} // namespace

// The first miss of an include set does not build a PCH. The second miss builds it next to the
// normal compile. The third miss compiles with it and gives the same result. The compile
// database has no "-include-pch": an IDE cannot use the private PCH.
TEST(PchBackend, ThirdMissCompilesWithThePch)
{
    Fixture f;
    const std::string commands = f.scratch.file("commands");
    EXPECT_EQ(f.run(1, commands), 5.0f);
    EXPECT_EQ(f.pch_builds(), 0u);
    EXPECT_EQ(f.run(2, commands), 6.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 0u);
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
    EXPECT_EQ(f.run(3, commands), 7.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 1u);
    const std::string db = read_file(fs::path(commands) / "compile_commands.json");
    EXPECT_NE(db.find("Calc.cpp"), std::string::npos);
    EXPECT_EQ(db.find("-include-pch"), std::string::npos);
    // An object hit runs no compile and does not touch the PCH.
    EXPECT_EQ(f.run(3, commands), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A changed header makes the PCH stale. The next miss does not use it: it builds a new PCH and
// compiles without one. The miss after that uses the new PCH. The objects follow the header.
TEST(PchBackend, ChangedHeaderInvalidatesPchAndObject)
{
    Fixture f;
    EXPECT_EQ(f.run(1), 5.0f);
    EXPECT_EQ(f.run(2), 6.0f);
    EXPECT_EQ(f.run(3), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
    write_layout_header(f.inc / "layout.h", 16, std::chrono::minutes(30));
    EXPECT_EQ(f.run(3), 19.0f); // the object of the old header is stale
    EXPECT_EQ(f.pch_compiles(), 1u); // the stale PCH is not used
    EXPECT_EQ(f.pch_builds(), 2u);
    EXPECT_EQ(f.run(4), 20.0f);
    EXPECT_EQ(f.pch_compiles(), 2u);
}

// A damaged PCH (same size and time, bad bytes) makes the compile with it fail. The backend then
// compiles without the PCH, removes the PCH entry and gives the right result.
TEST(PchBackend, CorruptPchFallsBack)
{
    Fixture f;
    EXPECT_EQ(f.run(1), 5.0f);
    EXPECT_EQ(f.run(2), 6.0f);
    const std::vector<fs::path> files = pch_files(f.cache);
    ASSERT_EQ(files.size(), 1u);
    const uintmax_t size = fs::file_size(files[0]);
    const auto time = fs::last_write_time(files[0]);
    std::ofstream(files[0], std::ios::binary | std::ios::trunc) << std::string(size, 'x');
    fs::last_write_time(files[0], time);
    EXPECT_EQ(f.run(3), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 1u); // the attempt with the damaged PCH
    EXPECT_TRUE(pch_files(f.cache).empty());
    EXPECT_EQ(f.run(4), 8.0f); // no new attempt with a bad PCH
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// HOT_PCH=0 turns the PCH off: no build and no use.
TEST(PchBackend, EnvironmentTurnsThePchOff)
{
    Fixture f;
    EnvGuard off("HOT_PCH", "0");
    EXPECT_EQ(f.run(1), 5.0f);
    EXPECT_EQ(f.run(2), 6.0f);
    EXPECT_EQ(f.run(3), 7.0f);
    EXPECT_EQ(f.pch_builds(), 0u);
    EXPECT_EQ(f.pch_compiles(), 0u);
    EXPECT_TRUE(pch_files(f.cache).empty());
}

// Two backends build the same PCH at the same time. Both compiles give the right result, the
// cache has one PCH, and a later miss uses it.
TEST(PchBackend, TwoBackendsRaceOnThePch)
{
    Fixture f;
    EXPECT_EQ(f.run(1), 5.0f);
    auto a = make_backend(f.scratch, "race_a", f.cache, f.cxx, f.inc.string());
    auto b = make_backend(f.scratch, "race_b", f.cache, f.cxx, f.inc.string());
    float ra = 0.0f;
    float rb = 0.0f;
    std::thread ta([&] { ra = load_and_run(*a, layout_script(2)); });
    std::thread tb([&] { rb = load_and_run(*b, layout_script(3)); });
    ta.join();
    tb.join();
    EXPECT_EQ(ra, 6.0f);
    EXPECT_EQ(rb, 7.0f);
    EXPECT_GE(f.pch_builds(), 1u);
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
    EXPECT_EQ(f.run(4), 8.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

#endif
