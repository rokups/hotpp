// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of HostSet (step 2a): load, match and refresh.
#include "hosts.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
    #include <process.h>
    #define getpid _getpid // the MSVC CRT has the POSIX name only as a deprecated name (C4996)
#else
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using hot::ls::HostSet;

namespace
{

class HostsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = fs::temp_directory_path()
            / ("hot_hosts_test_" + std::to_string(getpid()) + "_"
                + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        hosts_dir = root / "hosts";
        fs::create_directories(hosts_dir);
    }
    void TearDown() override { fs::remove_all(root); }

    // Makes a build directory. With a cache file it looks like a real CMake build directory.
    std::string make_build(const std::string& name, bool cache = true)
    {
        const fs::path b = root / "build" / name;
        fs::create_directories(b);
        if (cache)
            std::ofstream(b / "CMakeCache.txt") << "# cache\n";
        // The JSON text of write_host takes the path as it is. A "\\" of a Windows path is not a
        // valid JSON escape, so the path has the "/" form.
        return b.generic_string();
    }

    void write_host(
        const std::string& name, const std::string& build, const std::string& dirs, const std::string& scripts = "")
    {
        std::ofstream(hosts_dir / (name + ".json"))
            << R"({"version":1,"name":")" << name << R"(","binaryDir":")" << build
            << R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"",)"
            << R"("scriptDirs":[)" << dirs << R"(],"scripts":[)" << scripts << R"(],)"
            << R"("includeDirs":["/inc"],"defines":["A=1","B"],"flags":["-Wall"]})";
    }

    // Moves the modification time of the directory forward. A coarse clock cannot hide a change.
    // Windows (wine) cannot set the time of a directory with std::filesystem. There the clock of
    // the file system is fine enough, so the error is ignored.
    void bump_dir()
    {
        std::error_code ec;
        const auto t = fs::last_write_time(hosts_dir, ec);
        if (!ec)
            fs::last_write_time(hosts_dir, t + std::chrono::seconds(5), ec);
#ifndef _WIN32
        ASSERT_FALSE(ec) << ec.message();
#endif
    }

    fs::path root, hosts_dir;
};

TEST_F(HostsTest, LoadsTwoHosts)
{
    write_host("a", make_build("a"), R"("/src/a")");
    write_host("b", make_build("b"), R"("/src/b")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 2u);
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->name, "a");
    EXPECT_EQ(h->compiler, "/usr/bin/c++");
    EXPECT_EQ(h->std_version, "c++20");
    EXPECT_EQ(h->include_dirs, std::vector<std::string>{"/inc"});
    EXPECT_EQ(set.match("/src/b/y.hot")->name, "b");
}

TEST_F(HostsTest, CxxFlagsAddDefines)
{
    write_host("a", make_build("a"), R"("/src/a")");
    HostSet set;
    set.load(hosts_dir.string());
    const std::vector<std::string> want{"-Wall", "-DA=1", "-DB"};
    EXPECT_EQ(set.match("/src/a/x.hot")->cxx_flags(), want);
}

// A host whose defines and options come from side files. The lines hold quotes and backslashes.
TEST_F(HostsTest, SideFilesGiveDefinesAndOptions)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","scriptDirs":["/src/a"],)"
        << R"("defines":["X=1"],"definesFile":"a.defines","optionsFile":"a.options","flags":["-Wall"]})";
    std::ofstream(hosts_dir / "a.defines", std::ios::binary) << "Q=\"a b\"\r\n\nP=\"C:\\dir\"\nPIPE=\"x|y\"\n";
    std::ofstream(hosts_dir / "a.options") << "-Wextra\n-DR=\"1\"\n";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 1u); // the side files are not hosts
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    const std::vector<std::string> defines{"X=1", "Q=\"a b\"", "P=\"C:\\dir\"", "PIPE=\"x|y\""};
    EXPECT_EQ(h->defines, defines);
    const std::vector<std::string> flags{"-Wextra", "-DR=\"1\"", "-Wall"};
    EXPECT_EQ(h->flags, flags);
    EXPECT_EQ(h->cxx_flags().back(), "-DPIPE=\"x|y\"");
}

TEST_F(HostsTest, MissingSideFileIsEmpty)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json") << R"({"version":1,"name":"a","binaryDir":")" << build
                                        << R"(","scriptDirs":["/src/a"],)"
                                        << R"("definesFile":"none.defines","optionsFile":"none.options","flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    ASSERT_EQ(set.size(), 1u);
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    EXPECT_TRUE(h->defines.empty());
    EXPECT_TRUE(h->flags.empty());
}

TEST_F(HostsTest, SideFileChangeForcesReload)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json") << R"({"version":1,"name":"a","binaryDir":")" << build
                                        << R"(","scriptDirs":["/src/a"],)"
                                        << R"("definesFile":"a.defines","flags":[]})";
    std::ofstream(hosts_dir / "a.defines") << "A=1\n";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/a/x.hot")->defines, std::vector<std::string>{"A=1"});
    std::ofstream(hosts_dir / "a.defines") << "B=\"2\"\n";
    EXPECT_FALSE(set.refresh()); // the modification time of the directory is the same
    EXPECT_TRUE(set.force_reload_for((hosts_dir / "a.defines").string()));
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.match("/src/a/x.hot")->defines, std::vector<std::string>{"B=\"2\""});
    EXPECT_TRUE(set.force_reload_for((hosts_dir / "a.options").string()));
    EXPECT_FALSE(set.force_reload_for((hosts_dir / "a.txt").string()));
}

// The include dirs come from "includeDirs" and from an includes side file. The lines hold any
// character. A relative line is relative to "sourceDir". The result is normalized and unique.
TEST_F(HostsTest, IncludesFileGivesResolvedUniqueIncludeDirs)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","sourceDir":"/proj/sub",)"
        << R"("scriptDirs":["/src/a"],"includeDirs":["/proj/include","/ex/q\"uote","rel/ex","/proj/include/"],)"
        << R"("includesFile":"a.includes","flags":[]})";
    std::ofstream(hosts_dir / "a.includes", std::ios::binary) << "/abs/sp ace\n"
                                                              << "/abs/q\"uote\\back\n"
                                                              << "/abs/\xC3\xA5\xC5\xBE\xE2\x82\xAC\n" // UTF-8
                                                              << "/abs/[br,ack]$HOME#=|>-x\n"
                                                              << "-lead\n"
                                                              << "rel/inc\n"
                                                              << "rel/../other/\n"
                                                              << "/abs/trail/\n"
                                                              << "/abs/dot/./sub/../x\n"
                                                              << "/ex/q\"uote\r\n" // repeated from "includeDirs"
                                                              << "\n"
                                                              << "/abs/sp ace\n"; // repeated
    HostSet set;
    set.load(hosts_dir.string());
    ASSERT_EQ(set.size(), 1u);
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    const std::vector<std::string> want{
        "/proj/include", // the first entry of "includeDirs" stays first
        "/abs/sp ace",
#ifdef _WIN32
        "/abs/q\"uote/back", // on Windows "\\" is a separator, and the host set gives "/"
#else
        "/abs/q\"uote\\back",
#endif
        "/abs/\xC3\xA5\xC5\xBE\xE2\x82\xAC",
        "/abs/[br,ack]$HOME#=|>-x",
        "/proj/sub/-lead",
        "/proj/sub/rel/inc",
        "/proj/sub/other",
        "/abs/trail",
        "/abs/dot/x",
        "/ex/q\"uote",
        "/proj/sub/rel/ex",
    };
    EXPECT_EQ(h->include_dirs, want);
}

TEST_F(HostsTest, RelativeScriptPathsUseSourceDir)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json") << R"({"version":1,"name":"a","binaryDir":")" << build
                                        << R"(","sourceDir":"/proj/sub",)"
                                        << R"("scriptDirs":["scripts/","../other"],"scripts":["one.hot"],"flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    ASSERT_EQ(set.size(), 1u);
    EXPECT_TRUE(set.match("/proj/sub/scripts/x.hot"));
    EXPECT_TRUE(set.match("/proj/other/x.hot"));
    EXPECT_TRUE(set.match("/proj/sub/one.hot"));
    EXPECT_FALSE(set.match("/proj/sub/two.hot"));
}

TEST_F(HostsTest, IncludesFileChangeForcesReload)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json") << R"({"version":1,"name":"a","binaryDir":")" << build
                                        << R"(","scriptDirs":["/src/a"],)"
                                        << R"("includesFile":"a.includes","flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_TRUE(set.match("/src/a/x.hot")->include_dirs.empty()); // a missing file is empty
    std::ofstream(hosts_dir / "a.includes") << "/i/one\n";
    EXPECT_TRUE(set.force_reload_for((hosts_dir / "a.includes").string()));
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.match("/src/a/x.hot")->include_dirs, std::vector<std::string>{"/i/one"});
}

TEST_F(HostsTest, LongestPrefixWins)
{
    write_host("outer", make_build("o"), R"("/src")");
    write_host("inner", make_build("i"), R"("/src/deep/dir")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/deep/dir/x.hot")->name, "inner");
    EXPECT_EQ(set.match("/src/other/x.hot")->name, "outer");
}

TEST_F(HostsTest, ExactScriptBeatsDir)
{
    write_host("dir", make_build("d"), R"("/src/deep/dir")");
    write_host("exact", make_build("e"), R"("/src")", R"("/src/deep/dir/x.hot")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/deep/dir/x.hot")->name, "exact");
    EXPECT_EQ(set.match("/src/deep/dir/y.hot")->name, "dir");
}

// A host with preludes. The helper write_host has none.
TEST_F(HostsTest, IncompleteExactHostGivesWayToHostWithPreludes)
{
    write_host("a_scripts", make_build("s"), R"("/src/deep/dir")", R"("/src/deep/dir/x.hot")");
    std::ofstream(hosts_dir / "b_full.json")
        << R"({"version":1,"name":"b_full","binaryDir":")" << make_build("f") << R"(","scriptDirs":["/src/deep"],)"
        << R"("preludes":["/p/a.hot"],"includeDirs":["/inc"],"defines":[],"flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/deep/dir/x.hot")->name, "b_full");
    EXPECT_EQ(set.match("/src/deep/dir/y.hot")->name, "b_full");
}

TEST_F(HostsTest, IncompleteHostWinsWhenNoHostHasPreludes)
{
    write_host("a_scripts", make_build("s"), R"("/src/deep/dir")", R"("/src/deep/dir/x.hot")");
    write_host("b_dir", make_build("f"), R"("/src/deep")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/deep/dir/x.hot")->name, "a_scripts");
}

TEST_F(HostsTest, PrefixIsComponentAware)
{
    write_host("a", make_build("a"), R"("/a/b/")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_TRUE(set.match("/a/b/c.hot"));
    EXPECT_FALSE(set.match("/a/bc/x.hot"));
}

TEST_F(HostsTest, TieGoesToFirstName)
{
    write_host("zeta", make_build("z"), R"("/src")");
    write_host("alpha", make_build("a"), R"("/src")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/x.hot")->name, "alpha");
}

TEST_F(HostsTest, NoMatchGivesNull)
{
    write_host("a", make_build("a"), R"("/src/a")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/elsewhere/x.hot"), nullptr);
    HostSet empty;
    EXPECT_EQ(empty.match("/src/a/x.hot"), nullptr);
}

TEST_F(HostsTest, SkipsInvalidJsonAndWrongVersion)
{
    write_host("good", make_build("g"), R"("/src")");
    std::ofstream(hosts_dir / "bad.json") << "{ not json";
    std::ofstream(hosts_dir / "v2.json") << R"({"version":2,"name":"v2","binaryDir":")" << make_build("v2")
                                         << R"(","scriptDirs":["/src"]})";
    std::ofstream(hosts_dir / "note.txt") << "ignored";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 1u);
    EXPECT_EQ(set.match("/src/x.hot")->name, "good");
}

TEST_F(HostsTest, SkipsStaleHost)
{
    write_host("stale", make_build("s", false), R"("/src")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 0u);
    set.load(hosts_dir.string(), false);
    EXPECT_EQ(set.size(), 1u);
}

TEST_F(HostsTest, RefreshReloadsAfterChange)
{
    write_host("a", make_build("a"), R"("/src/a")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_FALSE(set.refresh());
    EXPECT_EQ(set.size(), 1u);
    write_host("b", make_build("b"), R"("/src/b")");
    bump_dir();
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.size(), 2u);
    EXPECT_EQ(set.match("/src/b/x.hot")->name, "b");
    EXPECT_FALSE(set.refresh());
}

TEST_F(HostsTest, RefreshAddsHostWhenCacheAppears)
{
    const std::string build = make_build("s", false);
    write_host("late", build, R"("/src")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 0u);
    EXPECT_FALSE(set.refresh());
    std::ofstream(fs::path(build) / "CMakeCache.txt") << "# cache\n";
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.size(), 1u);
    EXPECT_EQ(set.match("/src/x.hot")->name, "late");
    EXPECT_FALSE(set.refresh());
}

TEST_F(HostsTest, RefreshDropsHostWhenCacheVanishes)
{
    const std::string build = make_build("g");
    write_host("gone", build, R"("/src")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 1u);
    fs::remove(fs::path(build) / "CMakeCache.txt");
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.size(), 0u);
    EXPECT_EQ(set.match("/src/x.hot"), nullptr);
    EXPECT_FALSE(set.refresh());
    // The cache file returns: the host returns.
    std::ofstream(fs::path(build) / "CMakeCache.txt") << "# cache\n";
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.size(), 1u);
}

} // namespace

// "preludes" in a host file: a relative path is relative to "sourceDir".
TEST_F(HostsTest, PreludesAreRelativeToSourceDir)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json") << R"({"version":1,"name":"a","binaryDir":")" << build
                                        << R"(","sourceDir":"/src","scriptDirs":["a"],)"
                                        << R"("preludes":["p/game.prelude.hot","/abs/x.prelude.hot"]})";
    HostSet set;
    set.load(hosts_dir.string());
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    const std::vector<std::string> expected = {
        std::filesystem::path("/src/p/game.prelude.hot").lexically_normal().generic_string(),
        std::filesystem::path("/abs/x.prelude.hot").lexically_normal().generic_string()};
    EXPECT_EQ(h->preludes, expected);
    EXPECT_TRUE(h->prelude_suffix.empty());
}

// "preludeSuffix" in a host file selects the user preludes in the script dirs.
TEST_F(HostsTest, PreludeSuffixIsRead)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json") << R"({"version":1,"name":"a","binaryDir":")" << build
                                        << R"(","sourceDir":"/src","scriptDirs":["a"],)"
                                        << R"("preludeSuffix":".prelude.hot"})";
    HostSet set;
    set.load(hosts_dir.string());
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->prelude_suffix, ".prelude.hot");
}

// The source roots come from the inline arrays and from the side file. A "!" line is a SYSTEM dir.
TEST_F(HostsTest, LoadsSourceRootsAndSystemDirs)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","sourceDir":"/src/a","scriptDirs":["/src/a"],)"
        << R"("sourceRoots":["/r/one","rel/two"],"systemDirs":["/r/sys1"],"sourceRootsFile":"a.roots"})";
    std::ofstream(hosts_dir / "a.roots", std::ios::binary) << "/r/three\r\n\n!/r/sys2\n/r/one\n/r/four/\n";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 1u); // the roots file is not a host
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    const std::vector<std::string> roots{"/r/one", "/src/a/rel/two", "/r/three", "/r/four"};
    EXPECT_EQ(h->source_roots, roots);
    const std::vector<std::string> system{"/r/sys1", "/r/sys2"};
    EXPECT_EQ(h->system_dirs, system);
    EXPECT_EQ(h->binary_dir, build);
}

TEST_F(HostsTest, HostWithoutRootsHasNone)
{
    write_host("a", make_build("a"), R"("/src/a")");
    std::ofstream(hosts_dir / "b.json") << R"({"version":1,"name":"b","scriptDirs":[],"sourceRootsFile":"gone.roots"})";
    HostSet set;
    set.load(hosts_dir.string(), false);
    const auto h = set.match("/src/a/x.hot");
    ASSERT_TRUE(h);
    EXPECT_TRUE(h->source_roots.empty());
    EXPECT_TRUE(h->system_dirs.empty());
}

TEST_F(HostsTest, ChangedRootsFileForcesReload)
{
    write_host("a", make_build("a"), R"("/src/a")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_TRUE(set.force_reload_for((hosts_dir / "a.roots").string()));
    EXPECT_TRUE(set.refresh());
}

// The pure function host_header_refusal.
class HeaderRefusalTest : public HostsTest
{
protected:
    void SetUp() override
    {
        HostsTest::SetUp();
        base = fs::weakly_canonical(root).generic_string();
        for (const char* d : {"eng/inc/sub", "eng/inc2", "eng2/inc", "eng/sys/inc", "eng/build/inc", "game"})
            fs::create_directories(base + "/" + d);
    }
    std::string p(const std::string& rel) const { return base + "/" + rel; }
    hot::ls::Host host() const
    {
        hot::ls::Host h;
        h.name = "h";
        h.binary_dir = p("eng/build");
        h.include_dirs = {p("eng/inc"), p("eng/sys/inc"), p("eng/build/inc")};
        h.source_roots = {p("eng"), p("game")};
        h.system_dirs = {p("eng/sys")};
        return h;
    }
    std::string base;
};

TEST_F(HeaderRefusalTest, HeaderBelowAnIncludeDirGetsAnEdit)
{
    EXPECT_EQ(hot::ls::host_header_refusal(host(), p("eng/inc/sub/x.h")), "");
    EXPECT_EQ(hot::ls::host_header_refusal(host(), p("eng/inc/x.h")), "");
}

TEST_F(HeaderRefusalTest, IncludeDirBorderIsAPathBorder)
{
    EXPECT_EQ(hot::ls::host_header_refusal(host(), p("eng/inc2/x.h")),
        "outside the workspace folders and the include dirs of the host");
    // ".." does not leave the include dir by text.
    EXPECT_EQ(hot::ls::host_header_refusal(host(), p("eng/inc/../inc2/x.h")),
        "outside the workspace folders and the include dirs of the host");
}

TEST_F(HeaderRefusalTest, SourceRootBorderIsAPathBorder)
{
    hot::ls::Host h = host();
    h.include_dirs = {p("eng2/inc")};
    h.source_roots = {p("eng")};
    EXPECT_EQ(hot::ls::host_header_refusal(h, p("eng2/inc/x.h")), "outside the source dirs of the build of the host");
}

TEST_F(HeaderRefusalTest, UsrIncludeIsRefusedAndEngineSourceGetsAnEdit)
{
    hot::ls::Host h;
    h.include_dirs = {"/usr/include", "/home/u/engine/Source"};
    h.source_roots = {"/home/u/game", "/home/u/engine"};
    EXPECT_NE(hot::ls::host_header_refusal(h, "/usr/include/SDL2/SDL.h"), "");
    EXPECT_NE(hot::ls::host_header_refusal(h, "/usr/include/c++/15/vector"), "");
    EXPECT_EQ(hot::ls::host_header_refusal(h, "/usr/include/SDL2/SDL.h"), "outside the source dirs of the build of the host");
    EXPECT_EQ(hot::ls::host_header_refusal(h, "/home/u/engine/Source/Urho3D/Scene/Node.h"), "");
}

TEST_F(HeaderRefusalTest, BuildDirIsRefused)
{
    EXPECT_EQ(hot::ls::host_header_refusal(host(), p("eng/build/inc/gen.h")), "in the build dir of the host");
}

TEST_F(HeaderRefusalTest, SystemDirIsRefusedAndSiblingGetsAnEdit)
{
    EXPECT_EQ(hot::ls::host_header_refusal(host(), p("eng/sys/inc/v.h")), "outside the source dirs of the build of the host");
    hot::ls::Host h = host();
    h.include_dirs.push_back(p("eng/sys2"));
    fs::create_directories(p("eng/sys2"));
    EXPECT_EQ(hot::ls::host_header_refusal(h, p("eng/sys2/v.h")), "");
}

TEST_F(HeaderRefusalTest, NoSourceRootsAsksForCMake)
{
    hot::ls::Host h = host();
    h.source_roots.clear();
    EXPECT_EQ(hot::ls::host_header_refusal(h, p("eng/inc/x.h")), "the host file has no source roots (run CMake again)");
    // A header outside the include dirs keeps the first text.
    EXPECT_EQ(hot::ls::host_header_refusal(h, p("game/x.h")), "outside the workspace folders and the include dirs of the host");
}

#ifndef _WIN32
// A link in an include dir to a place outside the roots does not hide the real place.
TEST_F(HeaderRefusalTest, SymlinkIncludeDirToOutsideIsRefused)
{
    fs::create_directories(p("outside/inc"));
    fs::create_directory_symlink(p("outside/inc"), p("eng/link"));
    hot::ls::Host h = host();
    h.include_dirs.push_back(p("eng/link"));
    EXPECT_EQ(hot::ls::host_header_refusal(h, p("eng/link/x.h")), "outside the source dirs of the build of the host");
    // A link inside the include dir to a place outside the roots is refused as well.
    fs::create_directory_symlink(p("outside/inc"), p("eng/inc/ln"));
    EXPECT_EQ(hot::ls::host_header_refusal(h, p("eng/inc/ln/x.h")), "outside the source dirs of the build of the host");
}
#endif
