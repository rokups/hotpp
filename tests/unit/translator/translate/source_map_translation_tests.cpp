// Tests for TranslateResult::source_map: the map between generated C++ and the script.
#include <gtest/gtest.h>

#include <string>

#include "diag.h"
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;
using sppc::SourcePos;

namespace {

// The 1-based line of the first line that holds the text, or 0.
int line_of(const std::string& text, const std::string& needle)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos) return 0;
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
}

// Lines of the script: 1 using, 2 class, 3 field, 4 Update, 5 and 6 statements.
const char* const kScript =
    "using \"w.h\";\n"
    "class Foo : Widget {\n"
    "    int hits = 0;\n"
    "    override void Update(float dt) {\n"
    "        long total = hits + 2;\n"
    "        hits = total;\n"
    "    }\n"
    "}\n";

// The n bytes of the text at a 1-based line and column.
std::string text_at(const std::string& text, SourcePos p, size_t n)
{
    size_t off = 0;
    for (int l = 1; l < p.line; ++l) off = text.find('\n', off) + 1;
    return text.substr(off + static_cast<size_t>(p.col - 1), n);
}

sppc::TranslateResult translate_script(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "w.h",
               "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    return sppc::translate(temp_options(dir, script));
}

}  // namespace

TEST(SourceMapTranslation, IdentifiersMapBothWaysWithColumns)
{
    const sppc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    int64_t total = hits + 2;");
    ASSERT_GT(g, 0) << r.cpp;
    // Script line 5: "        long total = hits + 2;" (8 spaces).
    // Generated:     "    int64_t total = hits + 2;" (4 spaces).
    // "total": script col 14 to 18, generated col 13 to 17.
    EXPECT_EQ(r.source_map.map_to_generated(5, 14), (SourcePos{g, 13}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 16), (SourcePos{g, 15}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 18), (SourcePos{g, 17}));
    EXPECT_EQ(r.source_map.map_to_script(g, 13), (SourcePos{5, 14}));
    EXPECT_EQ(r.source_map.map_to_script(g, 15), (SourcePos{5, 16}));
    EXPECT_EQ(r.source_map.map_to_script(g, 17), (SourcePos{5, 18}));
    // "hits": script col 22 to 25, generated col 21 to 24.
    EXPECT_EQ(r.source_map.map_to_generated(5, 22), (SourcePos{g, 21}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 25), (SourcePos{g, 24}));
    EXPECT_EQ(r.source_map.map_to_script(g, 24), (SourcePos{5, 25}));
    // The number "2": script col 29, generated col 28.
    EXPECT_EQ(r.source_map.map_to_generated(5, 29), (SourcePos{g, 28}));
    EXPECT_EQ(r.source_map.map_to_script(g, 28), (SourcePos{5, 29}));
    // The next statement: "        hits = total;" and "    hits = total;".
    EXPECT_EQ(r.source_map.map_to_generated(6, 9), (SourcePos{g + 2, 5}));
    EXPECT_EQ(r.source_map.map_to_generated(6, 16), (SourcePos{g + 2, 12}));
    EXPECT_EQ(r.source_map.map_to_script(g + 2, 12), (SourcePos{6, 16}));
}

TEST(SourceMapTranslation, RewrittenTypeMapsToTheStartOfTheOtherRange)
{
    const sppc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    int64_t total = hits + 2;");
    ASSERT_GT(g, 0);
    // "long" (col 9 to 12) is "int64_t" (col 5 to 11): the lengths differ.
    EXPECT_EQ(r.source_map.map_to_generated(5, 9), (SourcePos{g, 5}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 12), (SourcePos{g, 5}));
    EXPECT_EQ(r.source_map.map_to_script(g, 5), (SourcePos{5, 9}));
    EXPECT_EQ(r.source_map.map_to_script(g, 11), (SourcePos{5, 9}));
}

TEST(SourceMapTranslation, TextWithoutSegmentGivesNoValue)
{
    const sppc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    int64_t total = hits + 2;");
    ASSERT_GT(g, 0);
    // The operator "+" and the spaces have no segment.
    EXPECT_FALSE(r.source_map.map_to_script(g, 26));
    EXPECT_FALSE(r.source_map.map_to_script(g, 4));
    EXPECT_FALSE(r.source_map.map_to_script(g, 100));
    EXPECT_FALSE(r.source_map.map_to_script(1, 1));  // the header comment
    EXPECT_FALSE(r.source_map.map_to_script(100000, 1));
    EXPECT_FALSE(r.source_map.map_to_script(0, 0));
    // The class header and the field are in the script but not in a body.
    EXPECT_FALSE(r.source_map.map_to_generated(2, 1));
    EXPECT_FALSE(r.source_map.map_to_generated(5, 20));  // the "="
    EXPECT_FALSE(r.source_map.map_to_generated(100, 1));
    EXPECT_FALSE(r.source_map.map_to_generated(5, 0));
}

TEST(SourceMapTranslation, EverySegmentOfEqualLengthRoundTrips)
{
    const sppc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_FALSE(r.source_map.empty());
    // The segments of the two statements: int64_t, total, hits, 2, hits, total. The segments
    // of the names: the field "hits" in the state struct, the function name and "dt" in the head.
    EXPECT_EQ(r.source_map.segments().size(), 9u);
    const sppc::LineMap gm(r.cpp), sm(kScript);
    for (const sppc::SourceSegment& s : r.source_map.segments()) {
        if (s.script_len != s.gen_len) continue;
        for (uint32_t d = 0; d < s.gen_len; ++d) {
            const auto sp = r.source_map.map_to_script(gm.line(s.gen_begin + d), gm.col(s.gen_begin + d));
            ASSERT_TRUE(sp);
            EXPECT_EQ(*sp, (SourcePos{sm.line(s.script_begin + d), sm.col(s.script_begin + d)}));
            const auto gp = r.source_map.map_to_generated(sp->line, sp->col);
            ASSERT_TRUE(gp);
            EXPECT_EQ(*gp, (SourcePos{gm.line(s.gen_begin + d), gm.col(s.gen_begin + d)}));
        }
        // The text is the same in both places.
        EXPECT_EQ(r.cpp.substr(s.gen_begin, s.gen_len), std::string(kScript).substr(s.script_begin, s.script_len));
    }
}

TEST(SourceMapTranslation, LocalNameInASecondMethodMapsToItsText)
{
    const std::string script =
        "using \"w.h\";\nclass Foo : Widget {\n"
        "    void a() { int x = 1; }\n"
        "    override void Update(float dt) { int yy = 2; yy = 3; }\n"
        "}\n";
    const sppc::TranslateResult r = translate_script(script);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // "yy" in the declaration: script line 4, col 42. "yy" in the assignment: col 50.
    const auto d = r.source_map.map_to_generated(4, 42);
    const auto u = r.source_map.map_to_generated(4, 50);
    ASSERT_TRUE(d);
    ASSERT_TRUE(u);
    EXPECT_EQ(text_at(r.cpp, *d, 5), "yy = ");
    EXPECT_EQ(text_at(r.cpp, *u, 5), "yy = ");
    EXPECT_NE(d->line, 0);
    EXPECT_EQ(r.source_map.map_to_script(d->line, d->col), (SourcePos{4, 42}));
    EXPECT_EQ(r.source_map.map_to_script(u->line, u->col), (SourcePos{4, 50}));
    // The first method: "x" at line 3, col 20.
    const auto x = r.source_map.map_to_generated(3, 20);
    ASSERT_TRUE(x);
    EXPECT_EQ(text_at(r.cpp, *x, 5), "x = 1");
}

TEST(SourceMapTranslation, FormatIncludeInsertDoesNotBreakTheMap)
{
    // A string with "${...}" adds an include line. The insert goes before all segments.
    const std::string script =
        "using \"w.h\";\nclass Foo : Widget {\n"
        "    override void Update(float dt) {\n"
        "        string s = \"v=${dt}\";\n"
        "        float zz = dt;\n"
        "    }\n"
        "}\n";
    const sppc::TranslateResult r = translate_script(script);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_NE(r.cpp.find("#include <format>"), std::string::npos);
    const auto g = r.source_map.map_to_generated(5, 15);  // "zz"
    ASSERT_TRUE(g);
    EXPECT_EQ(text_at(r.cpp, *g, 7), "zz = dt");
    EXPECT_EQ(r.source_map.map_to_script(g->line, g->col), (SourcePos{5, 15}));
}

TEST(SourceMapTranslation, FailedTranslationHasEmptyMap)
{
    const sppc::TranslateResult r = translate_script("class Foo : Widget { void f() { x = ; } }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.source_map.empty());
    EXPECT_FALSE(r.source_map.map_to_script(1, 1));
}

TEST(SourceMapType, ManualSegmentsWithUnsortedInput)
{
    // Script: "ab cd\nef\n". Generated: "XX\nef ab cd\n".
    sppc::SourceMap m;
    m.add({6, 2, 6, 2});  // "ef" at script line 2 -> generated line 2 col 1... offsets below
    m.add({0, 2, 9, 2});  // "ab"
    m.build("ab cd\nef\n", "XX\nYYY ef ab\n");
    // "ab": script offset 0 to 2 is generated offset 9 to 11: line 2, col 7.
    EXPECT_EQ(m.map_to_generated(1, 1), (SourcePos{2, 7}));
    EXPECT_EQ(m.map_to_generated(1, 2), (SourcePos{2, 8}));
    EXPECT_FALSE(m.map_to_generated(1, 3));
    // "ef": script offset 6 (line 2, col 1) is generated offset 6 (line 2, col 4).
    EXPECT_EQ(m.map_to_generated(2, 1), (SourcePos{2, 4}));
    EXPECT_EQ(m.map_to_script(2, 4), (SourcePos{2, 1}));
    EXPECT_EQ(m.map_to_script(2, 7), (SourcePos{1, 1}));
    EXPECT_FALSE(m.map_to_script(2, 6));
    EXPECT_FALSE(m.map_to_script(1, 1));
}

TEST(SourceMapType, EmptyMapGivesNoValue)
{
    sppc::SourceMap m;
    m.build("", "");
    EXPECT_TRUE(m.empty());
    EXPECT_FALSE(m.map_to_script(1, 1));
    EXPECT_FALSE(m.map_to_generated(1, 1));
}

TEST(SourceMapTranslation, FunctionHeadAndStateFieldNamesMapToTheScript)
{
    const sppc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The field "hits" (script line 3, col 9) is a field of the state struct.
    const int f = line_of(r.cpp, "    int32_t hits;");
    ASSERT_GT(f, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(f, 13), (SourcePos{3, 9}));
    EXPECT_EQ(r.source_map.map_to_generated(3, 10), (SourcePos{f, 14}));
    // The head of the generation function of "Update" (script line 4: name col 19, "dt" col 32).
    const int h = line_of(r.cpp, "static void Foo__Update(");
    ASSERT_GT(h, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(h, 13), (SourcePos{4, 19}));
    EXPECT_EQ(r.source_map.map_to_script(h, 20), (SourcePos{4, 19}));
    const std::string head = r.cpp.substr(r.cpp.find("static void Foo__Update("));
    const int dt_col = 1 + static_cast<int>(head.find(" dt)") + 1);
    EXPECT_EQ(r.source_map.map_to_script(h, dt_col), (SourcePos{4, 32}));
    EXPECT_EQ(r.source_map.map_to_script(h, dt_col + 1), (SourcePos{4, 33}));
}

// "this.hits" lowers to "st->hits". The member name has its own segment, so a request on "hits"
// in the script finds "hits" in the generated code (hover, go-to-definition, references).
TEST(SourceMapTranslation, MemberNameAfterThisMapsToTheScript)
{
    const char* const script =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int hits = 0;\n"
        "    override void Update(float dt) {\n"
        "        this.hits = this.hits + 2;\n"
        "    }\n"
        "}\n";
    const sppc::TranslateResult r = translate_script(script);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    st->hits = st->hits + 2;");
    ASSERT_GT(g, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(g, 9), (SourcePos{5, 14}));
    EXPECT_EQ(r.source_map.map_to_script(g, 20), (SourcePos{5, 26}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 15), (SourcePos{g, 10}));
}

// The map keeps the real script column of an expression, also at a column other than 1.
TEST(SourceMapTranslation, ExpressionMapsToItsRealScriptColumn)
{
    const sppc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    int64_t total = hits + 2;");
    ASSERT_GT(g, 0) << r.cpp;
    // "hits" in "hits + 2": generated col 21, script col 22 (8 spaces of indent).
    const std::optional<SourcePos> p = r.source_map.map_to_script(g, 21);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->line, 5);
    EXPECT_EQ(p->col, 22);
    const std::optional<SourcePos> back = r.source_map.map_to_generated(5, 22);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, (SourcePos{g, 21}));
}
