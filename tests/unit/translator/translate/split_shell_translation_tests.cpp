// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of the split mode (TranslateOptions::split_shell). The text before the line
// "// @hot-shell-end" depends only on the imports. An edit of an own body does not change it.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace
{

using namespace translator_test;

const char* kMarker = "// @hot-shell-end\n";

const char* kLib =
    "using \"w.h\";\n"
    "struct Pt {\n"
    "    int v = 1;\n"
    "    int Get() { return this.v; }\n"
    "    static int Make() { return 2; }\n"
    "}\n"
    "const int K = 3;\n"
    "int count = 0;\n"
    "class Base : Widget {\n"
    "    int hp = 1;\n"
    "    virtual int Power() { return 1; }\n"
    "    static int Make(int a = 1) { return a; }\n"
    "}\n"
    "int Helper(int a = 1) { return a; }\n";

const char* kImporter =
    "using \"w.h\";\n"
    "using \"lib.hot\";\n"
    "struct Own { int q = 0; }\n"
    "int OwnFn(int a) { return a + Helper() + count; }\n"
    "class Kid : Base {\n"
    "    override int Power() { return Helper() + K + count; }\n"
    "    int Use() { Pt p; return p.Get() + OwnFn(1); }\n"
    "}\n";

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

hotc::TranslateResult translate_split(
    const std::string& lib, const std::string& importer, bool split)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, importer);
    o.modules.emplace_back("lib.hot", lib);
    o.split_shell = split;
    return hotc::translate(o);
}

// The text before the marker. Empty when the text has no marker.
std::string shell_of(const hotc::TranslateResult& r)
{
    const size_t at = r.cpp.find(kMarker);
    return at == std::string::npos ? std::string() : r.cpp.substr(0, at);
}

TEST(SplitShellTranslation, OwnBodyEditKeepsTheShellText)
{
    const auto a = translate_split(kLib, kImporter, true);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string shell = shell_of(a);
    ASSERT_FALSE(shell.empty());
    // The shell has the imported bodies and the imported accessors, not an own body.
    EXPECT_NE(shell.find("Pt__Get(Pt* __self)\n"), std::string::npos);
    EXPECT_NE(shell.find("hot_fn_Helper(int32_t a)\n"), std::string::npos);
    EXPECT_NE(shell.find("hot_mv_count"), std::string::npos);
    EXPECT_EQ(shell.find("OwnFn"), std::string::npos);
    EXPECT_EQ(shell.find("class Kid"), std::string::npos);
    EXPECT_EQ(a.cpp.find(kMarker), a.cpp.rfind(kMarker));
    for (const char* edit : {"return a + Helper() + count;", "return Helper() + K + count;", "return p.Get() + OwnFn(1);"})
    {
        const auto b = translate_split(kLib,
            replaced(kImporter, edit, std::string("\n        int t = 1;\n        t = t + 2;\n        ") + edit), true);
        ASSERT_TRUE(b.ok) << b.diagnostics;
        EXPECT_EQ(shell, shell_of(b)) << edit;
        EXPECT_NE(a.cpp, b.cpp) << edit;
    }
}

TEST(SplitShellTranslation, OwnDeclarationEditKeepsTheShellText)
{
    const auto a = translate_split(kLib, kImporter, true);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const auto b = translate_split(kLib, replaced(kImporter, "struct Own { int q = 0; }", "struct Own { int q = 0; int r = 1; }"), true);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(shell_of(a), shell_of(b));
}

TEST(SplitShellTranslation, ImportEditChangesTheShellText)
{
    const auto a = translate_split(kLib, kImporter, true);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const auto b = translate_split(replaced(kLib, "int hp = 1;", "int hp = 1;\n    int mana = 2;"), kImporter, true);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(shell_of(a), shell_of(b));
}

TEST(SplitShellTranslation, FlagOffGivesNoMarkerAndTheSameCode)
{
    const auto off = translate_split(kLib, kImporter, false);
    const auto on = translate_split(kLib, kImporter, true);
    ASSERT_TRUE(off.ok) << off.diagnostics;
    ASSERT_TRUE(on.ok) << on.diagnostics;
    EXPECT_EQ(off.cpp.find("@hot-shell-end"), std::string::npos);
    EXPECT_NE(on.cpp.find(kMarker), std::string::npos);
    // The split text has the same lines, in another order, plus the marker. The banners and the
    // empty lines can repeat.
    auto lines = [](const std::string& t)
    {
        std::multiset<std::string> r;
        size_t at = 0;
        while (at < t.size())
        {
            const size_t e = t.find('\n', at);
            const std::string l = t.substr(at, e == std::string::npos ? e : e - at);
            if (!l.empty() && l.rfind("#line", 0) != 0 && l != "// @hot-shell-end" && l.rfind("// @hot-body-", 0) != 0 && l.rfind("// -------", 0) != 0)
                r.insert(l);
            at = e == std::string::npos ? t.size() : e + 1;
        }
        return r;
    };
    EXPECT_EQ(lines(off.cpp), lines(on.cpp));
}

} // namespace
