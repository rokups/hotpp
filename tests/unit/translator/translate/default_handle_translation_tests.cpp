// Tests for the stub "class *;", "default_handle", the warnings and the preludes (design 7.10.2, 7.10.3).
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

TEST(DefaultHandleTranslation, AClassGetsTheDefaultHandleAndPlainDataStaysAValue)
{
    const hotc::TranslateResult r =
        check_translate("", "    Mesh m;\n    Vec3 v;\n    Plain p;\n    void Make() { this.m = new Mesh(); }\n",
            {{"engine.prelude", kDefaultShared}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Mesh> m;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    Vec3 v;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    Plain p;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__st->m = std::make_shared<Mesh>();")) << r.cpp;
}

TEST(DefaultHandleTranslation, TheStubInTheScriptWorks)
{
    const hotc::TranslateResult r =
        check_translate(kDefaultShared, "    void Take(Mesh m) { }\n    Mesh Get() { return null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const std::shared_ptr<Mesh>& m")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Mesh> Foo__Get(")) << r.cpp;
}

TEST(DefaultHandleTranslation, ACustomDefaultHandleWorks)
{
    const hotc::TranslateResult r =
        check_translate("[native, default_handle(\"Arrow<$T>\")] class *;\n", "    Mesh m;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    Arrow<Mesh> m;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, ExactStubAndPatternStubBeatTheFallback)
{
    const hotc::TranslateResult r = check_translate(std::string(kDefaultShared)
            + "[native, value] class std.vector<*>;\n[native, handle(\"Arrow<$T>\")] class Weapon;\n",
        "    Mesh m;\n    Weapon w;\n    std.vector<int> v;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Mesh> m;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    Arrow<Weapon> w;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    std::vector<int32_t> v;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, ValueStubOnAStandardTypeBeatsTheFallback)
{
    const hotc::TranslateResult r = check_translate("", "    std.string s;\n",
        {{"engine.prelude", std::string(kDefaultShared) + "[native, value] class std.string;\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::string s;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, WithoutTheStubAClassHasNoHandle)
{
    const hotc::TranslateResult r = check_translate("", "    Mesh m;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    Mesh m;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, ValueOnStarIsAnError)
{
    const hotc::TranslateResult r = check_translate("[native, value] class *;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "t.hot:2:1: error: 'value' is not allowed on 'class *'")) << r.diagnostics;
}

TEST(DefaultHandleTranslation, TwoStarsInOneFileAreAnError)
{
    const hotc::TranslateResult r =
        check_translate(std::string(kDefaultShared) + "[native, default_handle(\"Arrow<$T>\")] class *;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "t.hot:3:1: error: only one 'class *'")) << r.diagnostics;
}

TEST(DefaultHandleTranslation, TwoConflictingStubsInOneFileAreAnError)
{
    const hotc::TranslateResult r =
        check_translate("[native, value] class Plain;\n[native, handle(\"Arrow<$T>\")] class Plain;\n", "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "t.hot:3:1: error: duplicate stub for 'Plain'")) << r.diagnostics;
}

TEST(DefaultHandleTranslation, ScriptStubOverridesPreludeStubWithAWarning)
{
    const hotc::TranslateResult r = check_translate("[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n",
        "    Weapon w;\n", {{"engine.prelude", "[native, value] class Weapon;\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "t.hot:2:1: warning: the stub for 'Weapon' overrides the stub of a prelude"))
        << r.warnings;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Weapon> w;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, ScriptStarOverridesPreludeStarWithAWarning)
{
    const hotc::TranslateResult r = check_translate(
        "[native, default_handle(\"Arrow<$T>\")] class *;\n", "    Mesh m;\n", {{"engine.prelude", kDefaultShared}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "warning: the 'class *' of the script overrides")) << r.warnings;
    EXPECT_TRUE(has(r.cpp, "    Arrow<Mesh> m;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, ALaterPreludeOverridesAnEarlierOneWithoutAWarning)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon w;\n    Mesh m;\n",
        {{"a.prelude", std::string(kDefaultShared) + "[native, value] class Weapon;\n"},
            {"b.prelude",
                "[native, default_handle(\"Arrow<$T>\")] class *;\n[native, handle(\"Arrow<$T>\")] class Weapon;\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
    EXPECT_TRUE(has(r.cpp, "    Arrow<Weapon> w;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    Arrow<Mesh> m;\n")) << r.cpp;
}

TEST(DefaultHandleTranslation, ThePreludeTextChangesTheOutputThroughOneSession)
{
    // The cache holds the facts of the headers only. The prelude is read on every call.
    hotc::TranslatorSession session;
    const auto run = [&](const std::string& prelude)
    { return check_translate("", "    Weapon w;\n", {{"engine.prelude", prelude}}, true, &session); };
    const hotc::TranslateResult a = run("[native, value] class Weapon;\n");
    const hotc::TranslateResult b = run("[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n");
    const hotc::TranslateResult c = run("[native, value] class Weapon;\n");
    ASSERT_TRUE(a.ok) << a.diagnostics;
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_TRUE(has(a.cpp, "    Weapon w;\n"));
    EXPECT_TRUE(has(b.cpp, "    std::shared_ptr<Weapon> w;\n"));
    EXPECT_EQ(a.cpp, c.cpp);
    // The handle stub of run b asks one new question: one more parse. Run c asks nothing new.
    EXPECT_EQ(session.parse_count(), 2);
}

TEST(DefaultHandleTranslation, TheFactsOfUsedTypesComeFromTheSessionCache)
{
    hotc::TranslatorSession session;
    const auto run = [&]
    { return check_translate("", "    Mesh m;\n", {{"engine.prelude", kDefaultShared}}, true, &session); };
    ASSERT_TRUE(run().ok);
    ASSERT_TRUE(run().ok);
    EXPECT_EQ(session.parse_count(), 1);
}
