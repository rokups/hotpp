// Tests for sppc::translate with the module-level declarations: module variables, enum, struct.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

const char* kHeader =
    "#include <string>\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "};\n";

sppc::TranslateResult translate_script(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return sppc::translate(temp_options(dir, "using \"u.h\";\nusing <string>;\n" + body));
}

void expect_error(const std::string& body, const std::string& msg)
{
    const sppc::TranslateResult r = translate_script(body);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << r.diagnostics;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(ModuleDeclTranslation, EnumBecomesAnEnumClass)
{
    const auto r = translate_script("enum Color : byte { Red, Green = 5, Blue }\nclass A : Unit { Color c = Color.Green; }\n");
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
    EXPECT_TRUE(has(r.cpp, "[value] class P")) << r.cpp;
}

TEST(ModuleDeclTranslation, ModuleVariableUsesTheModuleBlock)
{
    const auto r = translate_script(
        "[alias(\"old\")] int total = 3;\n"
        "class A : Unit { override void Update(float dt) { total = total + 1; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "spp_mod()->total = spp_mod()->total + 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "{\"total\", \"old\",")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "reg.set_module_state(&SppModule_layout, &SppModule_slot);")) << r.cpp;
}

TEST(ModuleDeclTranslation, NoModuleVariableMeansNoModuleBlock)
{
    const auto r = translate_script("class A : Unit { int n; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "SppModule")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "set_module_state")) << r.cpp;
}

TEST(ModuleDeclTranslation, LocalHidesAModuleVariable)
{
    const auto r = translate_script(
        "int total = 0;\n"
        "class A : Unit { override void Update(float dt) { int total = 5; total = total + 1; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "total = total + 1")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "spp_mod()->total = spp_mod()->total")) << r.cpp;
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
    expect_error("class H { int n; }\nH h;\nclass C : Unit { }\n", "a module variable can use only native types, enums and [value] classes");
}

TEST(ModuleDeclTranslation, ErrorModuleVariableInAnInitializer)
{
    expect_error("int a = 1;\nint b = a + 1;\nclass C : Unit { }\n", "cannot be used in the initializer of a module variable");
}
