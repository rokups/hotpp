// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for TranslateResult::source_map: the map between generated C++ and the script.
#include "diag.h"
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using hotc::SourcePos;

namespace
{

// The 1-based line of the first line that holds the text, or 0.
int line_of(const std::string& text, const std::string& needle)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos)
        return 0;
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
    for (int l = 1; l < p.line; ++l)
        off = text.find('\n', off) + 1;
    return text.substr(off + static_cast<size_t>(p.col - 1), n);
}

hotc::TranslateResult translate_script(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    return hotc::translate(temp_options(dir, script));
}

} // namespace

TEST(SourceMapTranslation, IdentifiersMapBothWaysWithColumns)
{
    const hotc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    HOT_TRACE_LINE(); int64_t total = hits + 2;");
    ASSERT_GT(g, 0) << r.cpp;
    // Script line 5: "        long total = hits + 2;" (8 spaces).
    // Generated:     "    HOT_TRACE_LINE(); int64_t total = hits + 2;" (the macro is 18 columns).
    // "total": script col 14 to 18, generated col 31 to 35.
    EXPECT_EQ(r.source_map.map_to_generated(5, 14), (SourcePos{g, 31}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 16), (SourcePos{g, 33}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 18), (SourcePos{g, 35}));
    EXPECT_EQ(r.source_map.map_to_script(g, 31), (SourcePos{5, 14}));
    EXPECT_EQ(r.source_map.map_to_script(g, 33), (SourcePos{5, 16}));
    EXPECT_EQ(r.source_map.map_to_script(g, 35), (SourcePos{5, 18}));
    // "hits": script col 22 to 25, generated col 39 to 42.
    EXPECT_EQ(r.source_map.map_to_generated(5, 22), (SourcePos{g, 39}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 25), (SourcePos{g, 42}));
    EXPECT_EQ(r.source_map.map_to_script(g, 42), (SourcePos{5, 25}));
    // The number "2": script col 29, generated col 46.
    EXPECT_EQ(r.source_map.map_to_generated(5, 29), (SourcePos{g, 46}));
    EXPECT_EQ(r.source_map.map_to_script(g, 46), (SourcePos{5, 29}));
    // The next statement: "        hits = total;" and "    HOT_TRACE_LINE(); hits = total;".
    EXPECT_EQ(r.source_map.map_to_generated(6, 9), (SourcePos{g + 2, 23}));
    EXPECT_EQ(r.source_map.map_to_generated(6, 16), (SourcePos{g + 2, 30}));
    EXPECT_EQ(r.source_map.map_to_script(g + 2, 30), (SourcePos{6, 16}));
}

TEST(SourceMapTranslation, RewrittenTypeMapsToTheStartOfTheOtherRange)
{
    const hotc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    HOT_TRACE_LINE(); int64_t total = hits + 2;");
    ASSERT_GT(g, 0);
    // "long" (col 9 to 12) is "int64_t" (col 23 to 29): the lengths differ.
    EXPECT_EQ(r.source_map.map_to_generated(5, 9), (SourcePos{g, 23}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 12), (SourcePos{g, 23}));
    EXPECT_EQ(r.source_map.map_to_script(g, 23), (SourcePos{5, 9}));
    EXPECT_EQ(r.source_map.map_to_script(g, 29), (SourcePos{5, 9}));
}

TEST(SourceMapTranslation, TextWithoutSegmentGivesNoValue)
{
    const hotc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    HOT_TRACE_LINE(); int64_t total = hits + 2;");
    ASSERT_GT(g, 0);
    // The operator "+" and the spaces have no segment.
    EXPECT_FALSE(r.source_map.map_to_script(g, 44));
    EXPECT_FALSE(r.source_map.map_to_script(g, 4));
    EXPECT_FALSE(r.source_map.map_to_script(g, 100));
    EXPECT_FALSE(r.source_map.map_to_script(1, 1)); // the header comment
    EXPECT_FALSE(r.source_map.map_to_script(100000, 1));
    EXPECT_FALSE(r.source_map.map_to_script(0, 0));
    // The class header and the field are in the script but not in a body.
    EXPECT_FALSE(r.source_map.map_to_generated(2, 1));
    EXPECT_FALSE(r.source_map.map_to_generated(5, 20)); // the "="
    EXPECT_FALSE(r.source_map.map_to_generated(100, 1));
    EXPECT_FALSE(r.source_map.map_to_generated(5, 0));
}

TEST(SourceMapTranslation, EverySegmentOfEqualLengthRoundTrips)
{
    const hotc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_FALSE(r.source_map.empty());
    // The segments of the two statements: int64_t, total, hits, 2, hits, total. The segments
    // of the names: the field "hits" in the state struct, the function name and "dt" in the head
    // of the member. The override has no wrapper, so the head of the member is the only head.
    EXPECT_EQ(r.source_map.segments().size(), 9u);
    const hotc::LineMap gm(r.cpp), sm(kScript);
    for (const hotc::SourceSegment& s : r.source_map.segments())
    {
        if (s.script_len != s.gen_len)
            continue;
        for (uint32_t d = 0; d < s.gen_len; ++d)
        {
            const auto sp = r.source_map.map_to_script(gm.line(s.gen_begin + d), gm.col(s.gen_begin + d));
            ASSERT_TRUE(sp);
            EXPECT_EQ(*sp, (SourcePos{sm.line(s.script_begin + d), sm.col(s.script_begin + d)}));
            // The way back gives the first generated text of a script name. The head of the
            // member repeats the name of the head of the generation function.
            bool repeated = false;
            for (const hotc::SourceSegment& o : r.source_map.segments())
                repeated =
                    repeated || (o.file == s.file && o.script_begin == s.script_begin && o.gen_begin < s.gen_begin);
            if (repeated)
                continue;
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
    const hotc::TranslateResult r = translate_script(script);
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
    const hotc::TranslateResult r = translate_script(script);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_NE(r.cpp.find("#include <format>"), std::string::npos);
    const auto g = r.source_map.map_to_generated(5, 15); // "zz"
    ASSERT_TRUE(g);
    EXPECT_EQ(text_at(r.cpp, *g, 7), "zz = dt");
    EXPECT_EQ(r.source_map.map_to_script(g->line, g->col), (SourcePos{5, 15}));
}

TEST(SourceMapTranslation, FailedTranslationHasEmptyMap)
{
    const hotc::TranslateResult r = translate_script("class Foo : Widget { void f() { x = ; } }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.source_map.empty());
    EXPECT_FALSE(r.source_map.map_to_script(1, 1));
}

TEST(SourceMapType, ManualSegmentsWithUnsortedInput)
{
    // Script: "ab cd\nef\n". Generated: "XX\nef ab cd\n".
    hotc::SourceMap m;
    m.add({6, 2, 6, 2}); // "ef" at script line 2 -> generated line 2 col 1... offsets below
    m.add({0, 2, 9, 2}); // "ab"
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
    hotc::SourceMap m;
    m.build("", "");
    EXPECT_TRUE(m.empty());
    EXPECT_FALSE(m.map_to_script(1, 1));
    EXPECT_FALSE(m.map_to_generated(1, 1));
}

TEST(SourceMapTranslation, FunctionHeadAndStateFieldNamesMapToTheScript)
{
    const hotc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The field "hits" (script line 3, col 9) is a field of the state struct.
    const int f = line_of(r.cpp, "    int32_t hits;");
    ASSERT_GT(f, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(f, 13), (SourcePos{3, 9}));
    EXPECT_EQ(r.source_map.map_to_generated(3, 10), (SourcePos{f, 14}));
    // The head of the member "Update" (script line 4: name col 19, "dt" col 32).
    const int h = line_of(r.cpp, "void FooState_g1::Update(");
    ASSERT_GT(h, 0) << r.cpp;
    const std::string head = r.cpp.substr(r.cpp.find("void FooState_g1::Update("));
    const int name_col = 1 + static_cast<int>(head.find("Update("));
    EXPECT_EQ(r.source_map.map_to_script(h, name_col), (SourcePos{4, 19}));
    EXPECT_EQ(r.source_map.map_to_script(h, name_col + 3), (SourcePos{4, 22}));
    const int dt_col = 1 + static_cast<int>(head.find(" dt)") + 1);
    EXPECT_EQ(r.source_map.map_to_script(h, dt_col), (SourcePos{4, 32}));
    EXPECT_EQ(r.source_map.map_to_script(h, dt_col + 1), (SourcePos{4, 33}));
}

// "this.hits" lowers to "__st->hits". The member name has its own segment, so a request on "hits"
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
    const hotc::TranslateResult r = translate_script(script);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    HOT_TRACE_LINE(); __st->hits = __st->hits + 2;");
    ASSERT_GT(g, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(g, 29), (SourcePos{5, 14}));
    EXPECT_EQ(r.source_map.map_to_script(g, 42), (SourcePos{5, 26}));
    EXPECT_EQ(r.source_map.map_to_generated(5, 15), (SourcePos{g, 30}));
}

// The map keeps the real script column of an expression, also at a column other than 1.
TEST(SourceMapTranslation, ExpressionMapsToItsRealScriptColumn)
{
    const hotc::TranslateResult r = translate_script(kScript);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const int g = line_of(r.cpp, "    HOT_TRACE_LINE(); int64_t total = hits + 2;");
    ASSERT_GT(g, 0) << r.cpp;
    // "hits" in "hits + 2": generated col 39, script col 22 (8 spaces of indent).
    const std::optional<SourcePos> p = r.source_map.map_to_script(g, 39);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->line, 5);
    EXPECT_EQ(p->col, 22);
    const std::optional<SourcePos> back = r.source_map.map_to_generated(5, 22);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, (SourcePos{g, 39}));
}

namespace
{

// Translates a script with one statement in Update (script line 5). Then checks that the
// last name in the script maps to the last name in the generated text, and back.
void expect_last_part_maps(const std::string& stmt, const std::string& last)
{
    const std::string script = "using \"w.h\";\n"
                               "class Foo : Widget {\n"
                               "    int hits = 0;\n"
                               "    override void Update(float dt) {\n"
                               "        " + stmt + "\n"
                               "    }\n"
                               "}\n";
    TempDir dir;
    write_file(dir.path() / "w.h",
        "#include <cstdint>\n"
        "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
        "namespace Ns { struct T {}; int Get() { return 1; } template <class A> struct L { static int Make() { return "
        "1; } }; }\n"
        "template <class A> struct Pool { static int Get() { return 1; } };\n"
        "template <class A, class B> struct Map { static int Make() { return 1; } };\n");
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics << "\n" << r.cpp;
    // The last occurrence of the name in the statement is the call name.
    const size_t col0 = stmt.rfind(last);
    ASSERT_NE(col0, std::string::npos);
    const int script_col = 9 + static_cast<int>(col0);
    const std::optional<SourcePos> g = r.source_map.map_to_generated(5, script_col);
    ASSERT_TRUE(g.has_value()) << r.cpp;
    EXPECT_EQ(text_at(r.cpp, *g, last.size()), last) << r.cpp;
    const std::optional<SourcePos> s = r.source_map.map_to_script(g->line, g->col);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, (SourcePos{5, script_col}));
}

} // namespace

TEST(SourceMapTranslation, LastPartOfScopeNameMaps)
{
    expect_last_part_maps("int x = Ns.Get();", "Get");
}

TEST(SourceMapTranslation, LastPartAfterRewrittenTemplateTypeMaps)
{
    expect_last_part_maps("int x = Pool<ulong>.Get();", "Get");
}

TEST(SourceMapTranslation, LastPartAfterTemplateArgsWithCommaMaps)
{
    expect_last_part_maps("int x = Map<int,float>.Make();", "Make");
}

TEST(SourceMapTranslation, LastPartAfterTemplateArgsWithScopeMaps)
{
    expect_last_part_maps("int x = Ns.L<Ns.T>.Make();", "Make");
}
