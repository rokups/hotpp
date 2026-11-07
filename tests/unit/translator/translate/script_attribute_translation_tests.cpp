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

namespace
{

// A ".cpp" script. HOT_ATTRIBUTE("Name") is the annotation "hot.attribute=Name" on the class.
hotc::TranslateResult cpp_attr_translate(const std::string& tags, bool declare = true)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o
        = temp_options(dir, "#include \"w.h\"\nclass " + tags + " Foo : public Widget { public: int n = 0; };\n");
    o.script_name = "t.cpp";
    // The include dir with "hot/script.h": the translator injects the header, so HOT_ATTRIBUTE works.
    o.include_dirs.push_back(std::string(HOT_SOURCE_DIR) + "/include");
    if (declare)
        o.preludes.push_back({"host.prelude", "[attribute] class Spawnable;\n[attribute] class Saved;\n"});
    return hotc::translate(o);
}

} // namespace

TEST(ScriptAttributeTranslation, CppScriptTagsComeFromTheAnnotation)
{
    const hotc::TranslateResult r = cpp_attr_translate(
        "[[clang::annotate(\"hot.attribute=Saved\")]] [[clang::annotate(\"hot.attribute=Spawnable\")]]");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "attributes[] = {\"Saved\", \"Spawnable\", nullptr};")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo_record.attributes = ")) << r.cpp;
}

TEST(ScriptAttributeTranslation, CppScriptTagNeedsAPreludeDeclaration)
{
    const hotc::TranslateResult r = cpp_attr_translate("[[clang::annotate(\"hot.attribute=Saved\")]]", false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "unknown attribute 'Saved' on class 'Foo'")) << r.diagnostics;
}

TEST(ScriptAttributeTranslation, CppScriptMacroGivesTheTag)
{
    const hotc::TranslateResult r = cpp_attr_translate("HOT_ATTRIBUTE(\"Spawnable\")");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "attributes[] = {\"Spawnable\", nullptr};")) << r.cpp;
}

TEST(ScriptAttributeTranslation, CppScriptIgnoresOtherAnnotations)
{
    const hotc::TranslateResult r = cpp_attr_translate("[[clang::annotate(\"tool.mark\")]]");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "Foo_record.attributes")) << r.cpp;
}

// A tag on a field goes to the row of the field. The strings of the tag are its arguments.
TEST(ScriptAttributeTranslation, FieldTagsGoToTheFieldRow)
{
    const hotc::TranslateResult r = attr_translate("[attribute] class Saved;\n"
                                                   "class Foo : Widget {\n"
                                                   "    [Saved(\"Hit \\\"points\\\"\", \"net\")] int hp = 1;\n"
                                                   "    [Saved, alias(\"old\")] float speed = 2.0f;\n"
                                                   "    int plain = 0;\n"
                                                   "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{\"Hit \\\"points\\\"\", \"net\", nullptr};")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "attrs_hp[] = {{\"Saved\", ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "attrs_speed[] = {{\"Saved\", nullptr}, {nullptr, nullptr}};")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "attrs_plain")) << r.cpp;
}

TEST(ScriptAttributeTranslation, FieldTagMisuseIsAnError)
{
    const std::pair<const char*, const char*> cases[] = {
        {"class Foo : Widget { [Saved] int hp = 1; }\n",
            "unknown attribute 'Saved' on field 'Foo.hp': declare it as '[attribute] class Saved;'"},
        {"[attribute] class Saved;\nclass Foo : Widget { [Saved] void Run() { } }\n", "unknown attribute 'Saved'"},
        {"[attribute] class Saved;\nclass Foo : Widget { [Saved, Saved] int hp = 1; }\n", "duplicate attribute 'Saved'"},
    };
    for (const auto& [script, error] : cases)
    {
        const hotc::TranslateResult r = attr_translate(script);
        EXPECT_FALSE(r.ok) << script;
        EXPECT_TRUE(has(r.diagnostics, error)) << script << r.diagnostics;
    }
}

// ---- tags on a property

TEST(ScriptAttributeTranslation, PropertyTagGoesToThePropertyTable)
{
    const hotc::TranslateResult r = attr_translate(
        "[attribute] class Attribute;\n"
        "class Foo : Widget {\n"
        "    int hp = 5;\n"
        "    [Attribute(\"Max Health\", \"AM_DEFAULT\")] int Health { get { return hp; } set { hp = value; } }\n"
        "    [Attribute] float Speed { get; set; }\n"
        "    int Plain { get; set; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static const hot::PropertyInfo Foo_props[]")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "{\"Health\", ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "{\"Speed\", ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "{\"Plain\", ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "\"Max Health\", \"AM_DEFAULT\", nullptr")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__pget_Health(")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__pset_Health(")) << r.cpp;
}

TEST(ScriptAttributeTranslation, ClassWithoutPropertyTagHasNoPropertyTable)
{
    const hotc::TranslateResult r = attr_translate(
        "[attribute] class Attribute;\nclass Foo : Widget { [Attribute] int n = 1;\n int P { get; set; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot::PropertyInfo")) << r.cpp;
}

TEST(ScriptAttributeTranslation, PropertyTagNeedsBothAccessors)
{
    const char* bodies[] = {"[Attribute] int P { get; }\n", "[Attribute] int P { set; }\n",
        "[Attribute] int P => 3;\n"};
    for (const char* body : bodies)
    {
        const hotc::TranslateResult r = attr_translate(
            std::string("[attribute] class Attribute;\nclass Foo : Widget { ") + body + "}\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "on property 'Foo.P' needs both 'get' and 'set'")) << body << r.diagnostics;
    }
}

TEST(ScriptAttributeTranslation, PropertyTagMisuseIsAnError)
{
    const hotc::TranslateResult undeclared = attr_translate("class Foo : Widget { [Spawn] int P { get; set; } }\n");
    EXPECT_FALSE(undeclared.ok);
    EXPECT_TRUE(has(undeclared.diagnostics, "unknown attribute 'Spawn' on property 'Foo.P'")) << undeclared.diagnostics;
    const hotc::TranslateResult stat =
        attr_translate("[attribute] class Attribute;\nclass Foo : Widget { [Attribute] static int P { get; set; } }\n");
    EXPECT_FALSE(stat.ok);
    EXPECT_TRUE(has(stat.diagnostics, "not allowed on the static property 'Foo.P'")) << stat.diagnostics;
}
