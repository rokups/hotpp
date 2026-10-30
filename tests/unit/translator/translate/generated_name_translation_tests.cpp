// Tests for the names of the generated code, the expression body of a lambda and the C style cast.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

hotc::TranslateResult translate_widget(const std::string& members)
{
    static TempDir dir;
    static bool header = false;
    if (!header)
    {
        write_file(dir.path() / "w.h", kHeaderV1);
        header = true;
    }
    return hotc::translate(
        temp_options(dir, "using \"w.h\";\nclass Foo : Widget {\n    int n = 0;\n" + members + "}\n"));
}

} // namespace

TEST(GeneratedNameTranslation, GeneratedNamesHaveTheDoubleUnderscorePrefix)
{
    const hotc::TranslateResult r = translate_widget("    void f(int v) { n = v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("Foo_Shell* __self"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("auto* __st = "), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__f__thunk(Widget* __a, int32_t v)"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("* self"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("auto* st "), std::string::npos) << r.cpp;
}

TEST(GeneratedNameTranslation, NamesOfTheOldGeneratedCodeAreLegal)
{
    const hotc::TranslateResult r = translate_widget(
        "    int f(int a, int st, int self) { int b = a + st + self; return b + this.n; }\n"
        "    Foo(int a) { this.n = a; }\n"
        "    void g() { int a = 1; int st = 2; int self = 3; this.n = a + st + self; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("int32_t a, int32_t st, int32_t self"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->n"), std::string::npos) << r.cpp;
}

TEST(GeneratedNameTranslation, DoubleUnderscoreNamesStayReserved)
{
    const hotc::TranslateResult r = translate_widget("    void f(int __a) { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("parameter name '__a' is reserved"), std::string::npos) << r.diagnostics;
    const hotc::TranslateResult c = translate_widget("    Foo(int __self) { }\n");
    EXPECT_FALSE(c.ok);
    EXPECT_NE(c.diagnostics.find("parameter name '__self' is reserved"), std::string::npos) << c.diagnostics;
}

TEST(GeneratedNameTranslation, LambdaExpressionBodyReturnsTheValue)
{
    const hotc::TranslateResult r = translate_widget(
        "    void f() {\n"
        "        var twice = (int x) => x * 2;\n"
        "        var alive = () => this.n > 0;\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](int32_t x)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return x * 2;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return __st->n > 0;"), std::string::npos) << r.cpp;
}

TEST(GeneratedNameTranslation, LambdaExpressionBodyAsAnArgument)
{
    const hotc::TranslateResult r = translate_widget(
        "    int apply(int v) { return v; }\n"
        "    void f() { this.apply(((int x) => x + 1)(2)); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("return x + 1;"), std::string::npos) << r.cpp;
}

TEST(GeneratedNameTranslation, CStyleCastOfABuiltinType)
{
    const hotc::TranslateResult r = translate_widget(
        "    void f(float v, void* q) {\n"
        "        int i = (int)v;\n"
        "        float w = (float)i * 2.0;\n"
        "        uint u = (uint)-i;\n"
        "        var p = (int*)q;\n"
        "        var s = (v * 2.0) + (w);\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("int32_t i = (int32_t)v;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("float w = (float)i * 2.0f;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("uint32_t u = (uint32_t)-i;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("auto p = (int32_t*)q;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("auto s = (v * 2.0f) + (w);"), std::string::npos) << r.cpp;
}

TEST(GeneratedNameTranslation, CastToANameWithoutAMarkIsAnError)
{
    const hotc::TranslateResult r = translate_widget("    void f(float v) { var i = (Foo)v; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("expected end of expression"), std::string::npos) << r.diagnostics;
}
