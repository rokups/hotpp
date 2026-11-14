// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the table of the resolved names (hotc::AnalyzeResult::refs). Each test finds the id of
// one declaration and then checks the exact set of the names that have this id.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace translator_test;

namespace
{

const char* kWidget = "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n";

hotc::AnalyzeResult analyze_refs(const std::string& text, const std::vector<std::pair<std::string, std::string>>& modules = {})
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    hotc::TranslateOptions o = temp_options(dir, "");
    o.resolve_refs = true;
    o.null_warnings = true; // the tests are about names, not about null checks
    o.modules = modules;
    return hotc::analyze(text, o);
}

bool ident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The offsets of the whole word "w" in "text".
std::vector<uint32_t> words(const std::string& text, const std::string& w)
{
    std::vector<uint32_t> out;
    for (size_t p = text.find(w); p != std::string::npos; p = text.find(w, p + 1))
        if ((p == 0 || !ident(text[p - 1])) && (p + w.size() >= text.size() || !ident(text[p + w.size()])))
            out.push_back(static_cast<uint32_t>(p));
    return out;
}

// The offsets of the occurrences "which" (0-based) of the word "w".
std::set<uint32_t> pick(const std::string& text, const std::string& w, std::initializer_list<size_t> which)
{
    const std::vector<uint32_t> all = words(text, w);
    std::set<uint32_t> out;
    for (size_t i : which)
    {
        EXPECT_LT(i, all.size()) << w;
        if (i < all.size())
            out.insert(all[i]);
    }
    return out;
}

std::set<uint32_t> all_of(const std::string& text, const std::string& w)
{
    const std::vector<uint32_t> all = words(text, w);
    return {all.begin(), all.end()};
}

const hotc::ResolvedRef* ref_at(const hotc::AnalyzeResult& r, uint32_t offset)
{
    for (const hotc::ResolvedRef& x : r.refs)
        if (x.offset == offset)
            return &x;
    return nullptr;
}

// The kind of the ref at "offset". Local when there is no ref: the test then fails on its set.
hotc::RefKind kind_at(const hotc::AnalyzeResult& r, uint32_t offset)
{
    const hotc::ResolvedRef* x = ref_at(r, offset);
    EXPECT_NE(x, nullptr) << offset;
    return x ? x->kind : hotc::RefKind::Local;
}

// The id of the name at the occurrence "nth" of the word "w".
std::string decl_of(const hotc::AnalyzeResult& r, const std::string& text, const std::string& w, size_t nth)
{
    const std::vector<uint32_t> all = words(text, w);
    if (nth >= all.size())
        return "<no word>";
    const hotc::ResolvedRef* x = ref_at(r, all[nth]);
    return x ? x->decl : "<no ref>";
}

std::set<uint32_t> uses_of(const hotc::AnalyzeResult& r, const std::string& decl)
{
    std::set<uint32_t> out;
    for (const hotc::ResolvedRef& x : r.refs)
        if (x.decl == decl)
            out.insert(x.offset);
    return out;
}

std::set<uint32_t> decls_of(const hotc::AnalyzeResult& r, const std::string& decl)
{
    std::set<uint32_t> out;
    for (const hotc::ResolvedRef& x : r.refs)
        if (x.decl == decl && x.is_decl)
            out.insert(x.offset);
    return out;
}

} // namespace

TEST(ResolvedRefs, NoTableWithoutTheOption)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    const hotc::AnalyzeResult r = hotc::analyze(
        "using \"w.h\";\nclass Foo : Widget { int n = 0; void Run() { n = n + 1; } }\n", temp_options(dir, ""));
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.refs.empty());
}

TEST(ResolvedRefs, LocalsWithTheSameNameHaveDifferentIds)
{
    const std::string text = "using \"w.h\";\n"
                             "class Foo : Widget {\n"
                             "    int x = 0;\n"
                             "    int Calc(int a) {\n"
                             "        { int t = a; a = t + 1; }\n"
                             "        { int t = a * 2; a = t; }\n"
                             "        int x = a;\n"
                             "        return x + this.x;\n"
                             "    }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const std::string t1 = decl_of(r, text, "t", 0);
    const std::string t2 = decl_of(r, text, "t", 2);
    EXPECT_EQ(t1, "@" + std::to_string(words(text, "t")[0]));
    EXPECT_NE(t1, t2);
    EXPECT_EQ(uses_of(r, t1), pick(text, "t", {0, 1}));
    EXPECT_EQ(uses_of(r, t2), pick(text, "t", {2, 3}));
    EXPECT_EQ(decls_of(r, t1), pick(text, "t", {0}));
    // The local "x" hides the field "x". "this.x" is the field.
    const std::string lx = decl_of(r, text, "x", 1);
    EXPECT_EQ(uses_of(r, lx), pick(text, "x", {1, 2}));
    EXPECT_EQ(uses_of(r, "Foo.x"), pick(text, "x", {0, 3}));
    EXPECT_EQ(decls_of(r, "Foo.x"), pick(text, "x", {0}));
}

TEST(ResolvedRefs, Parameters)
{
    const std::string text = "using \"w.h\";\n"
                             "class Foo : Widget {\n"
                             "    int Calc(int a, int b) { var f = (int a) => a + 1; return a + b + f(b); }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const std::string a = decl_of(r, text, "a", 0);
    EXPECT_EQ(a, "@" + std::to_string(words(text, "a")[0]));
    EXPECT_EQ(uses_of(r, a), pick(text, "a", {0, 3}));
    EXPECT_EQ(decls_of(r, a), pick(text, "a", {0}));
    // The lambda parameter "a" is another symbol.
    const std::string la = decl_of(r, text, "a", 1);
    EXPECT_NE(la, a);
    EXPECT_EQ(uses_of(r, la), pick(text, "a", {1, 2}));
    EXPECT_EQ(uses_of(r, decl_of(r, text, "b", 0)), all_of(text, "b"));
    EXPECT_EQ(kind_at(r, words(text, "b")[0]), hotc::RefKind::Param);
}

TEST(ResolvedRefs, StaticConstNextToAMultiplyIsAUse)
{
    const std::string text = "using \"w.h\";\n"
                             "class Foo : Widget {\n"
                             "    static const int Scale = 3;\n"
                             "    int Mul(int a) { int b = a * Scale; return b * Scale; }\n"
                             "    int Twice() { return Scale + Foo.Scale; }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(decl_of(r, text, "Scale", 1), "Foo.Scale");
    EXPECT_EQ(uses_of(r, "Foo.Scale"), all_of(text, "Scale"));
    EXPECT_EQ(decls_of(r, "Foo.Scale"), pick(text, "Scale", {0}));
    EXPECT_EQ(kind_at(r, words(text, "Scale")[2]), hotc::RefKind::Const);
    // "a * Scale" is a use, not the declaration of a local "Scale".
    ASSERT_NE(ref_at(r, words(text, "Scale")[1]), nullptr);
    EXPECT_FALSE(ref_at(r, words(text, "Scale")[1])->is_decl);
}

TEST(ResolvedRefs, EnumValues)
{
    const std::string text = "using \"w.h\";\n"
                             "enum Team { Red, Blue }\n"
                             "class Foo : Widget {\n"
                             "    Team side = Team.Red;\n"
                             "    int Score(Team t) {\n"
                             "        switch (t) { case Team.Red: return 1; case Team.Blue: return 2; }\n"
                             "        return t == Team.Red ? 3 : 4;\n"
                             "    }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "Team.Red"), all_of(text, "Red"));
    EXPECT_EQ(decls_of(r, "Team.Red"), pick(text, "Red", {0}));
    EXPECT_EQ(uses_of(r, "Team.Blue"), all_of(text, "Blue"));
    EXPECT_EQ(uses_of(r, "Team"), all_of(text, "Team"));
    EXPECT_EQ(kind_at(r, words(text, "Red")[1]), hotc::RefKind::EnumValue);
}

TEST(ResolvedRefs, MethodCallOnATypedVariable)
{
    const std::string text = "using \"w.h\";\n"
                             "class Gun : Widget {\n"
                             "    int ammo = 0;\n"
                             "    int Fire(int n) { ammo = ammo - n; return ammo; }\n"
                             "}\n"
                             "class Foo : Widget {\n"
                             "    $Gun g;\n"
                             "    int Run() {\n"
                             "        $Gun local = new $Gun();\n"
                             "        var v = new $Gun();\n"
                             "        return local.Fire(1) + v.Fire(2) + g.Fire(3) + this.g.ammo + Twice();\n"
                             "    }\n"
                             "    int Twice() { return 2; }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "Gun.Fire"), all_of(text, "Fire"));
    EXPECT_EQ(decls_of(r, "Gun.Fire"), pick(text, "Fire", {0}));
    EXPECT_EQ(kind_at(r, words(text, "Fire")[1]), hotc::RefKind::Method);
    EXPECT_EQ(uses_of(r, "Gun.ammo"), all_of(text, "ammo"));
    EXPECT_EQ(uses_of(r, "Foo.Twice"), all_of(text, "Twice"));
    EXPECT_EQ(uses_of(r, "Gun"), all_of(text, "Gun"));
}

TEST(ResolvedRefs, ImportedClassUse)
{
    const std::string text = "using \"w.h\";\n"
                             "using \"enemy.hot\";\n"
                             "class Foo : Widget {\n"
                             "    void Run($Enemy e) { e.Hit(1); int h = e.hp; $Enemy o = new $Enemy(); o.Hit(h); }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text,
        {{"enemy.hot", "using \"w.h\";\nclass Enemy : Widget { public int hp = 0; public void Hit(int d) { hp = hp - d; } }\n"}});
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "Enemy"), all_of(text, "Enemy"));
    EXPECT_EQ(decls_of(r, "Enemy"), std::set<uint32_t>{});
    EXPECT_EQ(uses_of(r, "Enemy.Hit"), all_of(text, "Hit"));
    EXPECT_EQ(uses_of(r, "Enemy.hp"), all_of(text, "hp"));
    const hotc::ResolvedRef* x = ref_at(r, words(text, "Hit")[0]);
    ASSERT_NE(x, nullptr);
    EXPECT_NE(x->decl_file.find("enemy.hot"), std::string::npos) << x->decl_file;
    EXPECT_EQ(kind_at(r, words(text, "Enemy")[0]), hotc::RefKind::Class);
}

TEST(ResolvedRefs, QualifiedStaticCall)
{
    const std::string text = "using \"w.h\";\n"
                             "class Util : Widget {\n"
                             "    static int Twice(int v) { return v * 2; }\n"
                             "    static int Four(int v) { return Twice(Twice(v)); }\n"
                             "}\n"
                             "class Foo : Widget {\n"
                             "    int Run() { return Util.Twice(3) + Util.Four(1); }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "Util.Twice"), all_of(text, "Twice"));
    EXPECT_EQ(kind_at(r, words(text, "Twice")[3]), hotc::RefKind::StaticMethod);
    EXPECT_EQ(uses_of(r, "Util"), all_of(text, "Util"));
    EXPECT_EQ(kind_at(r, words(text, "Util")[1]), hotc::RefKind::Class);
}

TEST(ResolvedRefs, ClassNameInTypePositions)
{
    const std::string text = "using \"w.h\";\n"
                             "class Gun : Widget {\n"
                             "    Gun() { }\n"
                             "    int n = 0;\n"
                             "}\n"
                             "class Rifle : Gun {\n"
                             "    $Gun spare;\n"
                             "    $Gun Make($Gun g) { return new $Gun(); }\n"
                             "    int Test(Widget w) {\n"
                             "        if (w is $Gun x) { return x.n; }\n"
                             "        $Gun y = w as $Gun;\n"
                             "        return 0;\n"
                             "    }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "Gun"), all_of(text, "Gun"));
    EXPECT_EQ(decls_of(r, "Gun"), pick(text, "Gun", {0}));
    EXPECT_EQ(uses_of(r, "Gun.n"), all_of(text, "n"));
}

TEST(ResolvedRefs, TheOptionDoesNotChangeTheGeneratedText)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    const std::string text = "using \"w.h\";\n"
                             "class Foo : Widget {\n"
                             "    static const int K = 2;\n"
                             "    int n = 0;\n"
                             "    int Run(int a) { int b = a * K; n = n + b; return n; }\n"
                             "}\n";
    hotc::TranslateOptions o = temp_options(dir, text);
    const hotc::TranslateResult plain = hotc::translate(o);
    o.resolve_refs = true;
    const hotc::TranslateResult with = hotc::translate(o);
    ASSERT_TRUE(plain.ok) << plain.diagnostics;
    ASSERT_TRUE(with.ok) << with.diagnostics;
    EXPECT_EQ(plain.cpp, with.cpp);
    EXPECT_TRUE(plain.refs.empty());
    EXPECT_FALSE(with.refs.empty());
}

// A rename over two files: the refs of the importer B and the declarations of A have equal ids.
TEST(ResolvedRefs, IdsAreEqualOverTwoUnits)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    const std::string a = "using \"w.h\";\n"
                          "enum Mode { Slow, Fast }\n"
                          "class Gun : Widget {\n"
                          "    public static const int Max = 4;\n"
                          "    public int Fire(int n) { return n; }\n"
                          "}\n";
    const std::string b = "using \"w.h\";\n"
                          "using \"a.hot\";\n"
                          "class User : Widget {\n"
                          "    int Run($Gun g) { Mode m = Mode.Fast; return g.Fire(Gun.Max); }\n"
                          "}\n";
    write_file(dir.path() / "a.hot", a);
    write_file(dir.path() / "b.hot", b);
    hotc::TranslateOptions o = temp_options(dir, "");
    o.resolve_refs = true;
    o.null_warnings = true;
    o.script_name = (dir.path() / "a.hot").string();
    const hotc::AnalyzeResult ra = hotc::analyze(a, o);
    o.script_name = (dir.path() / "b.hot").string();
    const hotc::AnalyzeResult rb = hotc::analyze(b, o);
    ASSERT_TRUE(ra.ok) << ra.diagnostics;
    ASSERT_TRUE(rb.ok) << rb.diagnostics;
    // The id of the declaration of "w" in A, and the id of the occurrence "nth" of "w" in B.
    auto decl_id = [&](const std::string& w)
    {
        const hotc::ResolvedRef* x = ref_at(ra, words(a, w)[0]);
        EXPECT_TRUE(x && x->is_decl) << w;
        return x ? x->id : std::string("<no ref>");
    };
    auto use_id = [&](const std::string& w, size_t nth)
    {
        const hotc::ResolvedRef* x = ref_at(rb, words(b, w)[nth]);
        EXPECT_TRUE(x && !x->is_decl) << w;
        return x ? x->id : std::string("<no ref>");
    };
    const std::string file = hotc::ref_file_id((dir.path() / "a.hot").string());
    EXPECT_EQ(decl_id("Gun"), file + "#Gun");
    EXPECT_EQ(use_id("Gun", 0), decl_id("Gun"));
    EXPECT_EQ(use_id("Gun", 1), decl_id("Gun"));
    EXPECT_EQ(use_id("Mode", 0), decl_id("Mode"));
    EXPECT_EQ(use_id("Fast", 0), decl_id("Fast"));
    EXPECT_EQ(use_id("Fast", 0), file + "#Mode.Fast");
    EXPECT_EQ(use_id("Fire", 0), decl_id("Fire"));
    EXPECT_EQ(use_id("Max", 0), decl_id("Max"));
    // A symbol of B has the path of B.
    const hotc::ResolvedRef* user = ref_at(rb, words(b, "User")[0]);
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(user->id, hotc::ref_file_id((dir.path() / "b.hot").string()) + "#User");
}

// An override, its root in a script base class and the calls through each type have one id.
TEST(ResolvedRefs, OverrideHasTheIdOfTheRoot)
{
    const std::string text = "using \"w.h\";\n"
                             "class Base : Widget {\n"
                             "    public virtual int Foo() { return 1; }\n"
                             "}\n"
                             "class Derived : Base {\n"
                             "    public override int Foo() { return 2; }\n"
                             "}\n"
                             "class User : Widget {\n"
                             "    int Run($Base b, $Derived d) { return b.Foo() + d.Foo(); }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "Base.Foo"), all_of(text, "Foo"));
    EXPECT_EQ(decls_of(r, "Base.Foo"), pick(text, "Foo", {0, 1}));
    EXPECT_EQ(uses_of(r, "Derived.Foo"), std::set<uint32_t>{});
}

// An interface method, its implementation and a call through the interface have one id.
TEST(ResolvedRefs, ImplementationHasTheIdOfTheInterfaceMethod)
{
    const std::string text = "using \"w.h\";\n"
                             "interface IShape {\n"
                             "    int Area();\n"
                             "}\n"
                             "class Sq : Widget, IShape {\n"
                             "    public int Area() { return 4; }\n"
                             "    int Go(IShape s, $Sq q) { return s.Area() + q.Area() + Area(); }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(uses_of(r, "IShape.Area"), all_of(text, "Area"));
    EXPECT_EQ(decls_of(r, "IShape.Area"), pick(text, "Area", {0, 1}));
    EXPECT_EQ(uses_of(r, "IShape"), all_of(text, "IShape"));
}

// The root ids are equal over two units: A declares the base class, B overrides and calls.
// A script interface of an import is not visible to the importer, so the test has no interface.
TEST(ResolvedRefs, RootIdsAreEqualOverTwoUnits)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    const std::string a = "using \"w.h\";\n"
                          "class Base : Widget {\n"
                          "    public virtual int Foo() { return 1; }\n"
                          "}\n";
    const std::string b = "using \"w.h\";\n"
                          "using \"a.hot\";\n"
                          "class Derived : Base {\n"
                          "    public override int Foo() { return 2; }\n"
                          "    int Go($Base p) { return p.Foo(); }\n"
                          "}\n";
    write_file(dir.path() / "a.hot", a);
    write_file(dir.path() / "b.hot", b);
    hotc::TranslateOptions o = temp_options(dir, "");
    o.resolve_refs = true;
    o.null_warnings = true;
    o.script_name = (dir.path() / "a.hot").string();
    const hotc::AnalyzeResult ra = hotc::analyze(a, o);
    o.script_name = (dir.path() / "b.hot").string();
    const hotc::AnalyzeResult rb = hotc::analyze(b, o);
    ASSERT_TRUE(ra.ok) << ra.diagnostics;
    ASSERT_TRUE(rb.ok) << rb.diagnostics;
    auto id_in = [](const hotc::AnalyzeResult& r, const std::string& text, const std::string& w, size_t nth)
    {
        const hotc::ResolvedRef* x = ref_at(r, words(text, w)[nth]);
        return x ? x->id : std::string("<no ref>");
    };
    const std::string file = hotc::ref_file_id((dir.path() / "a.hot").string());
    EXPECT_EQ(id_in(ra, a, "Foo", 0), file + "#Base.Foo");
    EXPECT_EQ(id_in(rb, b, "Foo", 0), file + "#Base.Foo");
    EXPECT_EQ(id_in(rb, b, "Foo", 1), file + "#Base.Foo");
}

// A member of a value with no known script type is in the list of the unresolved names.
TEST(ResolvedRefs, MemberOfANativeValueIsUnresolved)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    write_file(dir.path() / "stats.h", "struct Stats { int hp = 0; };\ninline Stats Native() { return {}; }\n");
    const std::string text = "using \"w.h\";\n"
                             "using \"stats.h\";\n"
                             "class Foo : Widget {\n"
                             "    int Get() { var x = Native(); return x.hp; }\n"
                             "}\n";
    hotc::TranslateOptions o = temp_options(dir, "");
    o.resolve_refs = true;
    o.null_warnings = true;
    const hotc::AnalyzeResult r = hotc::analyze(text, o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.unresolved.size(), 1u);
    EXPECT_EQ(r.unresolved[0].offset, words(text, "hp")[0]);
    EXPECT_EQ(r.unresolved[0].length, 2u);
    EXPECT_TRUE(r.raw_ranges.empty());
    o.resolve_refs = false;
    const hotc::AnalyzeResult off = hotc::analyze(text, o);
    EXPECT_TRUE(off.unresolved.empty());
    EXPECT_TRUE(off.raw_ranges.empty());
}

TEST(ResolvedRefs, MemberOfATypedReceiverIsResolved)
{
    const std::string text = "using \"w.h\";\n"
                             "class Gun : Widget {\n"
                             "    public int ammo = 0;\n"
                             "}\n"
                             "class Foo : Widget {\n"
                             "    int Get($Gun g) { return g.ammo; }\n"
                             "}\n";
    const hotc::AnalyzeResult r = analyze_refs(text);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.unresolved.empty());
    EXPECT_EQ(uses_of(r, "Gun.ammo"), all_of(text, "ammo"));
}

// The pass does not read the raw bodies of a ".cpp" script. The table has their ranges.
TEST(ResolvedRefs, RawBodyGivesARange)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWidget);
    const std::string text = "#include \"w.h\"\n\n"
                             "class A : public Widget\n{\npublic:\n    int n = 0;\n"
                             "    void Update(float dt) override { n = 1; }\n};\n";
    hotc::TranslateOptions o = temp_options(dir, text);
    o.script_name = "t.cpp";
    o.resolve_refs = true;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The initializer of "n" and the body of Update.
    ASSERT_EQ(r.raw_ranges.size(), 2u);
    EXPECT_EQ(r.raw_ranges[0].first, text.find("= 0;") + 1);
    const size_t open = text.find("{ n = 1; }");
    EXPECT_EQ(r.raw_ranges[1].first, open + 1);
    EXPECT_EQ(r.raw_ranges[1].second, text.find('}', open));
}
