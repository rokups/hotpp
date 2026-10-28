// Tests for sppc::translate with string interpolation: the format function and the include.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

const char* kHeader =
    "#include <string>\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "    std::string Name;\n"
    "};\n";

sppc::TranslateResult translate_with(const std::string& init, const std::string& lib)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    sppc::TranslateOptions o = temp_options(
        dir, "using \"u.h\";\nusing <string>;\nclass P : Unit {\n    int hp = 3;\n"
             "    override void Update(float dt) { this.Name = " + init + "; }\n}\n");
    if (!lib.empty()) o.format_lib = lib;
    return sppc::translate(o);
}

bool has(const sppc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

}  // namespace

TEST(StringInterpolationTranslation, StdFormatAndInclude)
{
    const sppc::TranslateResult r = translate_with("\"hp = ${this.hp}\"", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "std::format(\"hp = {}\", st->hp)")) << r.cpp;
    EXPECT_TRUE(has(r, "#include <format>\n")) << r.cpp;
    EXPECT_FALSE(has(r, "fmt/format.h"));
}

TEST(StringInterpolationTranslation, FmtFormatAndInclude)
{
    const sppc::TranslateResult r = translate_with("\"hp = ${this.hp:>4}\"", "fmt");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "fmt::format(\"hp = {:>4}\", st->hp)")) << r.cpp;
    EXPECT_TRUE(has(r, "#include <fmt/format.h>\n")) << r.cpp;
    EXPECT_FALSE(has(r, "#include <format>"));
}

TEST(StringInterpolationTranslation, NoInterpolationNoInclude)
{
    for (const char* lib : {"", "fmt"}) {
        const sppc::TranslateResult r = translate_with("\"hp {x}\"", lib);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        EXPECT_FALSE(has(r, "format")) << r.cpp;
        EXPECT_TRUE(has(r, "= \"hp {x}\";")) << r.cpp;
    }
}

TEST(StringInterpolationTranslation, IncludeKeepsTheLineNumbers)
{
    const sppc::TranslateResult a = translate_with("\"hp\"", "");
    const sppc::TranslateResult b = translate_with("\"hp ${this.hp}\"", "");
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(std::count(a.cpp.begin(), a.cpp.end(), '\n'), std::count(b.cpp.begin(), b.cpp.end(), '\n'));
}

TEST(StringInterpolationTranslation, UnknownFormatLibraryIsAnError)
{
    const sppc::TranslateResult r = translate_with("\"hp\"", "printf");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("format library"), std::string::npos) << r.diagnostics;
}

TEST(StringInterpolationTranslation, SyntaxErrorInExpressionHasAScriptPosition)
{
    const sppc::TranslateResult r = translate_with("\"a ${1 +} b\"", "");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("t.spp:5:"), std::string::npos) << r.diagnostics;
}
