// Tests for the safe call "?." at the start of an expression statement.
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

TEST(SafeCallTranslation, CallOnAHandleChecksTheHandle)
{
    const hotc::TranslateResult r = check_translate(kDefaultShared, "    void Run(Weapon m) { m?.Draw(1); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ if (m) hot::deref(m).Draw(1); }")) << r.cpp;
}

TEST(SafeCallTranslation, AssignmentToAFieldChecksTheHandle)
{
    const hotc::TranslateResult r = check_translate(kDefaultShared, "    void Run(Weapon m) { m?.dmg = 3; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ if (m) hot::deref(m).dmg = 3; }")) << r.cpp;
}

TEST(SafeCallTranslation, PointerReceiverChecksThePointer)
{
    const hotc::TranslateResult r = check_translate("", "    void Run(Weapon* p) { p?.Draw(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{ if (p) hot::deref(p).Draw(); }")) << r.cpp;
}

TEST(SafeCallTranslation, ReceiverThatIsNotAnameRunsOnce)
{
    const hotc::TranslateResult r =
        check_translate(kDefaultShared, "    Weapon Get() { return null; }\n    void Run() { this.Get()?.Draw(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto __safe0 = ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "if (__safe0) hot::deref(__safe0).Draw();")) << r.cpp;
}

TEST(SafeCallTranslation, SafeCallInsideAnotherExpressionIsAnError)
{
    for (const char* body : {"    void Run(Weapon m) { var x = m?.dmg; }\n", "    void Run(Weapon m) { f(m?.dmg); }\n",
             "    void Run(Weapon m) { return m?.dmg + 1; }\n"})
    {
        const hotc::TranslateResult r = check_translate(kDefaultShared, body);
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "'?.' is allowed only at the start of an expression statement"))
            << r.diagnostics;
    }
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
