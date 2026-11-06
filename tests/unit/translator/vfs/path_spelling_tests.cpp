// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the path helpers of the include search: clang_path, clang_spellings,
// text_without_dot_dot, host_path_alternatives, host_stat and host_read
// (src/translator/file_time.h).
#include "file_time.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
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

TEST(HostPathAlternatives, GivesTheOtherNamesOnceWithoutThePath)
{
    const std::vector<std::string> plain = hotc::host_path_alternatives("/d/x.h");
    EXPECT_FALSE(has(plain, "/d/x.h"));
#ifdef _WIN32
    // lexically_normal gives the preferred separator, so the only other name has backslashes.
    EXPECT_EQ(plain, std::vector<std::string>{"\\d\\x.h"});
#else
    EXPECT_TRUE(plain.empty());
#endif
    const std::vector<std::string> c = hotc::host_path_alternatives("/d/a/../b/y.h");
    EXPECT_FALSE(has(c, "/d/a/../b/y.h"));
    ASSERT_FALSE(c.empty());
    EXPECT_EQ(c.back(), "/d/b/y.h");
    for (const std::string& s : c)
        EXPECT_EQ(std::count(c.begin(), c.end(), s), 1) << s;
}

// A file system in memory. It records each name that a lookup asks for.
class RecordingFs final : public hot::FileSystem
{
public:
    bool read(const std::string& path, std::string& out) override
    {
        asked.push_back(path);
        const auto it = files.find(path);
        if (it == files.end())
            return false;
        out = it->second;
        return true;
    }
    hot::VfsStat stat(const std::string& path) override
    {
        asked.push_back(path);
        if (files.count(path) == 0)
            return {};
        return {true, 7};
    }
    std::map<std::string, std::string> files;
    std::vector<std::string> asked;
};

// The folder has "\\" (the Windows form). The rest of the name has "/" and "..". The host file
// system holds the name without "a/../" and with the same separators.
const char* const mixed_path = "C:\\t\\d/a/../b/y.h";
const char* const mixed_name = "C:\\t\\d/b/y.h";

TEST(HostLookup, FindsAMixedSeparatorNameWithoutDotDot)
{
    RecordingFs fs;
    fs.files[mixed_name] = "int y;";
    const hot::VfsStat st = hotc::host_stat(fs, mixed_path);
    EXPECT_TRUE(st.exists);
    EXPECT_EQ(st.version, 7u);
    std::string text;
    EXPECT_TRUE(hotc::host_read(fs, mixed_path, text));
    EXPECT_EQ(text, "int y;");
    EXPECT_FALSE(hotc::host_stat(fs, "C:\\t\\d/a/../c/y.h").exists);
    EXPECT_FALSE(hotc::host_read(fs, "C:\\t\\d/a/../c/y.h", text));
}

TEST(HostLookup, TriesThePathFirstThenTheAlternativesInOrder)
{
    RecordingFs fs;
    fs.files[mixed_path] = "int a;";
    fs.files[mixed_name] = "int y;";
    // A hit on the path asks for no other name.
    EXPECT_TRUE(hotc::host_stat(fs, mixed_path).exists);
    std::string text;
    EXPECT_TRUE(hotc::host_read(fs, mixed_path, text));
    EXPECT_EQ(text, "int a;");
    EXPECT_EQ(fs.asked, (std::vector<std::string>{mixed_path, mixed_path}));
    // After a miss, the lookup asks for the path, then each alternative until a hit.
    fs.files.erase(mixed_path);
    fs.asked.clear();
    EXPECT_TRUE(hotc::host_stat(fs, mixed_path).exists);
    std::vector<std::string> want = {mixed_path};
    for (const std::string& s : hotc::host_path_alternatives(mixed_path))
    {
        want.push_back(s);
        if (s == mixed_name)
            break;
    }
    EXPECT_EQ(fs.asked.back(), mixed_name);
    EXPECT_EQ(fs.asked, want);
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
