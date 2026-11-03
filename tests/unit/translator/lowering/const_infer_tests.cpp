// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the const inference: which locals and parameters get "const", the two errors for a
// written "const", and the casts that keep "const".
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using translator_test::translate_text;

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A script class Weapon and a class Hero with the given members.
std::string hero(const std::string& members)
{
    return "using <vector>;\nclass Weapon {\n    int dmg = 1;\n}\nclass Hero {\n    $Weapon wp;\n    int n = 0;\n"
        + members + "}\n";
}

hotc::TranslateResult run(const std::string& members)
{
    return translate_text(hero(members));
}

} // namespace

TEST(ConstInfer, VarThatIsNeverAssignedIsConstAuto)
{
    const hotc::TranslateResult r = run("    int Run() { var x = 3; return x; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const auto x = 3;")) << r.cpp;
}

TEST(ConstInfer, TypedLocalThatIsNeverAssignedIsConstT)
{
    const hotc::TranslateResult r = run("    int Run() { int x = 3; return x; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const int32_t x = 3;")) << r.cpp;
}

TEST(ConstInfer, HandleLocalIsConstSharedPtr)
{
    const hotc::TranslateResult r = run("    void Run() { $Weapon w = new $Weapon(); w.dmg = 2; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const std::shared_ptr<Weapon> w = ")) << r.cpp;
}

TEST(ConstInfer, AssignmentMakesTheLocalWritable)
{
    const hotc::TranslateResult r = run("    int Run() { int a = 1; a = 2; int b = 1; b += 2; return a + b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t a = 1;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const int32_t a")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const int32_t b")) << r.cpp;
}

TEST(ConstInfer, IncrementMakesTheLocalWritable)
{
    const hotc::TranslateResult r = run("    int Run() { int a = 1; a++; return a; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "const int32_t a")) << r.cpp;
}

TEST(ConstInfer, MethodCallOnTheLocalMakesItWritable)
{
    const hotc::TranslateResult r =
        run("    void Run() { std.vector<int> v = std.vector<int>(); v.push_back(1); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "const \x01std::vector")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::vector<int32_t> v")) << r.cpp;
}

TEST(ConstInfer, RefLocalIsNeverConst)
{
    const hotc::TranslateResult r = run("    int Run() { ref r = this.n; return r; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto& r = ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const auto& r")) << r.cpp;
}

TEST(ConstInfer, LocalWithoutInitializerIsNotConst)
{
    const hotc::TranslateResult r = run("    int Run() { int x; return x; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "const int32_t x")) << r.cpp;
}

TEST(ConstInfer, ParameterThatIsNeverWrittenIsConstInTheBody)
{
    const hotc::TranslateResult r = run("    int Run(int a, int b) { b = 2; return a + b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The signature does not change. The body form of "a" is const.
    EXPECT_TRUE(has(r.cpp, "static int32_t Hero__Run(Hero_Shell* __self, int32_t a, int32_t b);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t HeroState_g1::Run(int32_t const a, int32_t b)")) << r.cpp;
}

TEST(ConstInfer, VirtualMethodParameterKeepsTheBaseForm)
{
    const hotc::TranslateResult r = translate_text(
        "class Base {\n    virtual int F(int a) { return a; }\n}\n"
        "class Derived : Base {\n    override int F(int a) { return a + 1; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "int32_t const a")) << r.cpp;
}

TEST(ConstInfer, MethodAndFieldAreNeverConst)
{
    const hotc::TranslateResult r = run("    int Get() { return this.n; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "Get() const")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const int32_t n;")) << r.cpp;
}

TEST(ConstInfer, ForeachRefThatIsNotWrittenIsConstRef)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n    int n = 0;\n"
        "    void Run() { foreach (ref x in this.xs) { this.n = x; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (const auto& x : ")) << r.cpp;
}

TEST(ConstInfer, WrittenConstOnALocalIsAnError)
{
    for (const char* body : {"const int x = 1;", "const var x = 1;", "const ref x = this.n;"})
    {
        const hotc::TranslateResult r = run(std::string("    void Run() { ") + body + " }\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "const is inferred; remove it") || has(r.diagnostics, "after 'const'"))
            << body << ": " << r.diagnostics;
    }
    const hotc::TranslateResult r = run("    void Run() { const int x = 1; }\n");
    EXPECT_TRUE(has(r.diagnostics, "const is inferred; remove it")) << r.diagnostics;
}

TEST(ConstInfer, WrittenConstOnAMethodOrAFieldIsAnError)
{
    const hotc::TranslateResult m = run("    int Get() const { return 1; }\n");
    EXPECT_FALSE(m.ok);
    EXPECT_TRUE(has(m.diagnostics, "const is inferred; remove it")) << m.diagnostics;
    const hotc::TranslateResult f = translate_text("class A {\n    const int k = 1;\n}\n");
    EXPECT_FALSE(f.ok);
    EXPECT_TRUE(has(f.diagnostics, "const is inferred; remove it")) << f.diagnostics;
}

TEST(ConstInfer, CastWithConstStaysLegal)
{
    const hotc::TranslateResult r = run("    void Run(int* p) { var q = (const int*)p; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const auto q = (const int32_t*)p;")) << r.cpp;
}

// The stream operators change the operands: "s << x" the left one, "s >> x" both.
TEST(ConstInfer, StreamOperatorsMakeTheOperandsWritable)
{
    const hotc::TranslateResult r = translate_text(
        "using <sstream>;\nclass Hero {\n"
        "    int Run() { std.ostringstream ss; ss << 1; std.istringstream is; int v = 0; is >> v; return v; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "const std::ostringstream ss")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::istringstream is")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const int32_t v")) << r.cpp;
}

TEST(ConstInfer, ShiftOfANumberKeepsTheConst)
{
    const hotc::TranslateResult r = run("    int Run() { int a = 1; int b = a << 2; return b >> 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const int32_t a = 1;")) << r.cpp;
}

// A script array "int[3]" is a std::array: it does not decay to "int*", so the callee cannot write it
// through the pointer. The C++ compiler rejects the call. The array is never const here.
TEST(ConstInfer, ArrayDoesNotDecayToAPointerParameter)
{
    const hotc::TranslateResult r = run(
        "    void Fill(int* p) { if (p == null) return; p[0] = 1; }\n"
        "    int Run() { int[3] a; Fill(a); return a[0]; }\n");
    EXPECT_TRUE(has(r.cpp, "std::array<int32_t, 3> a = ")) << r.cpp << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "const std::array<int32_t, 3> a")) << r.cpp << r.diagnostics;
}
