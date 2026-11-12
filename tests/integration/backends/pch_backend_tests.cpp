// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the precompiled header (PCH) of the clang files backend. The PCH has the include
// lines at the start of the translated text. The first miss of an include set builds it, and
// the next miss compiles with "-include-pch". The tests read a log of the compiler runs.
//
// CMake defines the macros HOT_SOURCE_DIR and HOT_CXX.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/vfs.h"
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

// A host file system without files. The scripts and the headers come from the disk.
class EmptyFs final : public hot::FileSystem
{
public:
    bool read(const std::string&, std::string&) override { return false; }
    hot::VfsStat stat(const std::string&) override { return {}; }
};

// "incs": the include directories after the engine directories, in this order.
// "max_bytes": the size limit of the cache (0: no limit).
std::unique_ptr<hot::ClangFilesBackend> make_backend(const TestScratch& scratch, const std::string& work_name,
    const std::string& cache_dir, const std::string& compiler, const std::vector<std::string>& incs = {},
    const std::string& commands_dir = std::string(), std::shared_ptr<hot::FileSystem> fs = nullptr,
    uint64_t max_bytes = 0)
{
    hot::ClangFilesOptions o;
    o.fs = std::move(fs);
    o.compiler = compiler;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.include_dirs.insert(o.include_dirs.end(), incs.begin(), incs.end());
    o.work_dir = scratch.file(work_name);
    o.cache_dir = cache_dir;
    o.compile_commands_dir = commands_dir;
    o.cache_max_bytes = max_bytes;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

// An old time: the cache does not store an entry whose header is younger than the compile.
void set_age(const fs::path& path, std::chrono::minutes age)
{
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

// A header with a struct. The script gives the size of the struct plus a constant. "guard" true:
// an include guard LAYOUT_H in place of "#pragma once".
void write_layout_header(const fs::path& path, int pad, std::chrono::minutes age, bool guard = false)
{
    std::ofstream(path, std::ios::binary)
        << (guard ? "#ifndef LAYOUT_H\n#define LAYOUT_H\n" : "#pragma once\n") << "struct LayoutProbe { char pad["
        << pad << "]; };\n"
        << "inline float layout_size() { return static_cast<float>(sizeof(LayoutProbe)); }\n"
        << (guard ? "#endif\n" : "");
    set_age(path, age);
}

// The include set is the same for each value, so all the scripts share one PCH. The PCH does not
// depend on the script name either.
hot::ScriptSource layout_script(int add, const std::string& name = "calc.hot")
{
    return {name,
        "using \"Actor.h\";\nusing \"layout.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
        "    override void Tick(float dt) { this.result = layout_size() + " +
            std::to_string(add) + ".0; }\n}\n",
        {}, "Calc"};
}

// Loads the script, makes a Calc object, runs one tick and gives the result field.
float load_and_run(hot::Backend& backend, const hot::ScriptSource& source, uint32_t generation = 1)
{
    hot::LoadResult r = backend.load(source, generation);
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
    std::vector<std::string> before; // include directories before "inc"
    int work = 0;
    std::shared_ptr<hot::FileSystem> fs;

    Fixture()
    {
        fs::create_directories(inc);
        write_layout_header(inc / "layout.h", 4, std::chrono::minutes(60));
    }
    std::vector<std::string> incs() const
    {
        std::vector<std::string> r = before;
        r.push_back(inc.string());
        return r;
    }
    // Each load uses a new backend, so the module cache of a backend does not hide a compile. The
    // destructor of the backend waits for its PCH build.
    float run(int add, const std::string& commands_dir = std::string(), const std::string& name = "calc.hot",
        uint32_t generation = 1)
    {
        auto backend = make_backend(scratch, "work" + std::to_string(work++), cache, cxx, incs(), commands_dir, fs);
        return load_and_run(*backend, layout_script(add, name), generation);
    }
    size_t pch_builds() const { return count_text(read_file(log), "c++-header"); }
    size_t pch_compiles() const { return count_text(read_file(log), "-include-pch"); }
};

} // namespace

// The first miss of an include set builds the PCH in a thread next to its compile. The compile
// has no PCH. The second miss compiles with the PCH and gives the same result. The compile
// database has no "-include-pch": an IDE cannot use the private PCH.
TEST(PchBackend, SecondMissCompilesWithThePch)
{
    Fixture f;
    const std::string commands = f.scratch.file("commands");
    EXPECT_EQ(f.run(1, commands), 5.0f);
    EXPECT_EQ(f.pch_builds(), 1u); // the first miss builds the PCH, without a PCH compile
    EXPECT_EQ(f.pch_compiles(), 0u);
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
    EXPECT_EQ(f.run(2, commands), 6.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 1u);
    const std::string db = read_file(fs::path(commands) / "compile_commands.json");
    EXPECT_NE(db.find("Calc.cpp"), std::string::npos);
    EXPECT_EQ(db.find("-include-pch"), std::string::npos);
    // An object hit runs no compile and does not touch the PCH.
    EXPECT_EQ(f.run(2, commands), 6.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A changed header makes the PCH stale. The next miss does not use it: it builds a new PCH and
// compiles without one. The miss after that uses the new PCH. The objects follow the header.
TEST(PchBackend, ChangedHeaderInvalidatesPchAndObject)
{
    Fixture f;
    EXPECT_EQ(f.run(1), 5.0f);
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

// A host file system with no file for the compile (no mirror header) does not turn the PCH off.
TEST(PchBackend, HostFileSystemWithoutMirrorFilesUsesThePch)
{
    Fixture f;
    f.fs = std::make_shared<EmptyFs>();
    EXPECT_EQ(f.run(1), 5.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.run(3), 7.0f);
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
    auto a = make_backend(f.scratch, "race_a", f.cache, f.cxx, f.incs());
    auto b = make_backend(f.scratch, "race_b", f.cache, f.cxx, f.incs());
    float ra = 0.0f;
    float rb = 0.0f;
    std::thread ta([&] { ra = load_and_run(*a, layout_script(2)); });
    std::thread tb([&] { rb = load_and_run(*b, layout_script(3)); });
    ta.join();
    tb.join();
    a.reset(); // the destructors wait for the PCH builds
    b.reset();
    EXPECT_EQ(ra, 6.0f);
    EXPECT_EQ(rb, 7.0f);
    EXPECT_GE(f.pch_builds(), 1u);
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
    EXPECT_EQ(f.run(4), 8.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// Many backends in one process write the PCH header and build the PCH at the same time. The
// header of one writer must not replace the header of another: clang keeps the time of the
// header in the PCH. So a later miss uses the PCH, and no "bad" marker comes.
TEST(PchBackend, ManyBackendsRaceOnThePchHeader)
{
    Fixture f;
    const int n = 8;
    std::vector<std::unique_ptr<hot::ClangFilesBackend>> backends;
    for (int i = 0; i < n; ++i)
        backends.push_back(make_backend(f.scratch, "stress" + std::to_string(i), f.cache, f.cxx, f.incs()));
    std::vector<float> results(n, 0.0f);
    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back([&, i] { results[i] = load_and_run(*backends[i], layout_script(10 + i)); });
    for (std::thread& t : threads)
        t.join();
    backends.clear();
    for (int i = 0; i < n; ++i)
        EXPECT_EQ(results[i], static_cast<float>(14 + i));
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(f.cache) / "pch", ec))
        EXPECT_NE(e.path().extension(), ".bad") << read_file(e.path());
    EXPECT_EQ(f.run(30), 34.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A new header earlier in the include path hides the header of the PCH. Both copies have the
// same include guard, so a stale PCH would hide the new header without an error. The PCH key has
// the shadow paths, so the compile does not use the old PCH.
TEST(PchBackend, ShadowHeaderDoesNotUseTheOldPch)
{
    Fixture f;
    const fs::path inc0 = f.scratch.file("inc0");
    fs::create_directories(inc0);
    f.before = {inc0.string()};
    write_layout_header(f.inc / "layout.h", 4, std::chrono::minutes(60), true);
    EXPECT_EQ(f.run(1), 5.0f);
    EXPECT_EQ(f.run(3), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
    write_layout_header(inc0 / "layout.h", 16, std::chrono::minutes(60), true);
    EXPECT_EQ(f.run(3), 19.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// Two units with other script names and generations have the same include set. They share one
// PCH: the first miss builds it and the next miss uses it.
TEST(PchBackend, UnitsWithTheSameIncludesShareOnePch)
{
    Fixture f;
    EXPECT_EQ(f.run(1, "", "calc.hot", 1), 5.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.run(3, "", "third.hot", 5), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
}

// A compile error in the script code names the right script line when the PCH is on. The error
// does not turn the PCH off.
TEST(PchBackend, ScriptErrorKeepsItsLineWithThePch)
{
    Fixture f;
    EXPECT_EQ(f.run(1), 5.0f);
    {
        auto backend = make_backend(f.scratch, "bad", f.cache, f.cxx, f.incs());
        const hot::ScriptSource bad = {"calc.hot",
            "using \"Actor.h\";\nusing \"layout.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
            "    override void Tick(float dt) { this.result = \"fast\"; }\n}\n",
            {}, "Calc"};
        hot::LoadResult r = backend->load(bad, 1);
        ASSERT_FALSE(r);
        EXPECT_NE(r.error.find("calc.hot:5:"), std::string::npos) << r.error;
    }
    EXPECT_EQ(f.pch_compiles(), 1u);
    EXPECT_EQ(f.run(3), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 2u);
}

// The script includes "layout.h" only, and "layout.h" includes "layout_b.h". The object of a
// compile with the PCH depends on "layout_b.h" too: a change of it makes the object miss.
TEST(PchBackend, NestedPchHeaderInvalidatesTheObject)
{
    Fixture f;
    std::ofstream(f.inc / "layout.h", std::ios::binary)
        << "#pragma once\n#include \"layout_b.h\"\n"
        << "inline float layout_size() { return static_cast<float>(sizeof(LayoutProbe)); }\n";
    set_age(f.inc / "layout.h", std::chrono::minutes(60));
    const auto write_b = [&](int pad, std::chrono::minutes age)
    {
        std::ofstream(f.inc / "layout_b.h", std::ios::binary)
            << "#pragma once\nstruct LayoutProbe { char pad[" << pad << "]; };\n";
        set_age(f.inc / "layout_b.h", age);
    };
    write_b(4, std::chrono::minutes(60));
    EXPECT_EQ(f.run(1), 5.0f);
    EXPECT_EQ(f.run(3), 7.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
    write_b(16, std::chrono::minutes(30));
    EXPECT_EQ(f.run(3), 19.0f);
}

// A PCH larger than the size limit goes at once: the trim runs after each new PCH, not only when
// a backend starts.
TEST(PchBackend, NewPchIsTrimmedToTheLimit)
{
    Fixture f;
    auto backend = make_backend(f.scratch, "small", f.cache, f.cxx, f.incs(), "", nullptr, 2);
    EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f);
    EXPECT_EQ(load_and_run(*backend, layout_script(2)), 6.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    backend.reset(); // waits for the PCH build
    EXPECT_TRUE(pch_files(f.cache).empty());
}

// A limit smaller than one PCH removes each new PCH. The first rebuild writes the "bad" marker,
// so the next misses start no new PCH build.
TEST(PchBackend, PchLargerThanTheLimitIsBuiltOnce)
{
    Fixture f;
    auto backend = make_backend(f.scratch, "small", f.cache, f.cxx, f.incs(), "", nullptr, 2);
    EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f);
    for (int i = 2; i <= 6; ++i)
    {
        EXPECT_EQ(load_and_run(*backend, layout_script(i)), 4.0f + static_cast<float>(i));
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_TRUE(pch_files(f.cache).empty());
    size_t bad = 0;
    for (const auto& e : fs::directory_iterator(fs::path(f.cache) / "pch"))
        bad += e.path().extension() == ".bad" ? 1 : 0;
    EXPECT_EQ(bad, 1u);
}

// A compiler wrapper that removes the PCH file when it sees "-include-pch". It models a trim
// during the compile. The compile with the missing PCH fails and the plain compile works.
TEST(PchBackend, MissingPchDoesNotWriteTheBadMarker)
{
    Fixture f;
    // The wrapper does not change: the compiler id of the PCH key has its stamp. The trigger
    // file turns the removal on. The wrapper uses "mv": "rm" can be a shim in the PATH.
    const std::string trigger = f.scratch.file("trigger");
    const std::string wrapper = f.scratch.file("remove_pch.sh");
    std::ofstream(wrapper, std::ios::binary)
        << "#!/bin/sh\necho \"$*\" >> '" << f.log << "'\nprev=''\nfor a in \"$@\"; do\n"
           "  if [ \"$prev\" = '-include-pch' ] && [ -e '"
        << trigger << "' ]; then mv \"$a\" \"$a.gone\"; fi\n  prev=\"$a\"\ndone\nexec " << HOT_CXX
        << " \"$@\"\n";
    fs::permissions(wrapper, fs::perms::owner_all);
    for (int i = 1; i <= 1; ++i)
    {
        auto b = make_backend(f.scratch, "work" + std::to_string(i), f.cache, wrapper, f.incs());
        EXPECT_EQ(load_and_run(*b, layout_script(i)), 4.0f + static_cast<float>(i));
    }
    ASSERT_EQ(pch_files(f.cache).size(), 1u);
    std::ofstream(trigger) << "x";
    auto backend = make_backend(f.scratch, "work_rm", f.cache, wrapper, f.incs());
    EXPECT_EQ(load_and_run(*backend, layout_script(3)), 7.0f);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_compiles(), 1u); // the attempt with the missing PCH
    EXPECT_TRUE(pch_files(f.cache).empty());
    for (const auto& e : fs::directory_iterator(fs::path(f.cache) / "pch"))
        EXPECT_NE(e.path().extension(), ".bad");
}

// A load while the PCH thread runs uses no PCH and starts no second build. After wait_for_pch,
// the next load uses the PCH.
TEST(PchBackend, LoadWhileThePchBuildRunsUsesNoPch)
{
    Fixture f;
    const std::string path = f.scratch.file("slow_pch.sh");
    std::ofstream(path, std::ios::binary)
        << "#!/bin/sh\ncase \"$*\" in *c++-header*) sleep 2;; esac\nexec " << f.cxx << " \"$@\"\n";
    fs::permissions(path, fs::perms::owner_all);
    auto backend = make_backend(f.scratch, "work_slow", f.cache, path, f.incs());
    EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f); // starts the PCH thread
    EXPECT_EQ(load_and_run(*backend, layout_script(2)), 6.0f); // no PCH thread starts
    EXPECT_EQ(load_and_run(*backend, layout_script(3)), 7.0f); // the PCH thread still runs
    EXPECT_EQ(f.pch_compiles(), 0u);
    backend->wait_for_pch();
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
    EXPECT_EQ(load_and_run(*backend, layout_script(4)), 8.0f);
    EXPECT_EQ(f.pch_compiles(), 1u);
    EXPECT_EQ(f.pch_builds(), 1u);
}

// The trim removes the old marker files of a PCH that is gone. The files of a PCH that is in the
// cache stay, so the PCH still works.
TEST(PchBackend, TrimRemovesOldMarkersWithoutPch)
{
    Fixture f;
    EXPECT_EQ(f.run(1), 5.0f);
    ASSERT_EQ(pch_files(f.cache).size(), 1u);
    const fs::path pch_dir = fs::path(f.cache) / "pch";
    const std::string live = pch_files(f.cache)[0].stem().string();
    const auto days = [](int n) { return std::chrono::minutes(60 * 24 * n); };
    for (const char* ext : {".seen", ".h", ".bad"})
    {
        std::ofstream(pch_dir / (std::string("0123456789abcdef") + ext)) << "x";
        set_age(pch_dir / (std::string("0123456789abcdef") + ext), days(8));
        std::ofstream(pch_dir / (std::string("fedcba9876543210") + ext)) << "x";
        set_age(pch_dir / (std::string("fedcba9876543210") + ext), std::chrono::minutes(10));
    }
    set_age(pch_dir / (live + ".seen"), days(8));
    EXPECT_EQ(f.run(3), 7.0f); // the new backend trims
    EXPECT_EQ(f.pch_compiles(), 1u);
    for (const char* ext : {".seen", ".h", ".bad"})
    {
        EXPECT_FALSE(fs::exists(pch_dir / (std::string("0123456789abcdef") + ext))) << ext;
        EXPECT_TRUE(fs::exists(pch_dir / (std::string("fedcba9876543210") + ext))) << ext;
    }
    EXPECT_TRUE(fs::exists(pch_dir / (live + ".h")));
    EXPECT_TRUE(fs::exists(pch_dir / (live + ".seen")));
}

#endif
