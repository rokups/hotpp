// Tests for hotc::analyze: the facts of a script that has syntax errors.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using hotc::SourcePos;

namespace
{

hotc::AnalyzeResult analyze_text(const std::string& text)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    return hotc::analyze(text, temp_options(dir, ""));
}

} // namespace

TEST(Analyze, ValidScriptHasNoErrorAndAMap)
{
    const hotc::AnalyzeResult r = analyze_text(
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int hits = 0;\n"
        "    override void Update(float dt) { hits = hits + 1; }\n"
        "}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.errors.empty());
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].name, "Foo");
    EXPECT_EQ(r.classes[0].base, "Widget");
    EXPECT_EQ(r.classes[0].begin, (SourcePos{2, 1}));
    ASSERT_EQ(r.classes[0].members.size(), 2u);
    EXPECT_EQ(r.classes[0].members[0].kind, "field");
    EXPECT_EQ(r.classes[0].members[0].name, "hits");
    EXPECT_EQ(r.classes[0].members[0].begin, (SourcePos{3, 9}));
    EXPECT_EQ(r.classes[0].members[1].kind, "method");
    EXPECT_EQ(r.classes[0].members[1].name, "Update");
    EXPECT_EQ(r.classes[0].members[1].begin, (SourcePos{4, 19}));
    EXPECT_EQ(r.classes[0].members[1].end, (SourcePos{4, 56}));
    EXPECT_FALSE(r.source_map.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(4, 38)); // the first "hits" in the body
}

TEST(Analyze, ErrorsInTwoMethodsKeepTheValidMethod)
{
    const hotc::AnalyzeResult r = analyze_text(
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    void a() { x = ; }\n"
        "    override void Update(float dt) { n = n + 1; }\n"
        "    void b() { y = 1 +; }\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 2u);
    EXPECT_EQ(r.errors[0].pos, (SourcePos{4, 20}));
    EXPECT_EQ(r.errors[0].message, "expected an expression");
    EXPECT_EQ(r.errors[1].pos, (SourcePos{6, 23}));
    ASSERT_EQ(r.classes.size(), 1u);
    ASSERT_EQ(r.classes[0].members.size(), 4u);
    EXPECT_EQ(r.classes[0].members[0].name, "n");
    EXPECT_EQ(r.classes[0].members[1].name, "a");
    EXPECT_EQ(r.classes[0].members[2].name, "Update");
    EXPECT_EQ(r.classes[0].members[3].name, "b");
    // The map has the valid method "Update". The text is at the original position.
    ASSERT_FALSE(r.source_map.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(5, 38)); // the first "n" in the body
    // The broken methods are not in the map.
    EXPECT_FALSE(r.source_map.map_to_generated(4, 16));
    EXPECT_FALSE(r.source_map.map_to_generated(6, 16));
}

TEST(Analyze, HalfTypedMemberAccessGivesACompletionPoint)
{
    const hotc::AnalyzeResult r = analyze_text(
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    override void Update(float dt) {\n"
        "        player.\n"
        "    }\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.completion_points.size(), 1u);
    EXPECT_EQ(r.completion_points[0], (SourcePos{4, 16}));
    ASSERT_GE(r.errors.size(), 1u);
    EXPECT_EQ(r.errors[0].pos, (SourcePos{4, 16}));
    EXPECT_EQ(r.errors[0].message, "expected a member name after '.'");
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].members.at(0).name, "Update");
}

TEST(Analyze, MissingClassBraceKeepsTheClassAndTheMembers)
{
    const hotc::AnalyzeResult r = analyze_text(
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    override void Update(float dt) { n = 1; }\n");
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_EQ(r.errors[0].pos, (SourcePos{5, 1}));
    EXPECT_EQ(r.errors[0].message, "expected '}'");
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].members.size(), 2u);
}

TEST(Analyze, MissingSemicolonIsOneErrorAndTheOtherMembersStay)
{
    const hotc::AnalyzeResult r = analyze_text(
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    void a() {\n"
        "        x = 1\n"
        "    }\n"
        "    override void Update(float dt) { }\n"
        "}\n");
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_EQ(r.errors[0].pos, (SourcePos{5, 5}));
    EXPECT_EQ(r.errors[0].message, "expected ';'");
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].members.size(), 2u);
}

TEST(Analyze, TranslateStillFailsForTheSameText)
{
    const std::string text = "using \"w.h\";\nclass Foo : Widget {\n    void a() { x = ; }\n}\n";
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} };\n");
    hotc::TranslateOptions o = temp_options(dir, text);
    const hotc::TranslateResult t = hotc::translate(o);
    EXPECT_FALSE(t.ok);
    EXPECT_TRUE(t.cpp.empty());
    EXPECT_EQ(t.diagnostics, "t.hot:3:20: error: expected an expression\n");
}

// The module attribute array gives the type of "T[N]". The type text of the analysis uses it.
TEST(Analyze, ArrayTemplateOfTheModuleAttributeApplies)
{
    const std::string text = "using \"w.h\";\n[array(\"my::Fixed<$T, $N>\")];\nclass Foo : Wrap<int[3]> {\n}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].base, "Wrap<my::Fixed<int32_t, 3>>");
    const hotc::AnalyzeResult d = analyze_text("using \"w.h\";\nclass Foo : Wrap<int[3]> {\n}\n");
    ASSERT_EQ(d.classes.size(), 1u);
    EXPECT_EQ(d.classes[0].base, "Wrap<std::array<int32_t, 3>>");
}

// The attribute can come after the first use of "T[N]". The analysis reads the text again with the template.
TEST(Analyze, ArrayTemplateAfterTheFirstUseApplies)
{
    const std::string text = "using \"w.h\";\nclass Foo : Wrap<int[3]> {\n}\n[array(\"my::Fixed<$T, $N>\")];\n";
    const hotc::AnalyzeResult r = analyze_text(text); // the header has no "my::Fixed": only the base text counts
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].base, "Wrap<my::Fixed<int32_t, 3>>");
    // A syntax error before the attribute is reported once.
    const hotc::AnalyzeResult e =
        analyze_text("using \"w.h\";\nclass Foo : Widget {\n    void a() { x = ; }\n}\n[array(\"my::Fixed<$T, $N>\")];\n");
    EXPECT_EQ(e.errors.size(), 1u) << e.diagnostics;
}

// The type text of the parser holds an internal placeholder for "std". The base text for a
// language server must not hold it: the module attribute gives the namespace.
TEST(Analyze, BaseTextHasNoPlaceholderByte)
{
    const std::string text = "using \"w.h\";\n[std(\"mystd\")];\nclass Foo : Wrap<std.string> {\n    int hits = 0;\n}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    ASSERT_EQ(r.classes.size(), 1u);
    EXPECT_EQ(r.classes[0].base, "Wrap<mystd::string>");
    EXPECT_EQ(r.classes[0].base.find('\x01'), std::string::npos);
    const hotc::AnalyzeResult d = analyze_text("using \"w.h\";\nclass Foo : Wrap<std.string> {\n}\n");
    ASSERT_EQ(d.classes.size(), 1u);
    EXPECT_EQ(d.classes[0].base, "Wrap<std::string>");
}
