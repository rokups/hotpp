// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the checks of a stub (design 7.10.7). Libclang answers the questions about the type.
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

TEST(StubChecks, ValueOfACopyableNothrowMovableTypeIsOk)
{
    const hotc::TranslateResult r = check_translate("[native, value] class Plain;\n", "    Plain p;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    Plain p;\n")) << r.cpp;
}

TEST(StubChecks, ValueNeedsACopyConstructor)
{
    const hotc::TranslateResult r = check_translate("[native, value] class NoCopy;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "t.hot:2:1: error: 'value' needs a copy constructor: 'NoCopy'")) << r.diagnostics;
}

TEST(StubChecks, ValueNeedsANothrowMoveConstructor)
{
    const hotc::TranslateResult r = check_translate("[native, value] class ThrowMove;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "nothrow move constructor: 'ThrowMove'")) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "copy constructor")) << r.diagnostics;
}

TEST(StubChecks, ValueOnAPolymorphicTypeIsAnError)
{
    const hotc::TranslateResult r = check_translate("[native, value] class Poly;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'value' on the polymorphic type 'Poly'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "'final'")) << r.diagnostics;
}

TEST(StubChecks, ValueOnAFinalPolymorphicTypeIsOk)
{
    const hotc::TranslateResult r = check_translate("[native, value] class PolyFinal;\n", "");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(StubChecks, ValueOnAStandardTypeWorks)
{
    const hotc::TranslateResult r = check_translate("[native, value] class std.string;\n", "    std.string s;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::string s;\n")) << r.cpp;
}

TEST(StubChecks, HandleNeedsOperatorArrow)
{
    const hotc::TranslateResult r = check_translate("[native, handle(\"NoArrow<$T>\")] class Weapon;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'handle' needs 'operator->'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "NoArrow<Weapon>")) << r.diagnostics;
}

TEST(StubChecks, HandleWithArrowSharedPtrAndRawPointerAreOk)
{
    for (const char* tpl : {"Arrow<$T>", "std::shared_ptr<$T>", "$T*"})
    {
        const hotc::TranslateResult r =
            check_translate(std::string("[native, handle(\"") + tpl + "\")] class Weapon;\n", "");
        EXPECT_TRUE(r.ok) << tpl << ": " << r.diagnostics;
    }
}

TEST(StubChecks, IntrusiveNeedsBothMethods)
{
    const hotc::TranslateResult r = check_translate("[native, intrusive(\"AddRef\", \"Grab\")] class Counted;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "t.hot:2:1: error: 'intrusive' method 'Grab' is not a usable member of 'Counted'"))
        << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "'AddRef'")) << r.diagnostics;
}

TEST(StubChecks, IntrusiveWithBothMethodsIsOk)
{
    const hotc::TranslateResult r =
        check_translate("[native, intrusive(\"AddRef\", \"Release\")] class Counted;\n", "    Counted c;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Intrusive<Counted, &Counted::AddRef, &Counted::Release> c;")) << r.cpp;
}

TEST(StubChecks, IntrusiveMethodMustNotBeOverloaded)
{
    const hotc::TranslateResult r =
        check_translate("[native, intrusive(\"AddRef\", \"Release\")] class Overloaded;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'AddRef'")) << r.diagnostics;
}

TEST(StubChecks, ATypeThatTheHeadersDoNotHaveIsSkipped)
{
    const hotc::TranslateResult r = check_translate("[native, value] class NotInTheHeaders;\n", "");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(StubChecks, AnErrorInAPreludeNamesThePreludeFile)
{
    const hotc::TranslateResult r = check_translate("", "", {{"engine.prelude", "\n[native, value] class NoCopy;\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "engine.prelude:2:1: error: 'value' needs a copy constructor")) << r.diagnostics;
}

TEST(StubChecks, PatternStubsAreNotChecked)
{
    const hotc::TranslateResult r = check_translate("[native, value] class std.vector<*>;\n", "");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(StubChecks, TheFactsComeFromTheSessionCache)
{
    hotc::TranslatorSession session;
    const char* stubs = "[native, value] class Plain;\n[native, handle(\"Arrow<$T>\")] class Weapon;\n";
    ASSERT_TRUE(check_translate(stubs, "", {}, true, &session).ok);
    ASSERT_TRUE(check_translate(stubs, "", {}, true, &session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    // A new question (another stub) needs one more parse. The old answers stay.
    ASSERT_TRUE(check_translate("[native, value] class PolyFinal;\n", "", {}, true, &session).ok);
    EXPECT_EQ(session.parse_count(), 2);
    ASSERT_TRUE(check_translate("[native, value] class PolyFinal;\n", "", {}, true, &session).ok);
    EXPECT_EQ(session.parse_count(), 2);
}

// S20b: each instantiation of a pattern stub that the script uses gets the checks of design 7.10.7.
TEST(StubChecks, UsedPatternInstantiationIsChecked)
{
    const hotc::TranslateResult r = check_translate("[native, value] class Box<*>;\n", "    Box<int> b;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'value' needs a copy constructor")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "(the pattern stub")) << r.diagnostics;
}

TEST(StubChecks, UsedStandardPatternInstantiationIsOk)
{
    const hotc::TranslateResult r = check_translate(
        "[native, value] class std.vector<*>;\n[native, value] class Plain;\n", "    std.vector<Plain> v;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
