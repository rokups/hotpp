// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with string interpolation: the format function and the include.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace translator_test;

namespace
{

const char* kHeader =
    "#include <string>\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "    std::string Name;\n"
    "};\n";

hotc::TranslateResult translate_with(const std::string& init, const std::string& lib)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"u.h\";\nusing <string>;\nclass P : Unit {\n    int hp = 3;\n"
        "    override void Update(float dt) { this.Name = "
            + init + "; }\n}\n");
    if (!lib.empty())
        o.format_lib = lib;
    return hotc::translate(o);
}

bool has(const hotc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

} // namespace

TEST(StringInterpolationTranslation, StdFormatAndInclude)
{
    const hotc::TranslateResult r = translate_with("\"hp = ${this.hp}\"", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "std::format(\"hp = {}\", __st->hp)")) << r.cpp;
    EXPECT_TRUE(has(r, "#include <format>\n")) << r.cpp;
    EXPECT_FALSE(has(r, "fmt/format.h"));
}

TEST(StringInterpolationTranslation, FmtFormatAndInclude)
{
    const hotc::TranslateResult r = translate_with("\"hp = ${this.hp:>4}\"", "fmt");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "fmt::format(\"hp = {:>4}\", __st->hp)")) << r.cpp;
    EXPECT_TRUE(has(r, "#include <fmt/format.h>\n")) << r.cpp;
    EXPECT_FALSE(has(r, "#include <format>"));
}

TEST(StringInterpolationTranslation, NoInterpolationNoInclude)
{
    for (const char* lib : {"", "fmt"})
    {
        const hotc::TranslateResult r = translate_with("\"hp {x}\"", lib);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        EXPECT_FALSE(has(r, "format")) << r.cpp;
        EXPECT_TRUE(has(r, "= \"hp {x}\";")) << r.cpp;
    }
}

TEST(StringInterpolationTranslation, IncludeKeepsTheLineNumbers)
{
    const hotc::TranslateResult a = translate_with("\"hp\"", "");
    const hotc::TranslateResult b = translate_with("\"hp ${this.hp}\"", "");
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(std::count(a.cpp.begin(), a.cpp.end(), '\n'), std::count(b.cpp.begin(), b.cpp.end(), '\n'));
}

TEST(StringInterpolationTranslation, UnknownFormatLibraryIsAnError)
{
    const hotc::TranslateResult r = translate_with("\"hp\"", "printf");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("format library"), std::string::npos) << r.diagnostics;
}

TEST(StringInterpolationTranslation, SyntaxErrorInExpressionHasAScriptPosition)
{
    const hotc::TranslateResult r = translate_with("\"a ${1 +} b\"", "");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("t.hot:5:"), std::string::npos) << r.diagnostics;
}

// A script enum is an "enum class": std::format has no formatter for it. "${m}" prints the
// underlying integer.
TEST(StringInterpolationTranslation, ScriptEnumPrintsTheUnderlyingInteger)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    const hotc::TranslateResult r = hotc::translate(temp_options(dir,
        "using \"u.h\";\nusing <string>;\nenum Mode { Idle, Walk }\nenum Small : uint8_t { A, B }\n"
        "class P : Unit {\n    Mode m = Mode.Idle;\n    Small s = Small.B;\n"
        "    override void Update(float dt) { this.Name = \"m=${m} s=${this.s} e=${Mode.Walk}\"; }\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static_cast<int32_t>(")) << r.cpp;
    EXPECT_TRUE(has(r, "static_cast<uint8_t>(")) << r.cpp;
    EXPECT_TRUE(has(r, "static_cast<int32_t>(Mode::Walk)")) << r.cpp;
}
