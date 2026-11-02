// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for script attributes: "[attribute] class Name;" and the tag "[Name]" on a script class.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

hotc::TranslateResult attr_translate(
    const std::string& script, const std::vector<std::pair<std::string, std::string>>& preludes = {})
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, "using \"w.h\";\n" + script);
    for (const auto& [name, text] : preludes)
        o.preludes.push_back({name, text});
    return hotc::translate(o);
}

} // namespace

TEST(ScriptAttributeTranslation, TagsGoToTheRecordInSourceOrder)
{
    const hotc::TranslateResult r = attr_translate(
        "[attribute] class Spawnable;\n[attribute] class Saved;\n"
        "[Saved, Spawnable] class Foo : Widget { }\nclass Bar : Widget { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "attributes[] = {\"Saved\", \"Spawnable\", nullptr};")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo_record.attributes = ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Bar_record.attributes")) << r.cpp;
}

TEST(ScriptAttributeTranslation, PreludeDeclaresTheAttributeClass)
{
    const hotc::TranslateResult r
        = attr_translate("[Spawnable] class Foo : Widget { }\n", {{"host.prelude", "[attribute] class Spawnable;\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo_record.attributes = ")) << r.cpp;
}

TEST(ScriptAttributeTranslation, UndeclaredTagIsAnError)
{
    const hotc::TranslateResult r = attr_translate("[Spawnable] class Foo : Widget { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "unknown attribute 'Spawnable' on class 'Foo'")) << r.diagnostics;
}

TEST(ScriptAttributeTranslation, AttributeClassMisuseIsAnError)
{
    const std::pair<const char*, const char*> cases[] = {
        {"[attribute] class Tag { }\n", "an attribute class has no body"},
        {"[attribute, native] class Tag;\n", "attribute 'native' is not allowed on an attribute class"},
        {"[attribute] class : Widget;\n", "an attribute class is declared as '[attribute] class Name;'"},
        {"[attribute] class A.B;\n", "an attribute class is declared as '[attribute] class Name;'"},
        {"[Tag, native] class Thing;\n", "unknown attribute 'Tag'"},
        {"[attribute] class Tag;\n[Tag, Tag] class Foo : Widget { }\n", "duplicate attribute 'Tag'"},
        {"[attribute] class Tag;\n[Tag(\"x\")] class Foo : Widget { }\n", "unknown attribute 'Tag'"},
    };
    for (const auto& [script, error] : cases)
    {
        const hotc::TranslateResult r = attr_translate(script);
        EXPECT_FALSE(r.ok) << script;
        EXPECT_TRUE(has(r.diagnostics, error)) << script << r.diagnostics;
    }
}
