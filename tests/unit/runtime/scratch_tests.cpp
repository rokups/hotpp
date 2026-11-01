// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The scratch directory helpers: removal at exit, HOT_KEEP_TEMP and the safe remove.
#include "hot/runtime/process.h"
#include "hot/scratch.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>

namespace
{

namespace fs = std::filesystem;

std::string test_dir(const char* tag)
{
    return (fs::temp_directory_path() / ("hot_scratch_test_" + std::string(tag) + std::to_string(hot::detail::process_id()))).string();
}

// Each test sets the variable itself. The fixture sets it to "0" before the test, so the CMake
// option has no effect. It restores the old value after the test.
struct Scratch : ::testing::Test
{
    void SetUp() override
    {
        const char* old = getenv("HOT_KEEP_TEMP");
        had_old_ = old != nullptr;
        if (old)
            old_ = old;
        test_support::set_env("HOT_KEEP_TEMP", "0");
    }
    void TearDown() override
    {
        if (had_old_)
            test_support::set_env("HOT_KEEP_TEMP", old_.c_str());
        else
            test_support::unset_env("HOT_KEEP_TEMP");
    }

    bool had_old_ = false;
    std::string old_;
};

TEST_F(Scratch, DirectoryIsRemovedOnDestruction)
{
    const std::string path = test_dir("rm");
    {
        hot::ScratchDir dir(path);
        EXPECT_TRUE(fs::is_directory(path));
        std::ofstream(dir.file("a.txt")) << "x";
        EXPECT_TRUE(fs::exists(dir.file("a.txt")));
    }
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(Scratch, ConstructorRemovesOldContent)
{
    const std::string path = test_dir("old");
    fs::create_directories(path);
    std::ofstream(path + "/old.txt") << "x";
    {
        hot::ScratchDir dir(path);
        EXPECT_FALSE(fs::exists(dir.file("old.txt")));
    }
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(Scratch, KeepTempKeepsTheDirectory)
{
    const std::string path = test_dir("keep");
    test_support::set_env("HOT_KEEP_TEMP", "1");
    EXPECT_TRUE(hot::keep_temp());
    {
        hot::ScratchDir dir(path);
    }
    EXPECT_TRUE(fs::is_directory(path));
    std::error_code ec;
    fs::remove_all(path, ec);
}

TEST_F(Scratch, ZeroOrEmptyMeansFalse)
{
    test_support::set_env("HOT_KEEP_TEMP", "0");
    EXPECT_FALSE(hot::keep_temp());
    test_support::set_env("HOT_KEEP_TEMP", "");
    EXPECT_FALSE(hot::keep_temp());
}

TEST_F(Scratch, PredicateAcceptsOnlySafePaths)
{
#ifdef _WIN32
    EXPECT_TRUE(hot::is_removable_scratch_path("C:/tmp/x/p1")); // a Windows path needs a drive to be absolute
#else
    EXPECT_TRUE(hot::is_removable_scratch_path("/tmp/x/p1"));
#endif
    EXPECT_FALSE(hot::is_removable_scratch_path("/"));
    EXPECT_FALSE(hot::is_removable_scratch_path("/."));
    EXPECT_FALSE(hot::is_removable_scratch_path("/tmp/.."));
    EXPECT_FALSE(hot::is_removable_scratch_path("/a/../.."));
    EXPECT_FALSE(hot::is_removable_scratch_path("/tmp/x/../y"));
    EXPECT_FALSE(hot::is_removable_scratch_path("rel"));
    EXPECT_FALSE(hot::is_removable_scratch_path(""));
#ifdef _WIN32
    EXPECT_FALSE(hot::is_removable_scratch_path("C:/"));
    EXPECT_FALSE(hot::is_removable_scratch_path("C:/tmp/.."));
#endif
}

TEST_F(Scratch, RemoveIgnoresRelativePaths)
{
    // The sentinel is a relative path. The call must not remove it.
    const std::string rel = "hot_scratch_relative_" + std::to_string(hot::detail::process_id());
    fs::create_directories(rel);
    hot::remove_scratch(rel);
    EXPECT_TRUE(fs::is_directory(rel));
    std::error_code ec;
    fs::remove_all(rel, ec);
}

} // namespace
