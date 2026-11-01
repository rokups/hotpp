// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the checks of "async" and "await" in a .hot script.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

// Translates the script text with the given C++ standard.
hotc::TranslateResult translate_std(const std::string& script, const std::string& std_version)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = script;
    o.generation = 1;
    o.std_version = std_version;
    return hotc::translate(o);
}

bool has(const hotc::TranslateResult& r, const std::string& text)
{
    return r.diagnostics.find(text) != std::string::npos;
}

} // namespace

// A valid script of C++20: an async method with await, a task local and "var x = await t;".
TEST(AsyncDiagnostics, ValidScriptHasNoErrors)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int h() { return 2; }\n"
        "    async int f(int a) {\n"
        "        var t = this.h();\n"
        "        var x = await t;\n"
        "        return x + a;\n"
        "    }\n"
        "    async void g() { await this.f(1); }\n"
        "}\n",
        "c++20");
    EXPECT_EQ(count_errors(r.diagnostics), 0) << r.diagnostics;
}

TEST(AsyncDiagnostics, AsyncNeedsCpp20)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int f() { return 1; }\n"
        "    async int g() { return 2; }\n"
        "}\n",
        "c++17");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'async' needs C++20: set CMAKE_CXX_STANDARD 20 before hot_add_scripts")) << r.diagnostics;
    EXPECT_EQ(count_errors(r.diagnostics), 2) << r.diagnostics; // one for each async method
}

TEST(AsyncDiagnostics, AwaitOutsideAsyncMethodIsAnError)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int t() { return 1; }\n"
        "    int f() { return await this.t(); }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'await' is only valid inside an 'async' method")) << r.diagnostics;
}

TEST(AsyncDiagnostics, AwaitInLambdaIsAnError)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int t() { return 1; }\n"
        "    async int f() {\n"
        "        var g = () => await this.t();\n"
        "        return 1;\n"
        "    }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'await' is only valid inside an 'async' method (a lambda cannot await yet)")) << r.diagnostics;
}

TEST(AsyncDiagnostics, AsyncMethodCannotTakeReferenceOrPointer)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int f(int& a) { return a; }\n"
        "    async int g(int* b) { return 1; }\n"
        "    async int h(int c) { return c; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'async' method 'f' cannot take parameter 'a' by reference: the caller's object can end "
                       "before the task continues"))
        << r.diagnostics;
    EXPECT_TRUE(has(r, "'async' method 'g' cannot take parameter 'b' by reference")) << r.diagnostics;
    EXPECT_FALSE(has(r, "'async' method 'h'")) << r.diagnostics;
}

TEST(AsyncDiagnostics, AsyncMethodDoesNotReturnATask)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async Task<int> f() { return 1; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "write the value type, 'async' makes the task")) << r.diagnostics;
}

TEST(AsyncDiagnostics, AsyncVoidMethodCannotReturnAValue)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async void f() { return 1; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "an 'async void' method cannot return a value")) << r.diagnostics;
}

TEST(AsyncDiagnostics, AsyncOverrideMustMatchTheBaseMethod)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    virtual int f() { return 1; }\n"
        "    virtual async int g() { return 1; }\n"
        "}\n"
        "class B : A {\n"
        "    override async int f() { return 2; }\n"
        "    override int g() { return 2; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'async' of 'f' differs from the base method")) << r.diagnostics;
}

TEST(AsyncDiagnostics, AsyncOnAStructMethodIsAnError)
{
    const hotc::TranslateResult r = translate_std(
        "struct S {\n"
        "    async int f() { return 1; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'async' is only valid on a class method")) << r.diagnostics;
}

TEST(AsyncDiagnostics, NonAsyncOverrideOfAnAsyncBaseIsAnError)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    virtual async int g() { return 1; }\n"
        "}\n"
        "class B : A {\n"
        "    override int g() { return 2; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r, "'async' of 'g' differs from the base method")) << r.diagnostics;
}

// A reload copies the fields, and a task is move-only. A class and a struct cannot hold a task.
TEST(AsyncDiagnostics, TaskFieldIsAnError)
{
    const char* msg = "a 'Task' field is not supported: hold the task in a local variable or in a host object";
    const hotc::TranslateResult c = translate_std(
        "class A {\n"
        "    Task<int> t;\n"
        "    async int f() { return 1; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(c.ok);
    EXPECT_TRUE(has(c, msg)) << c.diagnostics;
    EXPECT_NE(c.diagnostics.find("t.hot:2"), std::string::npos) << c.diagnostics;

    const hotc::TranslateResult v = translate_std(
        "struct S {\n"
        "    int a = 0;\n"
        "    hot.Task<void> t;\n"
        "}\n"
        "class A {\n"
        "    int f() { return 1; }\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(has(v, msg)) << v.diagnostics;

    const hotc::TranslateResult n = translate_std(
        "class A {\n"
        "    List<Task<int>> ts;\n"
        "}\n",
        "c++20");
    EXPECT_FALSE(n.ok);
    EXPECT_TRUE(has(n, msg)) << n.diagnostics;
}

// The translator decides from the lowered statements that a body is a coroutine, not from the
// text. A string literal with "co_await" does not count. The body only throws, so the
// translator adds a co_await that does not suspend: the throw goes into the task.
TEST(AsyncTranslation, BodyThatOnlyThrowsBecomesACoroutine)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int f() {\n"
        "        throw std.runtime_error(\"co_await co_return\");\n"
        "    }\n"
        "}\n",
        "c++20");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("co_await std::suspend_never{};"), std::string::npos) << r.cpp;
}

// A body with an await is a coroutine already: no extra co_await.
TEST(AsyncTranslation, BodyWithAwaitGetsNoExtraCoAwait)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    async int g() { return 1; }\n"
        "    async int f() {\n"
        "        var x = await this.g();\n"
        "        throw std.runtime_error(\"x\");\n"
        "    }\n"
        "}\n",
        "c++20");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("co_await std::suspend_never{};"), std::string::npos) << r.cpp;
}

// The finally block of an async body does not run when a reload made the code stale.
TEST(AsyncTranslation, FinallyInAsyncBodyChecksTheGeneration)
{
    const hotc::TranslateResult r = translate_std(
        "class A {\n"
        "    int n = 0;\n"
        "    async int g() { return 1; }\n"
        "    async int f() {\n"
        "        try {\n"
        "            return await this.g();\n"
        "        } finally {\n"
        "            this.n = this.n + 1;\n"
        "        }\n"
        "    }\n"
        "    int h() {\n"
        "        try { return 1; } finally { this.n = 2; }\n"
        "    }\n"
        "}\n",
        "c++20");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t first = r.cpp.find("if (HOT_UNLIKELY(!hot_live)) return;");
    ASSERT_NE(first, std::string::npos) << r.cpp;
    // Only the async body has the check. The plain method h has none.
    EXPECT_EQ(r.cpp.find("if (HOT_UNLIKELY(!hot_live)) return;", first + 1), std::string::npos) << r.cpp;
}
