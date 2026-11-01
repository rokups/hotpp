// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with three conveniences: "=>" bodies, default values and "??".
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace check_translate_test;

namespace
{
// The lines of the text that have the part.
std::string lines_with(const std::string& text, const std::string& part)
{
    std::istringstream in(text);
    std::string out, line;
    while (std::getline(in, line))
        if (line.find(part) != std::string::npos)
            out += line + "\n";
    return out;
}
} // namespace

TEST(ExpressionBodyTranslation, MethodsAndReadOnlyProperty)
{
    const hotc::TranslateResult r = check_translate("",
        "    int count;\n"
        "    int Get() => count;\n"
        "    void Bump() => this.count += 1;\n"
        "    int Power => count * 2;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return count;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__st->count += 1;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return count * 2;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__get_Power")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "set_Power")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "=>")) << r.cpp;
}

TEST(ExpressionBodyTranslation, OverrideWithVoidExpression)
{
    const hotc::TranslateResult r = check_translate(
        "", "    int n;\n    void Move(float dt) { n += 1; }\n    override void Update(float dt) => Move(dt);\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__Move(__self, dt);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "return Foo__Move(__self, dt);")) << r.cpp;
}

TEST(DefaultValueTranslation, DefaultIsOnTheFirstDeclarationOnly)
{
    const hotc::TranslateResult r = check_translate("",
        "    int n;\n"
        "    void Heal(float amount = 1.0, int times = 2 * 3) { n += 1; }\n"
        "    void Run() { Heal(); Heal(2.0); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Heal(Foo_Shell* __self, float amount = 1.0f, int32_t times = 2 * 3);"))
        << r.cpp;
    // The slot, the thunk and the definition have no default text.
    EXPECT_TRUE(has(r.cpp, "static void Foo__Heal(Foo_Shell* __self, float amount, int32_t times)\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Heal__thunk(Widget* __a, float amount, int32_t times)")) << r.cpp;
    for (const char* part : {"__thunk", "hot::call<", "FooState_g1::Heal", "void Heal(", "add_thunk"})
        EXPECT_FALSE(has(lines_with(r.cpp, part), " = ")) << part << "\n" << lines_with(r.cpp, part);
    // A call without the argument is a call with the default of the declaration.
    EXPECT_TRUE(has(r.cpp, "Foo__Heal(__self); Foo__Heal(__self, 2.0f);")) << r.cpp;
}

TEST(DefaultValueTranslation, SlotKeyDoesNotChangeWithTheDefault)
{
    const hotc::TranslateResult a = check_translate("", "    void Heal(float amount = 1.0) { }\n");
    const hotc::TranslateResult b = check_translate("", "    void Heal(float amount = 5.0) { }\n");
    const hotc::TranslateResult c = check_translate("", "    void Heal(float amount) { }\n");
    ASSERT_TRUE(a.ok && b.ok && c.ok) << a.diagnostics << b.diagnostics << c.diagnostics;
    for (const char* part : {"add_thunk", "hot::call<", "reinterpret_cast<hot::SlotFn>", "typeid"})
    {
        EXPECT_EQ(lines_with(a.cpp, part), lines_with(c.cpp, part)) << part;
        EXPECT_EQ(lines_with(b.cpp, part), lines_with(c.cpp, part)) << part;
    }
}

TEST(DefaultValueTranslation, ConstructorMayHaveDefaults)
{
    const hotc::TranslateResult r = check_translate("",
        "    int n;\n    Foo(int start = 3) { n = start; }\n    void Make() { var f = new Foo(); var g = new "
        "Foo(7); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__new(int32_t start = 3)")) << r.cpp;
    // The factory of the engine uses the default value too.
    EXPECT_TRUE(has(r.cpp, "Foo__build(false, 3)")) << r.cpp;
}

TEST(DefaultValueTranslation, VirtualAndOverrideCannotHaveDefaults)
{
    const hotc::TranslateResult v = check_translate("", "    virtual void F(int a = 1) { }\n");
    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(has(v.diagnostics, "default values are not allowed on virtual or override methods")) << v.diagnostics;
    const hotc::TranslateResult o = check_translate("", "    override void Update(float dt = 1.0) { }\n");
    EXPECT_FALSE(o.ok);
    EXPECT_TRUE(has(o.diagnostics, "default values are not allowed on virtual or override methods")) << o.diagnostics;
}

TEST(DefaultValueTranslation, StructMethodAndConstructor)
{
    const hotc::TranslateResult r = check_translate(
        "struct V { int x; V(int q, int a = 5) { this.x = q + a; } void Add(int d = 2) { this.x += d; } }\n", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void Add(int32_t d = 2) {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "V(int32_t q, int32_t a = 5)")) << r.cpp;
    const hotc::TranslateResult bad = check_translate("struct V { int x; V(int a = 5) { this.x = a; } }\n", "");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "default value for each parameter")) << bad.diagnostics;
}

TEST(CoalesceTranslation, BecomesARuntimeHelperCall)
{
    const hotc::TranslateResult r = check_translate("",
        "    Foo Pick(Foo a, Foo b) { return a ?? b; }\n    int Num(std.shared_ptr<int> p, int d) { return *(p ?? p) + "
        "d; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return hot::coalesce(a, [&]() -> auto { return (b); });")) << r.cpp;
}

TEST(CoalesceTranslation, ChainIsRightAssociative)
{
    const hotc::TranslateResult r = check_translate("", "    Foo Pick(Foo a, Foo b, Foo c) => a ?? b ?? c;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(
        has(r.cpp, "hot::coalesce(a, [&]() -> auto { return (hot::coalesce(b, [&]() -> auto { return (c); })); })"))
        << r.cpp;
}

TEST(CoalesceTranslation, BorrowedPointerOnEitherSideIsBorrowed)
{
    const hotc::TranslateResult l = check_translate("",
        "    std.function<int()> cb;\n"
        "    void Run() { int x = 1; var p = &x; var n = (int*)null; var q = (int*)null;\n"
        "        q = p ?? n; this.cb = () => *q; }\n");
    EXPECT_FALSE(l.ok);
    EXPECT_TRUE(has(l.diagnostics, "the lambda captures 'q'")) << l.diagnostics;
    const hotc::TranslateResult rr = check_translate("",
        "    std.function<int()> cb;\n"
        "    void Run() { int x = 1; var p = &x; var n = (int*)null; var q = (int*)null;\n"
        "        q = n ?? p; this.cb = () => *q; }\n");
    EXPECT_FALSE(rr.ok);
    EXPECT_TRUE(has(rr.diagnostics, "the lambda captures 'q'")) << rr.diagnostics;
}

TEST(IsBindingTranslation, IfAndWhileBindAHandle)
{
    const hotc::TranslateResult r = check_translate("",
        "    int rage;\n    void Rage() { rage += 1; }\n"
        "    void Run(Foo x) {\n"
        "        if (x is Foo g) { g.Rage(); }\n"
        "        if (x is Foo h && h.rage > 1) { h.Rage(); } else if (x is Foo k) { k.Rage(); }\n"
        "        while (x is Foo w && w.rage < 5) { w.Rage(); }\n"
        "        bool plain = x is Foo;\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ HOT_TRACE_LINE(); auto g = hot::as_script<Foo_Shell>(x, \"Foo\"); if (g) {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto h = hot::as_script<Foo_Shell>(x, \"Foo\"); if (h && ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "else { HOT_TRACE_LINE(); auto k = hot::as_script<Foo_Shell>(x, \"Foo\"); if (k) {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (;;) { HOT_TRACE_LINE(); auto w = hot::as_script<Foo_Shell>(x, \"Foo\"); if (!(w && ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool plain = hot::is_script(x, \"Foo\");")) << r.cpp;
}

TEST(IsBindingTranslation, BindingOutsideTheStartOfAConditionIsAnError)
{
    const char* bad[] = {
        "if (a || x is Foo g) { }",
        "if (!(x is Foo g)) { }",
        "if (a && x is Foo g) { }",
        "if ((x is Foo g)) { }",
        "for (; x is Foo g; ) { }",
        "bool b = x is Foo g;",
        "do { } while (x is Foo g);",
        "if (x is Foo g || a) { }",
    };
    for (const char* stmt : bad)
    {
        const hotc::TranslateResult r =
            check_translate("", std::string("    void Run(Foo x, bool a) { ") + stmt + " }\n");
        EXPECT_FALSE(r.ok) << stmt;
        EXPECT_TRUE(
            has(r.diagnostics, "a 'is T name' binding is allowed only at the start of an if or while condition"))
            << stmt << "\n"
            << r.diagnostics;
    }
}

TEST(DefaultValueTranslation, FieldParameterAndThisAreErrors)
{
    const char* kMsg = "a default value cannot use a field, a parameter or 'this'";
    for (const char* body : {"    int count;\n    int Add(int a, int b = count) { return a + b; }\n",
             "    int Add2(int a, int c = a) { return a + c; }\n",
             "    int count;\n    int Add3(int b = this.count) { return b; }\n",
             "    int Num() { return 1; }\n    int Add4(int b = Num()) { return b; }\n",
             "    int P { get; set; }\n    int Add5(int b = P) { return b; }\n"})
    {
        const hotc::TranslateResult r = check_translate("", body);
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, kMsg)) << body << r.diagnostics;
    }
}

// A member of the native base is a member of the shell: a default value cannot use it.
TEST(DefaultValueTranslation, NativeBaseMemberIsAnError)
{
    const char* kMsg = "a default value cannot use a field, a parameter or 'this'";
    translator_test::TempDir dir;
    translator_test::write_file(dir.path() / "nb.h",
        "#include <string>\n"
        "class NBase { public: virtual ~NBase() {} virtual void Update(float) {}\n"
        "    static constexpr int kMax = 7; std::string Name; };\n");
    for (const char* body :
        {"    int Add(int a, int b = kMax) { return a + b; }\n", "    void Show(std.string s = Name) { }\n"})
    {
        const hotc::TranslateResult r = hotc::translate(
            translator_test::temp_options(dir, std::string("using \"nb.h\";\nclass Foo : NBase {\n") + body + "}\n"));
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, kMsg)) << body << r.diagnostics;
    }
    // A literal and an own name stay allowed.
    const hotc::TranslateResult ok = hotc::translate(translator_test::temp_options(
        dir, "using \"nb.h\";\nclass Foo : NBase {\n    int Add(int a, int b = 3) { return a + b + kMax; }\n}\n"));
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
}

TEST(DefaultValueTranslation, LiteralsOperatorsAndNativeNamesStayAllowed)
{
    const hotc::TranslateResult r = check_translate("",
        "    int Add(int a, int b = 2 * 3 + 1, float f = 1.5, int m = std.numeric_limits<int>.max()) { return a + b; "
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t b = 2 * 3 + 1")) << r.cpp;
}

TEST(DefaultValueTranslation, CoalesceInADefaultHasNoCapture)
{
    const hotc::TranslateResult r = check_translate("", "    int Add(int a, int b = 1 ?? 2) { return a + b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "[]() -> auto { return (2); }")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "b = hot::coalesce(1, [&]")) << r.cpp;
}
