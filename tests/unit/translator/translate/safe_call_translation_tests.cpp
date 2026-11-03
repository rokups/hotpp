// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the safe call "?.": at the start of a statement and inside an expression.
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

TEST(SafeCallTranslation, CallOnAHandleChecksTheHandle)
{
    const hotc::TranslateResult r = check_translate(kDefaultShared, "    void Run(Weapon m) { m?.Draw(1); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ HOT_TRACE_LINE(); if (m) hot::deref(m).Draw(1); }")) << r.cpp;
}

TEST(SafeCallTranslation, AssignmentToAFieldChecksTheHandle)
{
    const hotc::TranslateResult r = check_translate(kDefaultShared, "    void Run(Weapon m) { m?.dmg = 3; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ HOT_TRACE_LINE(); if (m) hot::deref(m).dmg = 3; }")) << r.cpp;
}

TEST(SafeCallTranslation, PointerReceiverChecksThePointer)
{
    const hotc::TranslateResult r = check_translate("", "    void Run(Weapon* p) { p?.Draw(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ HOT_TRACE_LINE(); if (p) hot::deref(p).Draw(); }")) << r.cpp;
}

TEST(SafeCallTranslation, ReceiverThatIsNotAnameRunsOnce)
{
    const hotc::TranslateResult r =
        check_translate(kDefaultShared, "    Weapon Get() { return null; }\n    void Run() { this.Get()?.Draw(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto __safe0 = ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "if (__safe0) hot::deref(__safe0).Draw();")) << r.cpp;
}

// A script class with a handle member, a value member and two methods.
static const std::string kNode = std::string(kDefaultShared)
    + "class Node { $Node next; int v; $Node Self() { return this.next; } int Add(int a) { return a; } void Ping() {} }\n";

TEST(SafeCallTranslation, SafeMemberInAVarInitializer)
{
    const hotc::TranslateResult r = check_translate(kNode, "    void Run($Node n) { var x = n?.v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::safe_access(n, [&](auto&& __safe0) { return (")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "); })")) << r.cpp;
}

TEST(SafeCallTranslation, SafeCallAsAnArgument)
{
    const hotc::TranslateResult r =
        check_translate(kNode, "    void Run($Node n) { Show(n?.Add(2)); }\n    void Show(int? v) {}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::safe_access(n, [&](auto&& __safe0) { return (")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "2)); })")) << r.cpp;
}

TEST(SafeCallTranslation, SafeMemberInAReturnAndInAnOperator)
{
    for (const char* body : {"    int? Run($Node n) { return n?.v; }\n", "    int Run($Node n) { return (n?.v) ?? 1; }\n"})
    {
        const hotc::TranslateResult r = check_translate(kNode, body);
        ASSERT_TRUE(r.ok) << body << r.diagnostics;
        EXPECT_TRUE(has(r.cpp, "hot::safe_access(n, ")) << r.cpp;
    }
}

TEST(SafeCallTranslation, ChainOfSafeMembers)
{
    const hotc::TranslateResult r = check_translate(kNode, "    void Run($Node n) { var x = n?.next?.v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::safe_access(hot::safe_access(n, [&](auto&& __safe0) { return (")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "[&](auto&& __safe1) { return (")) << r.cpp;
}

TEST(SafeCallTranslation, ReceiverIsAnArgumentOfTheHelper)
{
    const hotc::TranslateResult r = check_translate(kNode, "    void Run($Node n) { if (n == null) return; var x = n.Self()?.v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::safe_access(")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "auto __safe0 = ")) << r.cpp;
}

TEST(SafeCallTranslation, VoidCallInsideAnExpressionContext)
{
    const hotc::TranslateResult r = check_translate(kNode, "    void Run($Node n) { n?.Ping(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "if (n)")) << r.cpp; // the statement form stays an "if"
}

TEST(SafeCallTranslation, SafeMemberAsAnAssignmentTargetInAnExpressionIsAnError)
{
    const hotc::TranslateResult r = check_translate(kNode, "    void Run($Node n) { var x = (n?.v = 3); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'?.' cannot be the target of an assignment inside an expression")) << r.diagnostics;
}

TEST(SafeCallTranslation, TernaryWithADotNumberIsNotASafeCall)
{
    const hotc::TranslateResult r = check_translate("", "    float Run(bool c) { return c ?.5 : 1.5; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(SafeCallTranslation, PropertyReceiverRunsTheGetterOnce)
{
    const hotc::TranslateResult r =
        check_translate(kDefaultShared, "    Weapon W { get; set; }\n    void Run() { W?.Draw(1); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto __safe0 = ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "if (__safe0) hot::deref(__safe0).Draw(1);")) << r.cpp;
}
