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
#include "hot/runtime/process.h"
#include "hot/vfs.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
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

// The number of ".bad" marker files of the PCH cache.
size_t bad_markers(const fs::path& cache)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cache / "pch", ec))
        n += e.path().extension() == ".bad" ? 1 : 0;
    return n;
}

// A compiler wrapper with a shell command for the PCH build. Other runs go to the real compiler.
std::string make_pch_wrapper(const TestScratch& scratch, const std::string& name, const std::string& pch_command,
    const std::string& cxx)
{
    const std::string path = scratch.file(name);
    std::ofstream(path, std::ios::binary)
        << "#!/bin/sh\ncase \"$*\" in *c++-header*) " << pch_command << ";; esac\nexec " << cxx << " \"$@\"\n";
    fs::permissions(path, fs::perms::owner_all);
    return path;
}

// An exception in the PCH build is a failed PCH build: the process stays alive, the load works,
// the message appears once, and no "bad" marker stays for one day.
TEST(PchBackend, ExceptionInThePchBuildMeansNoPch)
{
    Fixture f;
    auto backend = make_backend(f.scratch, "work_throw", f.cache, f.cxx, f.incs());
    backend->set_pch_build_hook([] { throw std::runtime_error("boom"); });
    testing::internal::CaptureStderr();
    EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f);
    backend->wait_for_pch();
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(count_text(err, "has no PCH"), 1u) << err;
    EXPECT_NE(err.find("boom"), std::string::npos) << err;
    EXPECT_TRUE(pch_files(f.cache).empty());
    EXPECT_EQ(bad_markers(f.cache), 0u);
}

// A compile error of the PCH header gets the "bad" marker. A compiler that a signal kills does not.
TEST(PchBackend, BadMarkerOnlyForACompileErrorOfThePch)
{
    {
        Fixture f;
        const std::string cxx = make_pch_wrapper(f.scratch, "fail_pch.sh", "echo broken; exit 1", f.cxx);
        auto backend = make_backend(f.scratch, "work_fail", f.cache, cxx, f.incs());
        EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f);
        backend->wait_for_pch();
        EXPECT_EQ(bad_markers(f.cache), 1u);
    }
    {
        Fixture f;
        const std::string cxx = make_pch_wrapper(f.scratch, "kill_pch.sh", "kill -9 $$", f.cxx);
        auto backend = make_backend(f.scratch, "work_kill", f.cache, cxx, f.incs());
        EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f);
        backend->wait_for_pch();
        EXPECT_TRUE(pch_files(f.cache).empty());
        EXPECT_EQ(bad_markers(f.cache), 0u);
    }
}

// A load while wait_for_pch joins the PCH thread starts no second PCH build.
TEST(PchBackend, NoSecondPchBuildWhileWaitForPchJoins)
{
    Fixture f;
    auto backend = make_backend(f.scratch, "work_join", f.cache, f.cxx, f.incs());
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<int> started{0};
    backend->set_pch_build_hook(
        [&]
        {
            if (started++ == 0)
                gate.wait();
        });
    EXPECT_EQ(load_and_run(*backend, layout_script(1)), 5.0f); // starts the PCH thread
    std::thread waiter([&] { backend->wait_for_pch(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the waiter has the thread and joins
    EXPECT_EQ(load_and_run(*backend, layout_script(2)), 6.0f); // must not start a second build
    release.set_value();
    waiter.join();
    backend->wait_for_pch();
    EXPECT_EQ(started.load(), 1);
    EXPECT_EQ(f.pch_builds(), 1u);
}

// ---- The shared startup PCH ----

namespace
{

// A script with an include set of its own: "layout.h" and "other.h", in the order of "swap".
hot::ScriptSource two_header_script(int add, const std::string& name, bool swap = false)
{
    const std::string a = "using \"layout.h\";\n";
    const std::string b = "using \"other.h\";\n";
    return {name,
        "using \"Actor.h\";\n" + (swap ? b + a : a + b) +
            "class Calc : Actor {\n    float result = 0.0;\n"
            "    override void Tick(float dt) { this.result = layout_size() + other_value() + " +
            std::to_string(add) + ".0; }\n}\n",
        {}, "Calc"};
}

void write_other_header(const fs::path& dir)
{
    std::ofstream(dir / "other.h", std::ios::binary) << "#pragma once\ninline float other_value() { return 10.0f; }\n";
    set_age(dir / "other.h", std::chrono::minutes(60));
}

// Loads a batch with begin_load_many, compiles and finishes each unit. Gives the ok count.
size_t load_batch(hot::ClangFilesBackend& backend, std::vector<hot::ScriptSource> sources)
{
    for (hot::ScriptSource& s : sources)
        s.unit.clear(); // the unit name is the stem of the script name: the units of a batch differ
    auto many = backend.begin_load_many(sources, 1, hot::LoadMode::Reload);
    size_t ok = 0;
    for (auto& p : many)
        p->compile();
    for (auto& p : many)
    {
        hot::LoadResult r = p->finish();
        if (r)
            ++ok;
        else
            ADD_FAILURE() << r.error;
    }
    return ok;
}

// A PCH build in the hook waits for the gate.
struct Gate
{
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<int> started{0};
    std::function<void()> hook()
    {
        return [this]
        {
            ++started;
            gate.wait();
        };
    }
};

// Loads one script as a batch of one (begin_load_many, so the shared PCH logic runs), makes a
// Calc object, runs one tick and gives the result field.
float batch_and_run(hot::ClangFilesBackend& backend, hot::ScriptSource source, uint32_t generation = 1)
{
    source.unit.clear();
    auto many = backend.begin_load_many({source}, generation, hot::LoadMode::Reload);
    many[0]->compile();
    hot::LoadResult r = many[0]->finish();
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

} // namespace

TEST(PchBackend, UnionPchHeaderKeepsFirstSeenOrder)
{
    const std::string a = "// n\n#line 1 \"a\"\n#include <a>\n#include <b>\n";
    const std::string b = "#line 1 \"b\"\n#include <b>\n#include <c>\n#include <a>\n";
    EXPECT_EQ(hot::union_pch_header({a, b}), "#include <a>\n#include <b>\n#include <c>\n");
    EXPECT_EQ(hot::union_pch_header({b, a}), "#include <b>\n#include <c>\n#include <a>\n");
    EXPECT_EQ(hot::union_pch_header({}), "");
}

// One batch with two include sets starts one shared build. A later script with an include set
// that is a subset of the union compiles with the PCH on its first miss.
TEST(PchBackend, SharedPchServesFirstMiss)
{
    Fixture f;
    write_other_header(f.inc);
    auto backend = make_backend(f.scratch, "work_shared", f.cache, f.cxx, f.incs());
    Gate gate;
    backend->set_pch_build_hook(gate.hook());
    std::thread releaser(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            gate.release.set_value();
        });
    EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
    releaser.join();
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 0u);
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
    EXPECT_EQ(load_and_run(*backend, layout_script(7, "c.hot")), 11.0f); // include set of a.hot only
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A script with a line outside the union does not use the shared PCH. It builds an exact PCH as
// before. The line goes to the extra file for the next start.
TEST(PchBackend, SharedPchServesSubsetOnly)
{
    Fixture f;
    write_other_header(f.inc);
    {
        auto backend = make_backend(f.scratch, "work_sub1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    {
        auto backend = make_backend(f.scratch, "work_sub2", f.cache, f.cxx, f.incs());
        // A new object, but a subset of the union: the PCH serves it.
        EXPECT_EQ(load_batch(*backend, {layout_script(2, "a.hot")}), 1u);
        EXPECT_EQ(f.pch_compiles(), 1u);
        // A line outside the union: no shared PCH, so an exact build starts.
        EXPECT_EQ(load_and_run(*backend, two_header_script(3, "b.hot")), 0.0f + 4.0f + 10.0f + 3.0f);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_compiles(), 1u);
    EXPECT_EQ(f.pch_builds(), 2u);
    EXPECT_NE(read_file(fs::path(f.cache) / "pch" / "shared.extra").find("other.h"), std::string::npos);
}

// The order of the include lines of a unit does not matter for the shared PCH.
TEST(PchBackend, SharedPchOrderIndependent)
{
    Fixture f;
    write_other_header(f.inc);
    auto backend = make_backend(f.scratch, "work_order", f.cache, f.cxx, f.incs());
    EXPECT_EQ(load_batch(*backend, {two_header_script(1, "a.hot")}), 1u);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(load_and_run(*backend, two_header_script(2, "b.hot", true)), 4.0f + 10.0f + 2.0f);
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A failed shared build writes a "bad" marker for the shared key. The next start does not build
// the shared PCH again, and a script still builds and uses its exact PCH. The wrapper fails each
// PCH build whose header has "other.h": the shared PCH and the exact PCH of the second unit.
TEST(PchBackend, BadSharedPchKeepsExactPch)
{
    Fixture f;
    write_other_header(f.inc);
    const std::string bad_cxx = make_pch_wrapper(f.scratch, "fail_shared.sh",
        "for a in \"$@\"; do if [ -f \"$a\" ] && grep -q other.h \"$a\" 2>/dev/null; then echo broken; exit 1; fi; done",
        f.cxx);
    {
        auto backend = make_backend(f.scratch, "work_bad1", f.cache, bad_cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
        backend->wait_for_pch();
    }
    EXPECT_GE(bad_markers(f.cache), 1u);
    const size_t builds = f.pch_builds();
    {
        auto backend = make_backend(f.scratch, "work_bad2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(2, "a.hot"), two_header_script(2, "b.hot")}), 2u);
        backend->wait_for_pch();
    }
    EXPECT_GE(bad_markers(f.cache), 1u);
    EXPECT_GE(pch_files(f.cache).size(), 1u); // the exact PCH of the first unit
    EXPECT_LE(f.pch_builds(), builds + 1u);
    {
        auto backend = make_backend(f.scratch, "work_bad3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_and_run(*backend, layout_script(9, "a.hot")), 13.0f);
    }
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A second batch while the shared build runs starts no second build.
TEST(PchBackend, PrewarmWhileBuildRuns)
{
    Fixture f;
    write_other_header(f.inc);
    auto backend = make_backend(f.scratch, "work_prewarm", f.cache, f.cxx, f.incs());
    Gate gate;
    backend->set_pch_build_hook(gate.hook());
    EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
    EXPECT_EQ(load_batch(*backend, {two_header_script(1, "b.hot"), layout_script(2, "c.hot")}), 2u);
    gate.release.set_value();
    backend->wait_for_pch();
    EXPECT_EQ(gate.started.load(), 1);
    EXPECT_EQ(f.pch_builds(), 1u);
}

// A second batch that the union covers builds no new PCH, and the PCH still serves its unit.
TEST(PchBackend, SharedPchSecondBatchNoRebuild)
{
    Fixture f;
    write_other_header(f.inc);
    {
        auto backend = make_backend(f.scratch, "work_nb1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    const std::string inc = read_file(fs::path(f.cache) / "pch" / "shared.inc");
    {
        auto backend = make_backend(f.scratch, "work_nb2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(2, "c.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 1u);
    EXPECT_EQ(read_file(fs::path(f.cache) / "pch" / "shared.inc"), inc);
}

// A batch with a new header grows the union: one rebuild. The units of both batches use it.
TEST(PchBackend, SharedPchGrowsOnNewInclude)
{
    Fixture f;
    write_other_header(f.inc);
    {
        auto backend = make_backend(f.scratch, "work_gr1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    {
        auto backend = make_backend(f.scratch, "work_gr2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {two_header_script(2, "b.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 2u);
    EXPECT_EQ(f.pch_compiles(), 0u);
    {
        auto backend = make_backend(f.scratch, "work_gr3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(3, "c.hot"), two_header_script(3, "d.hot", true)}), 2u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 2u);
    EXPECT_EQ(f.pch_compiles(), 2u);
}

// A unit that fails does not add its include lines to shared.extra. So the next prewarm builds
// nothing new.
TEST(PchBackend, SharedExtraNotAddedForFailingUnit)
{
    Fixture f;
    write_other_header(f.inc);
    // The wrapper fails each unit compile whose source has "failmark_field".
    const std::string cxx = f.scratch.file("fail_all.sh");
    std::ofstream(cxx, std::ios::binary)
        << "#!/bin/sh\ncase \"$*\" in *c++-header*) ;; *) for a in \"$@\"; do if [ -f \"$a\" ] && grep -q failmark_field "
           "\"$a\" 2>/dev/null; then echo broken; exit 1; fi; done;; esac\nexec "
        << f.cxx << " \"$@\"\n";
    fs::permissions(cxx, fs::perms::owner_all);
    {
        auto backend = make_backend(f.scratch, "work_ex1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    {
        auto backend = make_backend(f.scratch, "work_ex2", f.cache, cxx, f.incs());
        hot::ScriptSource s{"x.hot",
            "using \"Actor.h\";\nusing \"other.h\";\nclass Calc : Actor {\n    float failmark_field = 0.0;\n}\n", {}, "Calc"};
        s.unit.clear();
        hot::LoadResult r = backend->load(s, 1);
        EXPECT_FALSE(static_cast<bool>(r));
        backend->wait_for_pch();
    }
    const std::string extra = read_file(fs::path(f.cache) / "pch" / "shared.extra");
    EXPECT_EQ(extra.find("other.h"), std::string::npos) << extra;
    const size_t builds = f.pch_builds(); // the failing unit can start an exact build
    {
        auto backend = make_backend(f.scratch, "work_ex3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(2, "b.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), builds);
}

// A unit that fails with the shared PCH and compiles without it does not disable the shared PCH
// for another unit.
TEST(PchBackend, FailingUnitDoesNotDisableSharedPch)
{
    Fixture f;
    write_other_header(f.inc);
    const std::string path = f.scratch.file("fail_unit.sh");
    std::ofstream(path, std::ios::binary)
        << "#!/bin/sh\ncase \"$*\" in *-include-pch*) for a in \"$@\"; do if [ -f \"$a\" ] && grep -q badmark_field \"$a\" "
           "2>/dev/null; then echo broken; exit 1; fi; done;; esac\nexec "
        << f.cxx << " \"$@\"\n";
    fs::permissions(path, fs::perms::owner_all);
    {
        auto backend = make_backend(f.scratch, "work_fu1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
        backend->wait_for_pch();
    }
    auto backend = make_backend(f.scratch, "work_fu2", f.cache, path, f.incs());
    hot::ScriptSource u1{"u1.hot",
        "using \"Actor.h\";\nusing \"layout.h\";\nclass Calc : Actor {\n    float badmark_field = 0.0;\n"
        "    float result = 0.0;\n    override void Tick(float dt) { this.result = layout_size(); }\n}\n",
        {}, "Calc"};
    EXPECT_EQ(load_batch(*backend, {u1}), 1u);
    EXPECT_EQ(bad_markers(f.cache), 0u);
    backend->wait_for_pch(); // a PCH build of the u1 batch can still run and add a compile
    const size_t before = f.pch_compiles();
    EXPECT_EQ(load_batch(*backend, {two_header_script(5, "u2.hot")}), 1u);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_compiles(), before + 1u);
}

TEST(PchBackend, EnvironmentTurnsTheSharedPchOff)
{
    Fixture f;
    write_other_header(f.inc);
    EnvGuard off("HOT_PCH", "0");
    auto backend = make_backend(f.scratch, "work_off", f.cache, f.cxx, f.incs());
    EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_builds(), 0u);
    EXPECT_TRUE(pch_files(f.cache).empty());
    EXPECT_FALSE(fs::exists(fs::path(f.cache) / "pch" / "shared.inc"));
}

// A body edit and an interface edit of a warm cache start no shared build. A new file at a probe
// path (a shadow path) changes no shared key.
TEST(PchBackend, EditBatchesOfAWarmCacheStartNoSharedBuild)
{
    Fixture f;
    write_other_header(f.inc);
    const fs::path inc0 = f.scratch.file("inc0");
    fs::create_directories(inc0);
    f.before = {inc0.string()};
    auto backend = make_backend(f.scratch, "work_edit", f.cache, f.cxx, f.incs());
    EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_builds(), 1u);
    // A body edit.
    EXPECT_EQ(load_batch(*backend, {layout_script(2, "a.hot")}), 1u);
    // An interface edit: a new field, the same includes.
    hot::ScriptSource iface = layout_script(3, "a.hot");
    iface.text.insert(iface.text.find("float result"), "int extra = 0;\n    ");
    EXPECT_EQ(load_batch(*backend, {iface}), 1u);
    // A new probe path: the file does not exist at the first parse, then it appears. The shared
    // PCH is reset and built again (the file can hide behind the old PCH).
    write_layout_header(inc0 / "layout.h", 4, std::chrono::minutes(60));
    EXPECT_EQ(load_batch(*backend, {layout_script(4, "a.hot")}), 1u);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_builds(), 2u);
}

// A new include line in an edit batch gives one build. The build starts after the last unit of
// the batch ends, not while the units compile.
TEST(PchBackend, EditBatchPchBuildStartsAfterTheBatch)
{
    Fixture f;
    write_other_header(f.inc);
    auto backend = make_backend(f.scratch, "work_after", f.cache, f.cxx, f.incs());
    EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
    backend->wait_for_pch();
    EXPECT_EQ(f.pch_builds(), 1u);
    std::atomic<int> started{0};
    backend->set_pch_build_hook([&] { ++started; });
    std::vector<hot::ScriptSource> sources = {two_header_script(2, "b.hot"), layout_script(2, "c.hot")};
    for (hot::ScriptSource& s : sources)
        s.unit.clear();
    auto many = backend->begin_load_many(sources, 1, hot::LoadMode::Reload);
    for (auto& p : many)
        p->compile();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(started.load(), 0); // the units compiled: no build yet
    EXPECT_EQ(f.pch_builds(), 1u);
    for (auto& p : many)
        EXPECT_TRUE(static_cast<bool>(p->finish()));
    many.clear();
    backend->wait_for_pch();
    EXPECT_EQ(started.load(), 1);
    EXPECT_EQ(f.pch_builds(), 2u);
}

// A failed shared build removes shared.inc. A header that is gone does not poison the union.
TEST(PchBackend, BadSharedPchRemovesTheUnion)
{
    Fixture f;
    write_other_header(f.inc);
    const std::string bad_cxx = make_pch_wrapper(f.scratch, "fail_shared2.sh",
        "for a in \"$@\"; do if [ -f \"$a\" ] && grep -q other.h \"$a\" 2>/dev/null; then echo broken; exit 1; fi; done",
        f.cxx);
    const fs::path inc_file = fs::path(f.cache) / "pch" / "shared.inc";
    {
        auto backend = make_backend(f.scratch, "work_ru1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_TRUE(fs::exists(inc_file));
    {
        auto backend = make_backend(f.scratch, "work_ru2", f.cache, bad_cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {two_header_script(1, "b.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_GE(bad_markers(f.cache), 1u);
    EXPECT_FALSE(fs::exists(inc_file));
    // The compile of the unit can add its line to shared.extra after the failed build removed
    // the file. That order is a race, so the test removes the file to test shared.inc only.
    fs::remove(fs::path(f.cache) / "pch" / "shared.extra");
    {
        auto backend = make_backend(f.scratch, "work_ru3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(2, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    ASSERT_TRUE(fs::exists(inc_file));
    EXPECT_EQ(read_file(inc_file).find("other.h"), std::string::npos);
}

// A damaged shared PCH file: the entry goes, no ".noshared" marker stays, and the next batch
// builds the shared PCH again.
TEST(PchBackend, CorruptSharedPchIsRemovedAndRebuilt)
{
    Fixture f;
    write_other_header(f.inc);
    {
        auto backend = make_backend(f.scratch, "work_cs1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}), 2u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    const std::vector<fs::path> files = pch_files(f.cache);
    ASSERT_EQ(files.size(), 1u);
    // Same size and time: the stamp of the manifest stays valid.
    const auto time = fs::last_write_time(files[0]);
    const std::string old = read_file(files[0]);
    {
        std::ofstream o(files[0], std::ios::binary | std::ios::trunc);
        o << std::string(old.size(), 'x');
    }
    fs::last_write_time(files[0], time);
    {
        auto backend = make_backend(f.scratch, "work_cs2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_and_run(*backend, layout_script(7, "c.hot")), 11.0f);
        backend->wait_for_pch();
    }
    EXPECT_TRUE(pch_files(f.cache).empty());
    size_t markers = 0;
    for (const auto& e : fs::directory_iterator(fs::path(f.cache) / "pch"))
        markers += e.path().extension() == ".noshared" ? 1 : 0;
    EXPECT_EQ(markers, 0u);
    {
        auto backend = make_backend(f.scratch, "work_cs3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(8, "d.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(pch_files(f.cache).size(), 1u);
}

// A file that appears at a probe path after the shared PCH build is a new header earlier in the
// include path. Both copies have the same include guard. The next batch sees the file, drops the
// shared PCH and builds it again, so the unit sees the NEW header.
TEST(PchBackend, NewFileAtAProbePathResetsTheSharedPch)
{
    Fixture f;
    const fs::path inc0 = f.scratch.file("inc0");
    fs::create_directories(inc0);
    f.before = {inc0.string()};
    write_layout_header(f.inc / "layout.h", 4, std::chrono::minutes(60), true);
    {
        auto backend = make_backend(f.scratch, "work_sh1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    const fs::path shadow = fs::path(f.cache) / "pch" / "shared.shadow";
    EXPECT_NE(read_file(shadow).find((inc0 / "layout.h").string()), std::string::npos);
    const size_t builds = f.pch_builds();
    {
        // The batch has a new file name: the translate and the object cache miss. The shared PCH serves it.
        auto backend = make_backend(f.scratch, "work_sh2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(batch_and_run(*backend, layout_script(2, "b.hot")), 6.0f);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), builds);
    write_layout_header(inc0 / "layout.h", 16, std::chrono::minutes(60), true);
    {
        auto backend = make_backend(f.scratch, "work_sh3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(batch_and_run(*backend, layout_script(3, "c.hot")), 19.0f);
        backend->wait_for_pch();
    }
    EXPECT_GT(f.pch_builds(), builds); // the shared PCH was built again
    {
        auto backend = make_backend(f.scratch, "work_sh4", f.cache, f.cxx, f.incs());
        EXPECT_EQ(batch_and_run(*backend, layout_script(4, "d.hot")), 20.0f);
        backend->wait_for_pch();
    }
}

// A bad shared build drops the union. A unit that planned before that must not add its lines to
// shared.extra after it (the epoch of the union changed).
TEST(PchBackend, StaleExtraAppendIsDiscardedAfterTheUnionDrops)
{
    Fixture f;
    write_other_header(f.inc);
    {
        auto backend = make_backend(f.scratch, "work_ep1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    const fs::path started = f.scratch.file("b_started");
    const fs::path go = f.scratch.file("b_go");
    const std::string cxx = f.scratch.file("epoch.sh");
    std::ofstream(cxx, std::ios::binary)
        << "#!/bin/sh\ncase \"$*\" in\n*c++-header*) for a in \"$@\"; do if [ -f \"$a\" ] && grep -q other.h \"$a\" "
           "2>/dev/null; then echo broken; exit 1; fi; done;;\n*b.cpp*) touch '"
        << started.string() << "'; i=0; while [ ! -f '" << go.string()
        << "' ] && [ $i -lt 400 ]; do sleep 0.05; i=$((i+1)); done;;\nesac\nexec " << f.cxx << " \"$@\"\n";
    fs::permissions(cxx, fs::perms::owner_all);
    const fs::path inc_file = fs::path(f.cache) / "pch" / "shared.inc";
    ASSERT_TRUE(fs::exists(inc_file));
    auto backend = make_backend(f.scratch, "work_ep2", f.cache, cxx, f.incs());
    Gate gate;
    backend->set_pch_build_hook(gate.hook());
    size_t ok = 0;
    std::thread worker([&] { ok = load_batch(*backend, {layout_script(1, "a.hot"), two_header_script(1, "b.hot")}); });
    const auto wait_for = [](const std::function<bool()>& cond)
    {
        for (int i = 0; i < 400 && !cond(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    wait_for([&] { return fs::exists(started); }); // the unit b planned: it has the extra line
    EXPECT_TRUE(fs::exists(started));
    gate.release.set_value(); // the shared build runs and fails
    wait_for([&] { return !fs::exists(inc_file); });
    EXPECT_FALSE(fs::exists(inc_file));
    std::ofstream(go.string()) << "go";
    worker.join();
    backend->wait_for_pch();
    EXPECT_EQ(ok, 2u);
    const fs::path extra = fs::path(f.cache) / "pch" / "shared.extra";
    EXPECT_EQ(read_file(extra).find("other.h"), std::string::npos);
}

// HOT_TIMING names the reason for each unit that compiles with no shared PCH.
TEST(PchBackend, NoSharedPchReasonIsLoggedUnderTiming)
{
    Fixture f;
    write_other_header(f.inc);
    EnvGuard timing("HOT_TIMING", "1");
    {
        // A cold cache: the shared build runs, and the entry is not there yet.
        auto backend = make_backend(f.scratch, "work_nr1", f.cache, f.cxx, f.incs());
        Gate gate;
        backend->set_pch_build_hook(gate.hook());
        testing::internal::CaptureStderr();
        EXPECT_EQ(batch_and_run(*backend, layout_script(1, "a.hot")), 5.0f);
        const std::string err = testing::internal::GetCapturedStderr();
        gate.release.set_value();
        backend->wait_for_pch();
        EXPECT_NE(err.find("hot: a no shared pch: shared pch building"), std::string::npos) << err;
    }
    {
        // The union has no "other.h" line.
        auto backend = make_backend(f.scratch, "work_nr2", f.cache, f.cxx, f.incs());
        Gate gate;
        backend->set_pch_build_hook(gate.hook());
        testing::internal::CaptureStderr();
        EXPECT_EQ(batch_and_run(*backend, two_header_script(1, "b.hot")), 15.0f);
        const std::string err = testing::internal::GetCapturedStderr();
        gate.release.set_value();
        backend->wait_for_pch();
        EXPECT_NE(err.find("no shared pch: "), std::string::npos) << err;
        EXPECT_NE(err.find("union lacks 1 lines (first: #include \"other.h\")"), std::string::npos) << err;
    }
}

// The shared key does not depend on the directory of the script. The shared PCH of a job in the
// directory A serves a unit in the directory B (a host file system adds a per-directory include path).
TEST(PchBackend, SharedPchKeyIsTheSameForAllScriptDirectories)
{
    Fixture f;
    f.fs = std::make_shared<EmptyFs>();
    const std::string dir_a = f.scratch.file("dir_a");
    const std::string dir_b = f.scratch.file("dir_b");
    {
        auto backend = make_backend(f.scratch, "work_dk1", f.cache, f.cxx, f.incs(), std::string(), f.fs);
        EXPECT_EQ(load_batch(*backend, {layout_script(1, dir_a + "/a.hot")}), 1u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_builds(), 1u);
    EnvGuard timing("HOT_TIMING", "1");
    auto backend = make_backend(f.scratch, "work_dk2", f.cache, f.cxx, f.incs(), std::string(), f.fs);
    testing::internal::CaptureStderr();
    EXPECT_EQ(batch_and_run(*backend, layout_script(2, dir_b + "/b.hot")), 6.0f);
    const std::string err = testing::internal::GetCapturedStderr();
    backend->wait_for_pch();
    EXPECT_EQ(err.find("shared entry missing"), std::string::npos) << err;
    EXPECT_EQ(f.pch_builds(), 1u);
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// A header that only the directory A finds (a file in the per-directory mirror path) stays out of
// the shared union. The unit in A compiles. A unit in B with the other lines uses the shared PCH.
TEST(PchBackend, LineResolvedOnlyInOneScriptDirectoryStaysOutOfTheUnion)
{
    Fixture f;
    f.fs = std::make_shared<EmptyFs>();
    const std::string dir_a = f.scratch.file("dir_la");
    const std::string dir_b = f.scratch.file("dir_lb");
    const fs::path rel_a = fs::path(dir_a).relative_path();
    const fs::path local = fs::path(f.scratch.file("work_lk1")) / "vfs" / rel_a;
    fs::create_directories(local);
    std::ofstream(local / "local.h", std::ios::binary) << "#pragma once\ninline float local_value() { return 100.0f; }\n";
    set_age(local / "local.h", std::chrono::minutes(60));
    // The translator finds the header next to the script. The compile finds the copy in the mirror path.
    fs::create_directories(dir_a);
    fs::copy_file(local / "local.h", fs::path(dir_a) / "local.h");
    const hot::ScriptSource a = {dir_a + "/a.hot",
        "using \"Actor.h\";\nusing \"layout.h\";\nusing \"local.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
        "    override void Tick(float dt) { this.result = layout_size() + local_value(); }\n}\n",
        {}, "Calc"};
    {
        auto backend = make_backend(f.scratch, "work_lk1", f.cache, f.cxx, f.incs(), std::string(), f.fs);
        EXPECT_EQ(batch_and_run(*backend, a), 104.0f);
        backend->wait_for_pch();
    }
    {
        auto backend = make_backend(f.scratch, "work_lk2", f.cache, f.cxx, f.incs(), std::string(), f.fs);
        EXPECT_EQ(batch_and_run(*backend, layout_script(2, dir_b + "/b.hot")), 6.0f);
        backend->wait_for_pch();
    }
    EXPECT_EQ(f.pch_compiles(), 1u);
}

// An older shared entry with the same union text must not hit after a reset. The union grows
// (two entries), then a file appears at a probe path. The reset restarts from the text of the first
// entry: the key must differ, so the unit sees the NEW header.
TEST(PchBackend, ResetSaltKeepsOldSharedEntriesFromHitting)
{
    Fixture f;
    const fs::path inc0 = f.scratch.file("inc0");
    fs::create_directories(inc0);
    f.before = {inc0.string()};
    write_other_header(f.inc);
    write_layout_header(f.inc / "layout.h", 4, std::chrono::minutes(60), true);
    {
        auto backend = make_backend(f.scratch, "work_sa1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    {
        auto backend = make_backend(f.scratch, "work_sa2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(2, "b.hot"), two_header_script(1, "c.hot")}), 2u);
        backend->wait_for_pch();
    }
    EXPECT_EQ(pch_files(f.cache).size(), 2u);
    write_layout_header(inc0 / "layout.h", 16, std::chrono::minutes(60), true);
    {
        auto backend = make_backend(f.scratch, "work_sa3", f.cache, f.cxx, f.incs());
        EXPECT_EQ(batch_and_run(*backend, layout_script(3, "d.hot")), 19.0f);
        backend->wait_for_pch();
    }
    EXPECT_TRUE(fs::exists(fs::path(f.cache) / "pch" / "shared.salt"));
    {
        auto backend = make_backend(f.scratch, "work_sa4", f.cache, f.cxx, f.incs());
        EXPECT_EQ(batch_and_run(*backend, layout_script(4, "e.hot")), 20.0f);
        backend->wait_for_pch();
    }
}

// The salt is a random nonce, not a counter: a deleted cache directory cannot reuse an old salt.
// A crash before the write of the list (simulated: the temp file of the list is a directory) leaves
// the new salt, no old entry and no shared.inc. The list is old, so the next batch resets again.
TEST(PchBackend, ResetWritesTheRandomSaltBeforeTheListAndDropsTheEntry)
{
    Fixture f;
    const fs::path inc0 = f.scratch.file("inc0");
    fs::create_directories(inc0);
    f.before = {inc0.string()};
    write_layout_header(f.inc / "layout.h", 4, std::chrono::minutes(60), true);
    {
        auto backend = make_backend(f.scratch, "work_or1", f.cache, f.cxx, f.incs());
        EXPECT_EQ(load_batch(*backend, {layout_script(1, "a.hot")}), 1u);
        backend->wait_for_pch();
    }
    const fs::path pch = fs::path(f.cache) / "pch";
    const std::string shadow_before = read_file(pch / "shared.shadow");
    EXPECT_FALSE(fs::exists(pch / "shared.salt"));
    write_layout_header(inc0 / "layout.h", 16, std::chrono::minutes(60), true);
    fs::create_directories(pch / ("shared.shadow.tmp" + std::to_string(hot::detail::process_id())));
    {
        auto backend = make_backend(f.scratch, "work_or2", f.cache, f.cxx, f.incs());
        EXPECT_EQ(batch_and_run(*backend, layout_script(3, "b.hot")), 19.0f);
        backend->wait_for_pch();
    }
    const std::string salt = read_file(pch / "shared.salt");
    EXPECT_GE(salt.size(), 16u);
    EXPECT_NE(salt, "1");
    EXPECT_EQ(read_file(pch / "shared.shadow"), shadow_before); // the list write failed: still old
}

// A failure of start_pending_pch (a thread that does not start) must not end the process. The
// backend has no hook for a failure of the thread constructor, so a test of this path is not
// possible here. The call in batch_done has a try/catch, and the build functions reset the state.

#endif

// Two backends in one process share the work directory. The library of the first backend stays
// mapped. The second backend must run its own new code, not the old library.
TEST(LibraryPathBackend, SecondBackendInSameWorkDirRunsNewCode)
{
    Fixture f;
    auto first = make_backend(f.scratch, "work", f.cache, HOT_CXX, f.incs());
    hot::LoadResult r1 = first->load(layout_script(1), 1);
    ASSERT_TRUE(r1) << r1.error;
    Session s1;
    s1.start(std::move(*r1.module), "Calc"); // keeps the first library mapped
    ASSERT_TRUE(s1.actor);

    auto second = make_backend(f.scratch, "work", f.cache, HOT_CXX, f.incs());
    hot::LoadResult r2 = second->load(layout_script(2), 1);
    ASSERT_TRUE(r2) << r2.error;
    Session s2;
    s2.start(std::move(*r2.module), "Calc");
    ASSERT_TRUE(s2.actor);
    s1.world.Tick(kDt);
    s2.world.Tick(kDt);
    EXPECT_EQ(*s2.field("result") - *s1.field("result"), 1.0f);
}
