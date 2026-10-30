// Tests for hotc::translate with properties: auto accessors, custom accessors and errors.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

std::string prop_script(const std::string& members)
{
    return "using \"w.h\";\nclass Foo : Widget {\n" + members + "\n}\n";
}

hotc::TranslateResult translate_props(const std::string& members)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    return hotc::translate(temp_options(dir, prop_script(members)));
}

void expect_prop_error(const std::string& members, const std::string& msg)
{
    const hotc::TranslateResult r = translate_props(members);
    EXPECT_FALSE(r.ok) << members;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << members << "\n" << r.diagnostics;
}

} // namespace

TEST(PropertyTranslation, AutoPropertyLowersToAccessorCalls)
{
    const hotc::TranslateResult r = translate_props(
        "    int N { get; set; } = 3;\n"
        "    void run() { N = 1; this.N += 2; N++; int k = this.N + N; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("    int32_t __p_N;\n"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("new (f) T(3);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("static int32_t Foo__get_N(Foo_Shell* __self)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("static void Foo__set_N(Foo_Shell* __self, int32_t value)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return __st->__p_N;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->__p_N = value;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_N(__self, 1);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_N(__self, Foo__get_N(__self) + (2));"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_N(__self, Foo__get_N(__self) + 1);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("int32_t k = Foo__get_N(__self) + Foo__get_N(__self);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.slots_out.find("Foo::get_N 1"), std::string::npos) << r.slots_out;
    EXPECT_NE(r.slots_out.find("Foo::set_N 1"), std::string::npos) << r.slots_out;
}

TEST(PropertyTranslation, CustomAccessorsBecomeFunctions)
{
    const hotc::TranslateResult r = translate_props(
        "    int raw = 0;\n"
        "    int V { get { return this.raw * 2; } set { this.raw = value / 2; } }\n"
        "    int run(int value) { V = value; return V; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("__p_V"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return __st->raw * 2;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->raw = value / 2;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_V(__self, value);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return Foo__get_V(__self);"), std::string::npos) << r.cpp;
}

TEST(PropertyTranslation, LocalHidesProperty)
{
    const hotc::TranslateResult r = translate_props("    int N { get; set; }\n    void run() { int N = 2; N = 3; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("N = 3;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Foo__set_N(__self, 3)"), std::string::npos) << r.cpp;
}

// "w.N" on another object of the class calls the accessors through hot::shell_of.
TEST(PropertyTranslation, OtherObjectUsesAccessors)
{
    const hotc::TranslateResult r = translate_props(
        "    int N { get; set; }\n"
        "    int hp = 0;\n"
        "    int run(Widget& w, Widget* q) {\n"
        "        w.N = 1;\n"
        "        q.N += 2;\n"
        "        w.N++;\n"
        "        int a = this.N = q.N = 4;\n"
        "        int b = (w.N = 5);\n"
        "        return w.N + q.N;\n"
        "    }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string w = "hot::shell_of<Foo_Shell>(w, \"Foo\")";
    const std::string q = "hot::shell_of<Foo_Shell>(q, \"Foo\")";
    EXPECT_NE(r.cpp.find("Foo__set_N(" + w + ", 1);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("hot::prop_update<hot::op_add>(Foo__get_N, Foo__set_N, " + q + ", 2);"), std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("hot::prop_update<hot::op_add>(Foo__get_N, Foo__set_N, " + w + ", 1);"), std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("int32_t a = hot::prop_set(Foo__set_N, __self, hot::prop_set(Foo__set_N, " + q + ", 4));"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("int32_t b = (hot::prop_set(Foo__set_N, " + w + ", 5));"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return Foo__get_N(" + w + ") + Foo__get_N(" + q + ");"), std::string::npos) << r.cpp;
}

// A member that is not a property keeps the native member access.
TEST(PropertyTranslation, OtherObjectPlainMemberIsNotRewritten)
{
    const hotc::TranslateResult r = translate_props(
        "    int N { get; set; }\n"
        "    void run(Foo2& p) { p.hp = 1; p.hp += p.n; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("hot::deref(p).hp = 1;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("hot::deref(p).hp += hot::deref(p).n;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Foo__set_hp"), std::string::npos) << r.cpp;
}

TEST(PropertyTranslation, Errors)
{
    expect_prop_error("    int N { get; }\n    void run(Widget& w) { w.N = 1; }", "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { get; }\n    void run(Widget* w) { w.N++; }", "property 'N' has no 'set' accessor");
    expect_prop_error(
        "    int N { set; }\n    int run(Widget& w) { return w.N; }", "property 'N' has no 'get' accessor");
    expect_prop_error("    int N { get; }\n    void run() { N = 1; }", "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { get; }\n    void run() { this.N += 1; }", "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { set; }\n    int run() { return N; }", "property 'N' has no 'get' accessor");
    expect_prop_error("    int N { }", "a property needs a 'get' or a 'set' accessor");
    expect_prop_error("    int N { get; get; }", "duplicate 'get' accessor");
    expect_prop_error("    int N { get; set { } }", "a property cannot mix auto and custom accessors");
    expect_prop_error("    int N { get { return 1; } } = 2;", "only an auto property can have an initializer");
    expect_prop_error("    int N { get; fetch; }", "expected 'get' or 'set'");
    expect_prop_error("    int N;\n    int N { get; }", "'N' is a field and a property");
    expect_prop_error("    int N { get; }\n    void N() { }", "'N' is a method and a property");
    expect_prop_error("    int N { get; }\n    int N { set; }", "property 'N' is declared twice");
    expect_prop_error("    int N { get; }\n    int get_N() { return 1; }",
        "method 'get_N' collides with the 'get' accessor of property 'N'");
    expect_prop_error("    int N { get; set; }\n    int m = N;", "property 'N' is not allowed in a field initializer");
    expect_prop_error("    override int N { get; }", "'override' is only valid on a method");
    expect_prop_error("    int get;", "'get' is a reserved word");
    expect_prop_error("    void set() { }", "'set' is a reserved word");
    expect_prop_error("    void f(int get) { }", "'get' is a reserved word");
    expect_prop_error("    int get { get; }", "'get' is a reserved word");
}
