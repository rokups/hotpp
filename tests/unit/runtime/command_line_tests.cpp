// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The command line helpers of the clang files backend (hot/command_line.h). They are pure, so
// the tests run on all platforms.
#include "hot/command_line.h"
#include "hot/runtime/process.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(CommandLine, QuoteLeavesPlainArgument)
{
    EXPECT_EQ(hot::windows_quote_arg("clang++"), "clang++");
    EXPECT_EQ(hot::windows_quote_arg("C:\\a\\b.cpp"), "C:\\a\\b.cpp"); // no quote: backslashes stay
    EXPECT_EQ(hot::windows_quote_arg("-DX=\\"), "-DX=\\");
}

TEST(CommandLine, QuoteAddsQuotes)
{
    EXPECT_EQ(hot::windows_quote_arg(""), "\"\"");
    EXPECT_EQ(hot::windows_quote_arg("a b"), "\"a b\"");
    EXPECT_EQ(hot::windows_quote_arg("a\tb"), "\"a\tb\"");
    EXPECT_EQ(hot::windows_quote_arg("C:\\Program Files\\x"), "\"C:\\Program Files\\x\"");
}

TEST(CommandLine, QuoteDoublesBackslashesBeforeQuote)
{
    EXPECT_EQ(hot::windows_quote_arg("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(hot::windows_quote_arg("a\\\"b"), "\"a\\\\\\\"b\"");
    // A backslash at the end goes before the closing quote: it becomes two.
    EXPECT_EQ(hot::windows_quote_arg("C:\\my dir\\"), "\"C:\\my dir\\\\\"");
}

TEST(CommandLine, JoinsArguments)
{
    EXPECT_EQ(hot::windows_command_line({}), "");
    EXPECT_EQ(hot::windows_command_line({"clang++", "-o", "a b.dll", ""}), "clang++ -o \"a b.dll\" \"\"");
}

TEST(CommandLine, MingwTriples)
{
    EXPECT_TRUE(hot::is_mingw_triple("x86_64-w64-mingw32"));
    EXPECT_TRUE(hot::is_mingw_triple("x86_64-w64-windows-gnu"));
    EXPECT_FALSE(hot::is_mingw_triple("x86_64-pc-windows-msvc"));
    EXPECT_FALSE(hot::is_mingw_triple("x86_64-pc-linux-gnu"));
    EXPECT_FALSE(hot::is_mingw_triple(""));
}

TEST(CommandLine, MingwFlagsNeedMingwTriple)
{
    EXPECT_TRUE(hot::mingw_target_flags("", "/sysroot").empty());
    EXPECT_TRUE(hot::mingw_target_flags("x86_64-pc-windows-msvc", "/sysroot").empty());
    const std::vector<std::string> expected = {"-fuse-ld=lld"};
    EXPECT_EQ(hot::mingw_target_flags("x86_64-w64-mingw32", ""), expected);
}

TEST(CommandLine, MingwFlagsUseHighestLibstdcxxVersion)
{
    const fs::path root =
        fs::temp_directory_path() / ("hot_command_line_test_" + std::to_string(hot::detail::process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    for (const char* v : {"9.5.0", "13.2.0", "13.10.1", "notes"})
        fs::create_directories(root / "include" / "c++" / v);
    const std::string sysroot = root.generic_string();
    const std::string base = sysroot + "/include/c++/13.10.1";
    const std::vector<std::string> expected = {"-nostdinc++", "-isystem", base, "-isystem",
        base + "/x86_64-w64-mingw32", "-isystem", base + "/backward", "-fuse-ld=lld"};
    EXPECT_EQ(hot::mingw_target_flags("x86_64-w64-windows-gnu", sysroot), expected);
    fs::remove_all(root, ec);
}

TEST(CommandLine, CxxDriver)
{
    EXPECT_EQ(hot::cxx_driver("clang"), "clang++");
    EXPECT_EQ(hot::cxx_driver("/opt/llvm/bin/clang"), "/opt/llvm/bin/clang++");
    EXPECT_EQ(hot::cxx_driver("C:\\llvm\\bin\\clang.exe"), "C:\\llvm\\bin\\clang++.exe");
    EXPECT_EQ(hot::cxx_driver("clang++"), "clang++");
    EXPECT_EQ(hot::cxx_driver("/usr/bin/clang-19"), "/usr/bin/clang-19");
    EXPECT_EQ(hot::cxx_driver("x86_64-w64-mingw32-g++"), "x86_64-w64-mingw32-g++");
}
