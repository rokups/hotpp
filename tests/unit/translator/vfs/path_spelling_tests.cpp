// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the path helpers of the include search: clang_path, clang_spellings,
// text_without_dot_dot and host_path_candidates (src/translator/file_time.h).
#include "file_time.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

bool has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

TEST(TextWithoutDotDot, RemovesANameAndItsDotDot)
{
    EXPECT_EQ(hotc::text_without_dot_dot("/d/a/../b/y.h"), "/d/b/y.h");
    EXPECT_EQ(hotc::text_without_dot_dot("/d/a/../b/../a/x.h"), "/d/a/x.h");
    EXPECT_EQ(hotc::text_without_dot_dot("/d/p/q/../../y.h"), "/d/y.h");
}

TEST(TextWithoutDotDot, KeepsTheSeparators)
{
    EXPECT_EQ(hotc::text_without_dot_dot("C:\\t\\d/a/../b/y.h"), "C:\\t\\d/b/y.h");
    EXPECT_EQ(hotc::text_without_dot_dot("C:/t/d\\a\\../b/y.h"), "C:/t/d\\b/y.h");
}

TEST(TextWithoutDotDot, RemovesTheFirstName)
{
    EXPECT_EQ(hotc::text_without_dot_dot("a/../b.h"), "b.h");
    EXPECT_EQ(hotc::text_without_dot_dot("a/b/../../c.h"), "c.h");
}

TEST(TextWithoutDotDot, RemovesADotDotAtTheEnd)
{
    EXPECT_EQ(hotc::text_without_dot_dot("/d/a/.."), "/d/");
    EXPECT_EQ(hotc::text_without_dot_dot("a/.."), "");
}

TEST(TextWithoutDotDot, KeepsWhatIsNotAName)
{
    EXPECT_EQ(hotc::text_without_dot_dot("../x.h"), "../x.h");
    EXPECT_EQ(hotc::text_without_dot_dot("/../x.h"), "/../x.h");
    EXPECT_EQ(hotc::text_without_dot_dot("C:/../x.h"), "C:/../x.h");
    EXPECT_EQ(hotc::text_without_dot_dot("./../x.h"), "./../x.h");
    EXPECT_EQ(hotc::text_without_dot_dot("/d/a..b/x.h"), "/d/a..b/x.h");
    EXPECT_EQ(hotc::text_without_dot_dot("/d/a/..b/x.h"), "/d/a/..b/x.h");
}

TEST(HostPathCandidates, ThePathComesFirstAndEachNameIsOnce)
{
    const std::vector<std::string> plain = hotc::host_path_candidates("/d/x.h");
    ASSERT_FALSE(plain.empty());
    EXPECT_EQ(plain.front(), "/d/x.h");
    const std::vector<std::string> c = hotc::host_path_candidates("/d/a/../b/y.h");
    EXPECT_EQ(c.front(), "/d/a/../b/y.h");
    EXPECT_TRUE(has(c, "/d/b/y.h"));
    for (const std::string& s : c)
        EXPECT_EQ(std::count(c.begin(), c.end(), s), 1) << s;
}

#ifdef _WIN32

TEST(ClangPath, GivesTheSlashForm)
{
    EXPECT_EQ(hotc::clang_path("C:\\t\\d\\x.h"), "C:/t/d/x.h");
}

TEST(ClangSpellings, TheFirstNameIsTheClangPath)
{
    const std::vector<std::string> s = hotc::clang_spellings("C:\\t\\d/a/x.h");
    ASSERT_FALSE(s.empty());
    EXPECT_EQ(s.front(), "C:/t/d/a/x.h");
    EXPECT_TRUE(has(s, "C:/t/d\\a/x.h"));
    EXPECT_TRUE(has(s, "C:\\t\\d\\a\\x.h"));
}

// The names that the include search builds for an include cycle with ".." parts.
TEST(ClangSpellings, CoversTheRelativeIncludesWithDotDot)
{
    const std::vector<std::string> s = hotc::clang_spellings("C:\\t\\d/a/../b/../a/x.h");
    EXPECT_TRUE(has(s, "C:/t/d\\a\\../b\\../a/x.h"));
    EXPECT_TRUE(has(hotc::clang_spellings("C:\\t\\d/a/../b/y.h"), "C:/t/d\\a\\../b/y.h"));
    const std::vector<std::string> two = hotc::clang_spellings("C:\\t\\d/p/q/../../y.h");
    EXPECT_TRUE(has(two, "C:/t/d\\p/q\\../../y.h"));
}

TEST(ClangSpellings, HasAtMostTheNamesOfEightSeparators)
{
    const std::vector<std::string> s = hotc::clang_spellings("C:/a/../b/../c/../d/../e/../f/x.h");
    EXPECT_LE(s.size(), 512u);
}

#else

TEST(ClangPath, DoesNotChangeAPosixPath)
{
    EXPECT_EQ(hotc::clang_path("/d/a/../x.h"), "/d/a/../x.h");
    EXPECT_EQ(hotc::clang_path("rel/x.h"), "rel/x.h");
}

TEST(ClangSpellings, IsThePathOnlyOnPosix)
{
    EXPECT_EQ(hotc::clang_spellings("/d/a/../b/y.h"), std::vector<std::string>{"/d/a/../b/y.h"});
}

#endif

} // namespace
