// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the arrow lambda.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

TEST(LambdaTranslation, ArrowLambdaBecomesABoxLambdaThatCapturesByValue)
{
    const hotc::TranslateOptions o = engine_options("timer_v1.hot", 1);
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The body lambda has no capture. The capture tuple holds "__self".
    EXPECT_NE(r.cpp.find("hot::make_lambda_deduced<__LamTag_0>(GEN, hot::caps(__self), [](auto& __c) "
                         "{ [[maybe_unused]] auto& __self = std::get<0>(__c); {"),
        std::string::npos)
        << r.cpp;
    // The tag struct is in the line of the live flag, so the line numbers do not move.
    EXPECT_NE(r.cpp.find("static uint32_t hot_live = 0; namespace { struct __LamTag_0 { "
                         "static constexpr const char* key = \"timer_v1:Beeper::arm/#0\"; "
                         "static constexpr const char* what = \"Beeper::arm lambda\"; "
                         "static constexpr const char* names[] = {\"__self\", nullptr}; "
                         "static constexpr const char* aliases[] = {nullptr}; "
                         "static const uint32_t* live() { return &hot_live; } }; }\n"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("hot::flush_lambdas(reg);"), std::string::npos) << r.cpp;
    // The body uses self and a new state pointer, not the pointer of the outer function.
    EXPECT_NE(r.cpp.find("auto* __st = state_of(__self);\n#line 15"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->fired = __st->fired + 1;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("=>"), std::string::npos);
}

TEST(LambdaTranslation, ParametersGetTypesAndPointersUseArrow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(
        temp_options(dir, lambda_script("        var f = (float x, int* q) => { if (q == null) return; q.foo(); this.n = x; };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("hot::caps(__self), [](auto& __c, float x, int32_t* q)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("hot::deref(q).foo();"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("__st->n = x;"), std::string::npos) << r.cpp;
}

TEST(LambdaTranslation, ParameterWithoutTypeIsAuto)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r =
        hotc::translate(temp_options(dir, lambda_script("        var f = (x, int y) => { };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The lambda has no signature of its own: the deferred form waits for the target type.
    EXPECT_NE(r.cpp.find("hot::make_lambda_deferred<__LamTag_0>(GEN, hot::caps(__self), [](auto& __c, auto x, int32_t y)"),
        std::string::npos)
        << r.cpp;
}

// A static method has no "__self". Its lambda has no state pointer.
TEST(LambdaTranslation, LambdaInAStaticMethodHasNoSelf)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(temp_options(dir,
        "using \"w.h\";\nclass Foo : Widget {\n    int n = 0;\n"
        "    static int run(int k) {\n        var f = () => { return k + 1; };\n        return f();\n    }\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("Foo.run lambda"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("state_of(__self)"), std::string::npos) << r.cpp;
}

// The key and the alias keys of the lambda go into the tag struct of the lambda.
TEST(LambdaTranslation, TagHoldsTheKeyAndTheAliasKeys)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(temp_options(dir,
        lambda_script("        var f = [name(\"tick\"), alias(\"old\"), alias(\"older\")] () => { this.n = 1; };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("key = \"" + std::string("t:Foo::run/n:tick") + "\";"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("aliases[] = {\"t:Foo::run/n:old\", \"t:Foo::run/n:older\", nullptr};"), std::string::npos)
        << r.cpp;
}

// The capture tuple holds the outer names that the body uses, in a fixed order: "__self" first.
// An inner lambda is a capture of the outer one through its own tuple.
TEST(LambdaTranslation, NestedLambdaCapturesThroughTheOuterTuple)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(temp_options(dir,
        lambda_script("        int k = 3;\n"
                      "        var add = (int x) => x + k;\n"
                      "        var outer = (int y) => { var g = (int z) => add(z) + y; return g(1); };\n"
                      "        this.n = outer(2);")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("hot::make_lambda_deduced<__LamTag_0>(GEN, hot::caps(__self, k), [](auto& __c, int32_t x)"),
        std::string::npos)
        << r.cpp;
    // The inner lambda is lowered first. It captures the closure "add" and the parameter "y". A
    // call of a captured closure counts as a write, so "add" is a shared cell.
    EXPECT_NE(r.cpp.find("hot::make_lambda_deduced<__LamTag_1>(GEN, hot::caps(__self, __cell_add, y), [](auto& __c, int32_t z)"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("hot::make_lambda_deduced<__LamTag_2>(GEN, hot::caps(__self, __cell_add), [](auto& __c, int32_t y)"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("names[] = {\"__self\", \"__cell_add\", \"y\", nullptr};"), std::string::npos) << r.cpp;
}

// A lambda that writes a local captures the shared cell, not the value.
TEST(LambdaTranslation, WrittenLocalIsACellCapture)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(
        temp_options(dir, lambda_script("        int c = 0;\n        var inc = () => { c += 1; };\n        inc();\n        this.n = c;")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("hot::caps(__self, __cell_c), [](auto& __c) { [[maybe_unused]] auto& __self = std::get<0>(__c); "
                         "[[maybe_unused]] auto& __cell_c = std::get<1>(__c); { HOT_NODEBUG_LOCAL auto* __st = "
                         "state_of(__self); auto& c = *__cell_c;"),
        std::string::npos)
        << r.cpp;
}

// A lambda of a static method has no "__self". With no capture the tuple is empty.
TEST(LambdaTranslation, LambdaWithoutCapturesHasAnEmptyTuple)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(temp_options(dir,
        "using \"w.h\";\nclass Foo : Widget {\n    int n = 0;\n"
        "    static int run(int k) {\n        var f = (int x) => { return x + 1; };\n        return f(k);\n    }\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("hot::make_lambda_deduced<__LamTag_0>(GEN, hot::caps(), [](auto& __c, int32_t x) { return x + 1; })"),
        std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("names[] = {nullptr};"), std::string::npos) << r.cpp;
}
