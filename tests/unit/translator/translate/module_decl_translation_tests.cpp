// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the module-level declarations: module variables, enum, struct.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kHeader =
    "#include <string>\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "};\n";

hotc::TranslateResult translate_script(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"u.h\";\nusing <string>;\n" + body));
}

void expect_error(const std::string& body, const std::string& msg)
{
    const hotc::TranslateResult r = translate_script(body);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << r.diagnostics;
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(ModuleDeclTranslation, EnumBecomesAnEnumClass)
{
    const auto r =
        translate_script("enum Color : byte { Red, Green = 5, Blue }\nclass A : Unit { Color c = Color.Green; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "enum class Color : uint8_t {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Green = 5")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Color::Green")) << r.cpp;
}

TEST(ModuleDeclTranslation, EnumDefaultTypeIsInt32)
{
    const auto r = translate_script("enum Mode { Idle, Run }\nclass A : Unit { Mode m; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "enum class Mode : int32_t {")) << r.cpp;
}

TEST(ModuleDeclTranslation, EnumFieldUsesTheUnderlyingTypeAsKey)
{
    const auto r = translate_script("enum Mode { Idle, Run }\nclass A : Unit { Mode m; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "typeid(std::underlying_type_t<Mode>).name()")) << r.cpp;
}

TEST(ModuleDeclTranslation, StructIsAValueClass)
{
    const auto r = translate_script("struct P { float x = 1.0; }\nclass A : Unit { P p; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct P")) << r.cpp;
}

TEST(ModuleDeclTranslation, ModuleVariableUsesTheModuleBlock)
{
    const auto r = translate_script(
        "[alias(\"old\")] int total = 3;\n"
        "class A : Unit { override void Update(float dt) { total = total + 1; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot_mod()->total = hot_mod()->total + 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "{\"total\", \"old\",")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "reg.set_module_state(&HotModule_layout, &HotModule_slot);")) << r.cpp;
}

TEST(ModuleDeclTranslation, NoModuleVariableMeansNoModuleBlock)
{
    const auto r = translate_script("class A : Unit { int n; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "HotModule")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "set_module_state")) << r.cpp;
}

TEST(ModuleDeclTranslation, LocalHidesAModuleVariable)
{
    const auto r = translate_script(
        "int total = 0;\n"
        "class A : Unit { override void Update(float dt) { int total = 5; total = total + 1; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "total = total + 1")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot_mod()->total = hot_mod()->total")) << r.cpp;
}

TEST(ModuleDeclTranslation, ErrorDuplicateModuleVariable)
{
    expect_error("int a;\nint a;\nclass A : Unit { }\n", "module variable 'a' is declared twice");
}

TEST(ModuleDeclTranslation, ErrorDuplicateEnum)
{
    expect_error("enum E { A }\nenum E { B }\nclass C : Unit { }\n", "enum 'E' is declared twice");
}

TEST(ModuleDeclTranslation, ErrorDuplicateEnumerator)
{
    expect_error("enum E { A, A }\nclass C : Unit { }\n", "enumerator 'A' is declared twice in enum 'E'");
}

TEST(ModuleDeclTranslation, ErrorEnumUnderlyingType)
{
    expect_error("enum E : float { A }\nclass C : Unit { }\n", "the type of an enum must be");
}

TEST(ModuleDeclTranslation, ErrorModuleVariableOfAScriptHandleClass)
{
    expect_error("class H { int n; }\nH h;\nclass C : Unit { }\n",
        "a module variable can use only native types, enums and structs");
}

// An initializer can read a module variable declared before it (S16). The loader runs the
// initializers in declaration order, so the earlier variable has its value.
TEST(ModuleDeclTranslation, InitializerReadsAnEarlierModuleVariable)
{
    const hotc::TranslateResult r = translate_script("int a = 1;\nint b = a + 1;\nclass C : Unit { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("new (f) T(hot_mod()->a + 1);"), std::string::npos) << r.cpp;
    // hot_mod() comes before the init functions that use it.
    EXPECT_LT(r.cpp.find("HOT_HIDDEN static HotModule* hot_mod()"), r.cpp.find("HotModule__init_b")) << r.cpp;
}

TEST(ModuleDeclTranslation, ErrorModuleVariableDeclaredLaterInAnInitializer)
{
    expect_error("int b = a + 1;\nint a = 1;\nclass C : Unit { }\n",
        "the module variable 'a' is not declared before this variable");
    expect_error(
        "int a = a + 1;\nclass C : Unit { }\n", "the module variable 'a' is not declared before this variable");
}

// "T convert_<var>(Old old)" at file level is the convert hook of a module variable (S16).
TEST(ModuleDeclTranslation, ConvertHookOfAModuleVariable)
{
    const hotc::TranslateResult r = translate_script(
        "float level = 1.0;\nfloat convert_level(int old) { return old * 1.5; }\nclass C : Unit { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("static float HotModule__convert_level(int32_t old)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("typeid(int32_t).name(), HotModule__convert_level_hook}"), std::string::npos) << r.cpp;
}

TEST(ModuleDeclTranslation, ConvertHookErrors)
{
    expect_error("int a;\nint convert_b(int old) { return old; }\nclass C : Unit { }\n",
        "'convert_b' is not a convert hook: the script has no module variable 'b'");
    expect_error("int a;\nfloat convert_a(int old) { return 1.0; }\nclass C : Unit { }\n",
        "'convert_a' must return the type of the module variable: 'int'");
    expect_error("int a;\nint convert_a(int old, int x) { return old; }\nclass C : Unit { }\n",
        "'convert_a' is a convert hook. It takes one parameter: the old value");
    expect_error("int a;\nint b;\nint convert_a(float old) { return b; }\nclass C : Unit { }\n",
        "the module variable 'b' cannot be used in a convert hook");
    expect_error("int a;\nint helper() { return 1; }\nclass C : Unit { }\n",
        "a function must be a member of a class. At file level, only a convert hook");
    expect_error("int a;\n[alias(\"x\")] int convert_a(float old) { return 1; }\nclass C : Unit { }\n",
        "attribute 'alias' is not allowed on a convert hook");
}
