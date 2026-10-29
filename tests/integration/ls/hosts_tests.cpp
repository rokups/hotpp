// Tests of HostSet (step 2a): load, match and refresh.
#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "hosts.h"

namespace fs = std::filesystem;
using spp::ls::HostSet;

namespace {

class HostsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() / ("spp_hosts_test_" + std::to_string(getpid()) + "_" +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
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
        if (cache) std::ofstream(b / "CMakeCache.txt") << "# cache\n";
        return b.string();
    }

    void write_host(const std::string& name, const std::string& build, const std::string& dirs,
        const std::string& scripts = "")
    {
        std::ofstream(hosts_dir / (name + ".json"))
            << R"({"version":1,"name":")" << name << R"(","binaryDir":")" << build
            << R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"",)"
            << R"("scriptDirs":[)" << dirs << R"(],"scripts":[)" << scripts << R"(],)"
            << R"("includeDirs":["/inc"],"defines":["A=1","B"],"flags":["-Wall"]})";
    }

    // Moves the modification time of the directory forward. A coarse clock cannot hide a change.
    void bump_dir()
    {
        fs::last_write_time(hosts_dir, fs::last_write_time(hosts_dir) + std::chrono::seconds(5));
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
    const auto h = set.match("/src/a/x.spp");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->name, "a");
    EXPECT_EQ(h->compiler, "/usr/bin/c++");
    EXPECT_EQ(h->std_version, "c++20");
    EXPECT_EQ(h->include_dirs, std::vector<std::string>{"/inc"});
    EXPECT_EQ(set.match("/src/b/y.spp")->name, "b");
}

TEST_F(HostsTest, CxxFlagsAddDefines)
{
    write_host("a", make_build("a"), R"("/src/a")");
    HostSet set;
    set.load(hosts_dir.string());
    const std::vector<std::string> want{"-Wall", "-DA=1", "-DB"};
    EXPECT_EQ(set.match("/src/a/x.spp")->cxx_flags(), want);
}

// A host whose defines and options come from side files. The lines hold quotes and backslashes.
TEST_F(HostsTest, SideFilesGiveDefinesAndOptions)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","scriptDirs":["/src/a"],)"
        << R"("defines":["X=1"],"definesFile":"a.defines","optionsFile":"a.options","flags":["-Wall"]})";
    std::ofstream(hosts_dir / "a.defines", std::ios::binary)
        << "Q=\"a b\"\r\n\nP=\"C:\\dir\"\nPIPE=\"x|y\"\n";
    std::ofstream(hosts_dir / "a.options") << "-Wextra\n-DR=\"1\"\n";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 1u);  // the side files are not hosts
    const auto h = set.match("/src/a/x.spp");
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
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","scriptDirs":["/src/a"],)"
        << R"("definesFile":"none.defines","optionsFile":"none.options","flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    ASSERT_EQ(set.size(), 1u);
    const auto h = set.match("/src/a/x.spp");
    ASSERT_TRUE(h);
    EXPECT_TRUE(h->defines.empty());
    EXPECT_TRUE(h->flags.empty());
}

TEST_F(HostsTest, SideFileChangeForcesReload)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","scriptDirs":["/src/a"],)"
        << R"("definesFile":"a.defines","flags":[]})";
    std::ofstream(hosts_dir / "a.defines") << "A=1\n";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/a/x.spp")->defines, std::vector<std::string>{"A=1"});
    std::ofstream(hosts_dir / "a.defines") << "B=\"2\"\n";
    EXPECT_FALSE(set.refresh());  // the modification time of the directory is the same
    EXPECT_TRUE(set.force_reload_for((hosts_dir / "a.defines").string()));
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.match("/src/a/x.spp")->defines, std::vector<std::string>{"B=\"2\""});
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
    std::ofstream(hosts_dir / "a.includes", std::ios::binary)
        << "/abs/sp ace\n"
        << "/abs/q\"uote\\back\n"
        << "/abs/\xC3\xA5\xC5\xBE\xE2\x82\xAC\n"  // UTF-8
        << "/abs/[br,ack]$HOME#=|>-x\n"
        << "-lead\n"
        << "rel/inc\n"
        << "rel/../other/\n"
        << "/abs/trail/\n"
        << "/abs/dot/./sub/../x\n"
        << "/ex/q\"uote\r\n"  // repeated from "includeDirs"
        << "\n"
        << "/abs/sp ace\n";  // repeated
    HostSet set;
    set.load(hosts_dir.string());
    ASSERT_EQ(set.size(), 1u);
    const auto h = set.match("/src/a/x.spp");
    ASSERT_TRUE(h);
    const std::vector<std::string> want{
        "/proj/include",  // the first entry of "includeDirs" stays first
        "/abs/sp ace",
        "/abs/q\"uote\\back",
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
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","sourceDir":"/proj/sub",)"
        << R"("scriptDirs":["scripts/","../other"],"scripts":["one.spp"],"flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    ASSERT_EQ(set.size(), 1u);
    EXPECT_TRUE(set.match("/proj/sub/scripts/x.spp"));
    EXPECT_TRUE(set.match("/proj/other/x.spp"));
    EXPECT_TRUE(set.match("/proj/sub/one.spp"));
    EXPECT_FALSE(set.match("/proj/sub/two.spp"));
}

TEST_F(HostsTest, IncludesFileChangeForcesReload)
{
    const std::string build = make_build("a");
    std::ofstream(hosts_dir / "a.json")
        << R"({"version":1,"name":"a","binaryDir":")" << build << R"(","scriptDirs":["/src/a"],)"
        << R"("includesFile":"a.includes","flags":[]})";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_TRUE(set.match("/src/a/x.spp")->include_dirs.empty());  // a missing file is empty
    std::ofstream(hosts_dir / "a.includes") << "/i/one\n";
    EXPECT_TRUE(set.force_reload_for((hosts_dir / "a.includes").string()));
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.match("/src/a/x.spp")->include_dirs, std::vector<std::string>{"/i/one"});
}

TEST_F(HostsTest, LongestPrefixWins)
{
    write_host("outer", make_build("o"), R"("/src")");
    write_host("inner", make_build("i"), R"("/src/deep/dir")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/deep/dir/x.spp")->name, "inner");
    EXPECT_EQ(set.match("/src/other/x.spp")->name, "outer");
}

TEST_F(HostsTest, ExactScriptBeatsDir)
{
    write_host("dir", make_build("d"), R"("/src/deep/dir")");
    write_host("exact", make_build("e"), R"("/src")", R"("/src/deep/dir/x.spp")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/deep/dir/x.spp")->name, "exact");
    EXPECT_EQ(set.match("/src/deep/dir/y.spp")->name, "dir");
}

TEST_F(HostsTest, PrefixIsComponentAware)
{
    write_host("a", make_build("a"), R"("/a/b/")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_TRUE(set.match("/a/b/c.spp"));
    EXPECT_FALSE(set.match("/a/bc/x.spp"));
}

TEST_F(HostsTest, TieGoesToFirstName)
{
    write_host("zeta", make_build("z"), R"("/src")");
    write_host("alpha", make_build("a"), R"("/src")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/src/x.spp")->name, "alpha");
}

TEST_F(HostsTest, NoMatchGivesNull)
{
    write_host("a", make_build("a"), R"("/src/a")");
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.match("/elsewhere/x.spp"), nullptr);
    HostSet empty;
    EXPECT_EQ(empty.match("/src/a/x.spp"), nullptr);
}

TEST_F(HostsTest, SkipsInvalidJsonAndWrongVersion)
{
    write_host("good", make_build("g"), R"("/src")");
    std::ofstream(hosts_dir / "bad.json") << "{ not json";
    std::ofstream(hosts_dir / "v2.json")
        << R"({"version":2,"name":"v2","binaryDir":")" << make_build("v2") << R"(","scriptDirs":["/src"]})";
    std::ofstream(hosts_dir / "note.txt") << "ignored";
    HostSet set;
    set.load(hosts_dir.string());
    EXPECT_EQ(set.size(), 1u);
    EXPECT_EQ(set.match("/src/x.spp")->name, "good");
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
    EXPECT_EQ(set.match("/src/b/x.spp")->name, "b");
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
    EXPECT_EQ(set.match("/src/x.spp")->name, "late");
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
    EXPECT_EQ(set.match("/src/x.spp"), nullptr);
    EXPECT_FALSE(set.refresh());
    // The cache file returns: the host returns.
    std::ofstream(fs::path(build) / "CMakeCache.txt") << "# cache\n";
    EXPECT_TRUE(set.refresh());
    EXPECT_EQ(set.size(), 1u);
}

}  // namespace
