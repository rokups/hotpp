// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::analyze: the facts of a script that has syntax errors.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
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

// A semantic error (not a syntax error) in one method must not remove the generated text of the
// other methods. The map of the valid method works.
TEST(Analyze, BadOverrideInOneMethodKeepsTheMapOfTheOtherMethod)
{
    const std::string text =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    override void Missing(float dt) { n = 1; }\n"
        "    void b() { n = n + 2; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("error"), std::string::npos) << r.diagnostics;
    EXPECT_FALSE(r.cpp.empty());
    EXPECT_FALSE(r.source_map.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16)); // the first "n" in the body of "b"
    EXPECT_NE(r.cpp.find("n + 2"), std::string::npos);
    EXPECT_EQ(r.cpp.find("n = 1"), std::string::npos); // the bad method is left out
}

// Two bad overrides in one class: both members go in one pass or in two passes. The method
// between them stays. The first diagnostics stay in the result.
TEST(Analyze, TwoBadOverridesKeepTheMethodBetweenThem)
{
    const std::string text =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    override void Missing(float dt) { n = 1; }\n"
        "    void b() { n = n + 3; }\n"
        "    override void Other(int k) { n = 4; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("t.hot:4:"), std::string::npos) << r.diagnostics; // the first pass
    EXPECT_NE(r.cpp.find("n + 3"), std::string::npos);
    EXPECT_EQ(r.cpp.find("n = 4"), std::string::npos);
}

// A "{" in a string or a comment must not move the end of the class. The other classes keep their map.
TEST(Analyze, BraceInStringDoesNotExtendTheClassRegion)
{
    const std::string text =
        "using \"w.h\";\n"
        "class A : Widget {\n"
        "    string s() { return \"{\"; } // {\n"
        "    override void Missing(float dt) { }\n"
        "}\n"
        "class B : Widget {\n"
        "    int n = 0;\n"
        "    void b() { n = n + 7; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.cpp.find("n + 7"), std::string::npos) << r.diagnostics;
    EXPECT_TRUE(r.source_map.map_to_generated(8, 16));
}

// An error at the class header (position before the colon) must not blank only the base list.
// Without the base, a later pass would blank the "override" member and the class would pass.
TEST(Analyze, HeaderErrorBlanksTheWholeClassNotTheBaseList)
{
    const std::string text =
        "using \"w.h\";\n"
        "class A : NoSuchBase {\n"
        "    override void Missing(float dt) { }\n"
        "}\n"
        "class B : Widget {\n"
        "    int n = 0;\n"
        "    void b() { n = n + 5; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.cpp.find("A_record"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("n + 5"), std::string::npos) << r.diagnostics;
}

// A field of an unknown type next to a good method: the method stays in the text.
TEST(Analyze, UnknownFieldTypeKeepsTheGoodMethod)
{
    const std::string text =
        "using \"w.h\";\n"
        "class A : Widget {\n"
        "    Zork z;\n"
        "    int n = 0;\n"
        "    override void Missing(float dt) { }\n"
        "    void b() { n = n + 9; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.cpp.find("n + 9"), std::string::npos) << r.diagnostics;
}

// An error inside an imported header blanks the "using" line of that import. The importer keeps the
// map of its own methods.
TEST(Analyze, BrokenImportIsBlankedAndTheMethodsKeepTheirMap)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    write_file(dir.path() / "bad.h", "int broken( ;\nstruct Bad { int x = ; };\n");
    const std::string text =
        "using \"w.h\";\n"
        "using \"bad.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    void b() { n = n + 6; }\n"
        "}\n";
    const hotc::AnalyzeResult r = hotc::analyze(text, temp_options(dir, ""));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.cpp.find("n + 6"), std::string::npos) << r.diagnostics;
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16));
}

// A syntax error in a method blanks only the statements from the error statement to the end of the
// body. The lines before it keep their map.
TEST(Analyze, ParseErrorKeepsTheStatementsBeforeIt)
{
    const std::string text =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    void b() {\n"
        "        n = n + 1;\n"
        "        if (n > 0) {\n"
        "            n = foo(;\n"
        "        }\n"
        "    }\n"
        "    void c() { n = n + 8; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(5, 9)) << r.diagnostics; // "n" on line 5
    EXPECT_TRUE(r.source_map.map_to_generated(10, 16));
    EXPECT_NE(r.cpp.find("n + 1"), std::string::npos);
    EXPECT_EQ(r.cpp.find("foo("), std::string::npos);
}

// A syntax error in a property accessor blanks the whole property. The class stays intact and the
// later method keeps its map.
TEST(Analyze, ParseErrorInAPropertySetterKeepsTheLaterMethod)
{
    const std::string text =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    int X { get { return n; } set { foo(; } }\n"
        "    void c() { n = n + 8; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16)) << r.diagnostics;
    EXPECT_NE(r.cpp.find("n + 8"), std::string::npos);
}

// A brace in a string of a property accessor is not a block brace. The later method keeps its map.
TEST(Analyze, ParseErrorInAPropertySetterWithOpenBraceStringKeepsTheLaterMethod)
{
    const std::string text =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    int X { get { return n; } set { string s = \"{\"; foo(; } }\n"
        "    void c() { n = n + 8; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16)) << r.diagnostics;
    EXPECT_NE(r.cpp.find("n + 8"), std::string::npos);
}

// A brace in a string of a property accessor is not a block brace. The later method keeps its map.
TEST(Analyze, ParseErrorInAPropertySetterWithCloseBraceStringKeepsTheLaterMethod)
{
    const std::string text =
        "using \"w.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    int X { get { return n; } set { string s = \"}\"; foo(; } }\n"
        "    void c() { n = n + 8; }\n"
        "}\n";
    const hotc::AnalyzeResult r = analyze_text(text);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16)) << r.diagnostics;
    EXPECT_NE(r.cpp.find("n + 8"), std::string::npos);
}

// The import match uses a path separator boundary: "w.h" is not the end of "neww.h".
TEST(Analyze, BrokenImportMatchesOnAPathBoundary)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} };\n");
    write_file(dir.path() / "neww.h", "int broken( ;\n");
    const std::string text =
        "using \"w.h\";\n"
        "using \"neww.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    void b() { n = n + 6; }\n"
        "}\n";
    const hotc::AnalyzeResult r = hotc::analyze(text, temp_options(dir, ""));
    EXPECT_NE(r.cpp.find("n + 6"), std::string::npos) << r.diagnostics;
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16));
}

// The import path has "." and ".." segments. They go before the compare.
TEST(Analyze, BrokenImportMatchesAfterNormalizingTheImportPath)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} };\n");
    std::filesystem::create_directories(dir.path() / "sub");
    write_file(dir.path() / "sub" / "bad.h", "int broken( ;\n");
    const std::string text =
        "using \"w.h\";\n"
        "using \"sub/../sub/./bad.h\";\n"
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    void b() { n = n + 6; }\n"
        "}\n";
    const hotc::AnalyzeResult r = hotc::analyze(text, temp_options(dir, ""));
    EXPECT_NE(r.cpp.find("n + 6"), std::string::npos) << r.diagnostics;
    EXPECT_TRUE(r.source_map.map_to_generated(5, 16));
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

TEST(AnalyzeImports, ReadsClassesStaticsFunctionsConstsVarsAndEnums)
{
    const hotc::ImportFacts f = hotc::analyze_imports(
        "using \"other.hot\";\n"
        "const int Limit = 3;\n"
        "int counter = 0;\n"
        "int Add(int a, float b) { return a; }\n"
        "private int Hidden() { return 1; }\n"
        "enum Color { Red, Green }\n"
        "private enum Secret { A }\n"
        "class Weapon : Base {\n"
        "    static const int MaxAmmo = 9;\n"
        "    static int made = 0;\n"
        "    int ammo = 1;\n"
        "    void fire(int n) { }\n"
        "    static Weapon Make(int k) { return Weapon(); }\n"
        "}\n"
        "private class Hid { }\n",
        "");
    ASSERT_EQ(f.imports.size(), 1u);
    EXPECT_EQ(f.imports[0], "other.hot");
    ASSERT_EQ(f.classes.size(), 1u);
    const hotc::ImportClass& c = f.classes[0];
    EXPECT_EQ(c.name, "Weapon");
    EXPECT_EQ(c.base, "Base");
    auto find = [&](const std::string& n) -> const hotc::ImportMember*
    {
        for (const hotc::ImportMember& m : c.members)
            if (m.name == n)
                return &m;
        return nullptr;
    };
    ASSERT_TRUE(find("MaxAmmo"));
    EXPECT_TRUE(find("MaxAmmo")->is_static && find("MaxAmmo")->is_const);
    ASSERT_TRUE(find("made"));
    EXPECT_TRUE(find("made")->is_static && !find("made")->is_const);
    ASSERT_TRUE(find("ammo"));
    EXPECT_FALSE(find("ammo")->is_static);
    ASSERT_TRUE(find("fire"));
    EXPECT_EQ(find("fire")->signature, "(int32_t n)");
    ASSERT_TRUE(find("Make"));
    EXPECT_TRUE(find("Make")->is_static);
    ASSERT_EQ(f.functions.size(), 1u);
    EXPECT_EQ(f.functions[0].name, "Add");
    ASSERT_EQ(f.vars.size(), 2u);
    EXPECT_EQ(f.vars[0].name, "Limit");
    EXPECT_TRUE(f.vars[0].is_const);
    ASSERT_EQ(f.enums.size(), 1u);
    EXPECT_EQ(f.enums[0].name, "Color");
    EXPECT_EQ(f.enums[0].enumerators.size(), 2u);
}

TEST(AnalyzeImports, ToleratesASyntaxError)
{
    const hotc::ImportFacts f = hotc::analyze_imports("class A { int x = ; void f() { } }\nclass B { }\n", "");
    ASSERT_GE(f.classes.size(), 1u);
    const hotc::ImportClass* b = nullptr;
    for (const hotc::ImportClass& c : f.classes)
        if (c.name == "B")
            b = &c;
    EXPECT_TRUE(b); // the class after the broken one
    bool has_f = false;
    for (const hotc::ImportClass& c : f.classes)
        if (c.name == "A")
            for (const hotc::ImportMember& m : c.members)
                has_f = has_f || (m.name == "f" && m.kind == "method");
    EXPECT_TRUE(has_f); // the member after the broken field
}

namespace
{

std::vector<hotc::VirtualMethod> virtuals_of(const std::string& text, const std::string& cls)
{
    TempDir dir;
    write_file(dir.path() / "w.h",
        "#include <string>\n"
        "class Widget { public: virtual ~Widget() {}\n"
        "  virtual void Update(float dt) {}\n"
        "  virtual std::string Describe(int level, bool full) const { return {}; }\n"
        "  virtual void Draw() = 0;\n"
        "  void Plain(int x) {}\n"
        "private: virtual void Hidden() {} };\n");
    hotc::TranslatorSession session;
    return hotc::virtual_methods(text, temp_options(dir, ""), session, cls);
}

const hotc::VirtualMethod* find_virtual(const std::vector<hotc::VirtualMethod>& v, const std::string& n)
{
    for (const hotc::VirtualMethod& m : v)
        if (m.name == n)
            return &m;
    return nullptr;
}

} // namespace

TEST(VirtualMethods, NativeBaseVirtualsAreListedWithParameterNames)
{
    const auto v = virtuals_of("using \"w.h\";\nclass Foo : Widget {\n    override \n}\n", "Foo");
    const hotc::VirtualMethod* u = find_virtual(v, "Update");
    ASSERT_TRUE(u);
    EXPECT_TRUE(u->native);
    EXPECT_EQ(u->ret_script, "void");
    EXPECT_EQ(u->signature, "(float dt)");
    const hotc::VirtualMethod* d = find_virtual(v, "Describe");
    ASSERT_TRUE(d);
    EXPECT_TRUE(d->is_const);
    EXPECT_EQ(d->param_names, (std::vector<std::string>{"level", "full"}));
    const hotc::VirtualMethod* w = find_virtual(v, "Draw");
    ASSERT_TRUE(w);
    EXPECT_TRUE(w->pure);
}

TEST(VirtualMethods, NonVirtualAndPrivateMethodsAreHidden)
{
    const auto v = virtuals_of("using \"w.h\";\nclass Foo : Widget {\n}\n", "Foo");
    EXPECT_FALSE(find_virtual(v, "Plain"));
    EXPECT_FALSE(find_virtual(v, "Hidden"));
}

TEST(VirtualMethods, ScriptParentVirtualIsListedAndOverriddenIsHidden)
{
    const auto v = virtuals_of(
        "using \"w.h\";\n"
        "class Mid : Widget {\n"
        "    virtual void Pulse(int n) { }\n"
        "    void Quiet() { }\n"
        "    override void Update(float dt) { }\n"
        "}\n"
        "class Leaf : Mid {\n"
        "    override void Draw() { }\n"
        "    override \n"
        "}\n",
        "Leaf");
    const hotc::VirtualMethod* p = find_virtual(v, "Pulse");
    ASSERT_TRUE(p);
    EXPECT_FALSE(p->native);
    EXPECT_EQ(p->owner, "Mid");
    EXPECT_EQ(p->signature, "(int n)");
    EXPECT_FALSE(find_virtual(v, "Quiet"));
    EXPECT_TRUE(find_virtual(v, "Update")); // Mid overrides it: Leaf can override it again
    EXPECT_FALSE(find_virtual(v, "Draw")); // Leaf overrides it already
}

TEST(VirtualMethods, TemplateBaseAndPartlyWrittenOverrideGiveTheList)
{
    TempDir dir;
    write_file(dir.path() / "t.h", "template <class T> class Box { public: virtual ~Box() {} virtual T Get(T fallback) { return fallback; } };\n");
    hotc::TranslatorSession session;
    const auto v = hotc::virtual_methods(
        "using \"t.h\";\nclass A : Box<int> {\n    override \n}\n", temp_options(dir, ""), session, "A");
    const hotc::VirtualMethod* g = find_virtual(v, "Get");
    ASSERT_TRUE(g);
    EXPECT_EQ(g->signature, "(int fallback)");
    EXPECT_EQ(g->ret_script, "int");
}

// An import with a syntax error in one class: the valid classes of the import stay usable. The
// refs of the importer have the names of the valid class, and the diagnostics are the error of
// the import.
namespace
{
const char* const kBrokenImportLib =
    "using \"w.h\";\n"
    "class Gun : Widget {\n"
    "    public int Fire(int b) { return b * 2; }\n"
    "}\n"
    "class Bad { public int F() { return 1 +; } }\n";
const char* const kBrokenImportMain =
    "using \"w.h\";\n"
    "using \"lib.hot\";\n"
    "class Pistol : Widget {\n"
    "    public int Shoot($Gun g) { if (g == null) return 0; return g.Fire(1); }\n"
    "}\n";

hotc::TranslateOptions broken_import_options(const TempDir& dir)
{
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    write_file(dir.path() / "lib.hot", kBrokenImportLib);
    hotc::TranslateOptions o = temp_options(dir, "");
    o.modules = {{"lib.hot", kBrokenImportLib}};
    o.import_cache = true;
    return o;
}
} // namespace

TEST(Analyze, BrokenImportKeepsTheRefsOfItsValidClasses)
{
    TempDir dir;
    hotc::TranslateOptions o = broken_import_options(dir);
    o.resolve_refs = true;
    hotc::TranslatorSession session;
    const hotc::AnalyzeResult r = hotc::analyze(kBrokenImportMain, o, session);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.passes, 2);
    EXPECT_EQ(r.diagnostics.rfind("lib.hot:5:", 0), 0u) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("error"), std::string::npos);
    bool fire = false;
    for (const hotc::ResolvedRef& x : r.refs)
        fire = fire || (x.decl == "Gun.Fire" && x.decl_file.find("lib.hot") != std::string::npos);
    EXPECT_TRUE(fire);
    // A strict translation of the same script, in the same session, still fails.
    hotc::TranslateOptions strict = broken_import_options(dir);
    strict.script_text = kBrokenImportMain;
    const hotc::TranslateResult t = hotc::translate(strict, session);
    EXPECT_FALSE(t.ok);
    EXPECT_NE(t.diagnostics.find("lib.hot:5:"), std::string::npos) << t.diagnostics;
    EXPECT_TRUE(t.import_diagnostics.empty());
    // A lenient translation reports the error of the import apart.
    hotc::TranslateOptions lenient = strict;
    lenient.lenient_imports = true;
    const hotc::TranslateResult l = hotc::translate(lenient, session);
    EXPECT_TRUE(l.ok) << l.diagnostics;
    EXPECT_NE(l.import_diagnostics.find("lib.hot:5:"), std::string::npos) << l.import_diagnostics;
    // The lenient hit from the cache gives the same error. The strict lookup stays strict.
    const hotc::TranslateResult l2 = hotc::translate(lenient, session);
    EXPECT_EQ(l2.import_diagnostics, l.import_diagnostics);
    EXPECT_FALSE(hotc::translate(strict, session).ok);
}

// An import with an overload that does not parse: the call of the overload must not get the ref of
// the other overload.
TEST(Analyze, BrokenImportOverloadGivesNoRefToTheOtherOverload)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    const std::string lib = "using \"w.h\";\n"
                            "class Gun : Widget {\n"
                            "    public int Fire(int b) { return b * 2; }\n"
                            "    public int Fire(float b +) { return 1; }\n"
                            "}\n";
    const std::string main = "using \"w.h\";\n"
                             "using \"lib.hot\";\n"
                             "class Pistol : Widget {\n"
                             "    public int Shoot($Gun g) { if (g == null) return 0; return g.Fire(1.5); }\n"
                             "}\n";
    write_file(dir.path() / "lib.hot", lib);
    hotc::TranslateOptions o = temp_options(dir, "");
    o.resolve_refs = true;
    o.modules = {{"lib.hot", lib}};
    const hotc::AnalyzeResult r = hotc::analyze(main, o);
    for (const hotc::ResolvedRef& x : r.refs)
        if (x.decl == "Gun.Fire")
            ADD_FAILURE() << "ref to Gun.Fire at " << x.pos.line << ":" << x.pos.col << " decl_file=" << x.decl_file;
}
