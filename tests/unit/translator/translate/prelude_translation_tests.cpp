// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for cpp_prelude: the text goes to the start of the shell, and the scoped rules decide who gets it.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

hotc::TranslateResult script_translate(const std::string& script)
{
    static TempDir* dir = nullptr;
    if (!dir)
    {
        dir = new TempDir();
        write_file(dir->path() / "w.h", kHierHeader);
    }
    return hotc::translate(temp_options(*dir, "using \"w.h\";\n" + script));
}

const char* kObject = "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Object;\n";

} // namespace

TEST(PreludeTranslation, ShellBodyStartsWithTheText)
{
    const hotc::TranslateResult r = script_translate(std::string(kObject)
        + "[cpp_prelude(\"int pre = 1;\")] class : Object;\n"
          "class Hero : Object { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct Hero_Shell final : Object, hot::ShellBase {\n    int pre = 1;\n")) << r.cpp;
}

TEST(PreludeTranslation, ChildShellGetsTheRuleOfItsBase)
{
    const hotc::TranslateResult r = script_translate(std::string(kObject)
        + "[cpp_prelude(\"below_object;\")] class : Object;\n"
          "[cpp_prelude(\"below_hero;\")] class : Hero;\n"
          "class Hero : Object { }\n"
          "class Smith : Hero { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct Hero_Shell : Object, hot::ShellBase {\n    below_object;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Smith_Shell final : Hero_Shell {\n    below_object;\n    below_hero;\n")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "below_hero;\n    template <class... A>\n    explicit Hero_Shell")) << r.cpp;
}

TEST(PreludeTranslation, TextOfTheClassComesLast)
{
    const hotc::TranslateResult r = script_translate(std::string(kObject)
        + "[cpp_prelude(\"stub_text;\")] class : Object;\n"
          "[cpp_prelude(\"own_text;\")] class Hero : Object { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::ShellBase {\n    stub_text;\n    own_text;\n")) << r.cpp;
}

TEST(PreludeTranslation, ThisAndBaseAreReplaced)
{
    const hotc::TranslateResult r = script_translate(std::string(kObject)
        + "[cpp_prelude(\"HOT($T, $B); SECOND($T);\")] class : Object;\n"
          "class Hero : Object { }\n"
          "class Smith : Hero { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    HOT(Hero_Shell, Object); SECOND(Hero_Shell);\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    HOT(Smith_Shell, Hero_Shell); SECOND(Smith_Shell);\n")) << r.cpp;
}

TEST(PreludeTranslation, SelfStubTextDoesNotReachScriptChild)
{
    const hotc::TranslateResult r = script_translate(
        "[native, self, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\"), cpp_prelude(\"self_text;\")] "
        "class Object;\n"
        "class Hero : Object { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "self_text;")) << r.cpp;
}

TEST(PreludeTranslation, ExactStubTextReachesScriptChild)
{
    const hotc::TranslateResult r = script_translate(
        "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\"), cpp_prelude(\"exact_text;\")] class Object;\n"
        "class Hero : Object { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::ShellBase {\n    exact_text;\n")) << r.cpp;
}

TEST(PreludeTranslation, ClassStarTextGoesFirst)
{
    const hotc::TranslateResult r = script_translate(std::string(kObject)
        + "[cpp_prelude(\"all_text;\")] class *;\n"
          "[cpp_prelude(\"stub_text;\")] class : Object;\n"
          "class Hero : Object { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::ShellBase {\n    all_text;\n    stub_text;\n")) << r.cpp;
}

TEST(PreludeTranslation, StructStarTextGoesToTheStruct)
{
    const hotc::TranslateResult r = script_translate(
        "[cpp_prelude(\"// of $T\")] struct *;\n"
        "struct Pt { int x; }\n"
        "class Hero : Widget { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct Pt {\n    // of Pt\n    int32_t x;\n")) << r.cpp;
}

TEST(PreludeHandle, SelfStopsTheHandleAtTheNamedType)
{
    const hotc::TranslateResult r = hier_translate(
        "[native, self, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Object;\n",
        "    Object o;\n    Item i;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    hot::Intrusive<Object, &Object::AddRef, &Object::Release> o;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    Item i;\n")) << r.cpp;
}

TEST(PreludeHandle, DerivedRuleGivesTheHandleToSubclassesOnly)
{
    const hotc::TranslateResult r = hier_translate(
        "[handle(\"std::shared_ptr<$T>\")] class : Object;\n", "    Object o;\n    Item i;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    Object o;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Item> i;\n")) << r.cpp;
}

TEST(PreludeHandle, DerivedRuleBeatsTheExactStubOfTheBase)
{
    const hotc::TranslateResult r = hier_translate(std::string(kIntrusiveStub)
            + "[handle(\"std::shared_ptr<$T>\")] class : Object;\n",
        "    Object o;\n    Item i;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    hot::Intrusive<Object, &Object::AddRef, &Object::Release> o;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Item> i;\n")) << r.cpp;
}

// A rule "class : Base;" with only a cpp_prelude is not a storage rule. The handle of Base stays.
TEST(PreludeHandle, PreludeOnlyDerivedRuleKeepsTheIntrusiveHandle)
{
    const hotc::TranslateResult r = hier_translate(std::string(kIntrusiveStub)
            + "[cpp_prelude(\"x;\")] class : Object;\n",
        "    Object o;\n    Item i;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    " + kIntr + " i;\n")) << r.cpp;
    const hotc::TranslateResult s = script_translate(std::string(kObject)
        + "[cpp_prelude(\"x;\")] class : Object;\n"
          "class Hero : Object { }\n");
    ASSERT_TRUE(s.ok) << s.diagnostics;
    EXPECT_TRUE(has(s.cpp, "struct Hero_Shell final : Object, hot::ShellBase {\n    x;\n")) << s.cpp;
    EXPECT_FALSE(has(s.cpp, "enable_shared_from_this")) << s.cpp;
}

TEST(PreludeHandle, PreludeOnlyDerivedRuleOnAScriptClassKeepsTheHandle)
{
    const hotc::TranslateResult r = script_translate(std::string(kObject)
        + "[cpp_prelude(\"x;\")] class : Hero;\n"
          "class Hero : Object { }\n"
          "class Smith : Hero { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct Smith_Shell final : Hero_Shell {\n    x;\n")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "enable_shared_from_this")) << r.cpp;
}

// An explicit [native] makes a derived rule a storage rule: a Raw rule stops the walk.
TEST(PreludeHandle, NativeDerivedRuleIsAStorageRule)
{
    const hotc::TranslateResult r = hier_translate(std::string(kIntrusiveStub) + "[native] class : Object;\n",
        "    Item i;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    Item i;\n")) << r.cpp;
}
