// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// default_paths(): a development build gets the values from the build tree.
#include "hot/paths.h"

#include <fstream>
#include <string>

#include <gtest/gtest.h>

TEST(DefaultPaths, DevBuildHasCompilerAndIncludeDirs)
{
#ifdef HOT_PORTABLE
    GTEST_SKIP() << "The test covers the development build.";
#else
    const hot::DefaultPaths p = hot::default_paths();
    EXPECT_FALSE(p.compiler.empty());
    EXPECT_FALSE(p.include_dirs.empty());
    EXPECT_FALSE(p.work_dir.empty());
#endif
}

#ifndef _WIN32
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>

TEST(DefaultPaths, CheckWorkDirCreatesAndRejectsUnsafeDirs)
{
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/hot_paths_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    const fs::path root = tmpl;

    // A missing directory is created with mode 0700.
    const fs::path dir = root / "work";
    EXPECT_EQ(hot::check_work_dir(dir.string()), "");
    struct stat st;
    ASSERT_EQ(stat(dir.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0700u);
    EXPECT_EQ(hot::check_work_dir(dir.string()), "");

    // A directory that other users can write to is refused.
    ASSERT_EQ(chmod(dir.c_str(), 0770), 0);
    EXPECT_NE(hot::check_work_dir(dir.string()).find("can be written"), std::string::npos);
    ASSERT_EQ(chmod(dir.c_str(), 0707), 0);
    EXPECT_FALSE(hot::check_work_dir(dir.string()).empty());

    // A file is refused. A symlink to a directory is refused.
    const fs::path file = root / "file";
    { std::ofstream(file) << "x"; }
    EXPECT_NE(hot::check_work_dir(file.string()).find("not a directory"), std::string::npos);
    const fs::path link = root / "link";
    fs::create_directory_symlink(root, link);
    EXPECT_FALSE(hot::check_work_dir(link.string()).empty());

    // An empty name is refused.
    EXPECT_FALSE(hot::check_work_dir("").empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif

#ifdef _WIN32
#include <filesystem>
#include <process.h>

// The Windows form of check_work_dir: it creates the directory and refuses a file or an empty name.
// The test uses the Windows form of the path: a drive letter and backslashes.
TEST(DefaultPaths, CheckWorkDirCreatesAndRejectsUnsafeDirsWindows)
{
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path();
    const std::string base = temp.string();
    ASSERT_GE(base.size(), 3u);
    ASSERT_EQ(base[1], ':'); // a drive letter
    const fs::path root = temp / ("hot_paths_test_" + std::to_string(_getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);

    // A missing directory, also a nested one, is created. The name has backslashes only.
    std::string dir = (root / "work" / "inner").string();
    for (char& c : dir)
        if (c == '/')
            c = '\\';
    EXPECT_EQ(hot::check_work_dir(dir), "");
    EXPECT_TRUE(fs::is_directory(dir));
    EXPECT_EQ(hot::check_work_dir(dir), "");

    // A file is refused.
    const fs::path file = root / "file";
    { std::ofstream(file) << "x"; }
    EXPECT_NE(hot::check_work_dir(file.string()).find("cannot create the work directory"), std::string::npos);

    // An empty name is refused.
    EXPECT_FALSE(hot::check_work_dir("").empty());

    fs::remove_all(root, ec);
}
#endif
