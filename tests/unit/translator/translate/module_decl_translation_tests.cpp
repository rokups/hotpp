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
    EXPECT_TRUE(has(r.cpp, "typeid(hot::typekey_t<std::underlying_type_t<Mode>>).name()")) << r.cpp;
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
    expect_error("class H { int n; }\n$H h;\nclass C : Unit { }\n",
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
    EXPECT_NE(r.cpp.find("typeid(int32_t).name(), HotModule__convert_level_hook, nullptr, hot::field_copy<float>()}"), std::string::npos) << r.cpp;
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
    expect_error("int a;\npublic int helper() { return 1; }\nclass C : Unit { }\n",
        "a free function has no access word");
    expect_error("int a;\n[alias(\"x\")] int convert_a(float old) { return 1; }\nclass C : Unit { }\n",
        "attribute 'alias' is not allowed on a convert hook");
}

namespace
{

const char* kAliasHeader =
    "#include <string>\n"
    "#include <vector>\n"
    "namespace ns { struct Thing { int x = 0; }; }\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "};\n";

hotc::TranslateResult translate_alias(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kAliasHeader);
    return hotc::translate(temp_options(dir, "using \"u.h\";\nusing <vector>;\n" + body));
}

void expect_alias_error(const std::string& body, const std::string& msg)
{
    const hotc::TranslateResult r = translate_alias(body);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << r.diagnostics;
}

} // namespace

// An alias of a native type works as that type: field, parameter, return, local, template
// argument and "new". The module alias is also written as C++ text.
TEST(ModuleDeclTranslation, AliasOfANativeTypeIsThatType)
{
    const auto r = translate_alias(
        "using Thing = ns.Thing;\n"
        "using Ints = std.vector<int>;\n"
        "class A : Unit {\n"
        "    Thing t;\n"
        "    Ints list;\n"
        "    std.vector<Thing> things;\n"
        "    Thing Make(Thing p) { Thing l = new Thing(); l.x = p.x; return l; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "ns::Thing t;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::vector<int32_t> list;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::vector<ns::Thing> things;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "ns::Thing l = ns::Thing();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using Thing = ns::Thing;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using Ints = std::vector<int32_t>;\n")) << r.cpp;
}

// An alias of a script class has the handle storage of the class: field, template argument, "new", null.
TEST(ModuleDeclTranslation, AliasOfAScriptClassIsAHandle)
{
    const auto r = translate_alias(
        "class Unit2 : Unit { int hp = 1; }\n"
        "using Soldier = $Unit2;\n"
        "class A : Unit {\n"
        "    Soldier lead;\n"
        "    std.vector<Soldier> squad;\n"
        "    void f(Soldier s) { Soldier n = new Soldier(); if (lead == null) { lead = n; } squad.push_back(s); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Unit2> lead;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::vector<std::shared_ptr<Unit2>> squad;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using Soldier = std::shared_ptr<Unit2>;\n")) << r.cpp;
}

// An alias can use an earlier alias. An alias name before "." is a scope (an enum).
TEST(ModuleDeclTranslation, AliasChainAndEnumAlias)
{
    const auto r = translate_alias(
        "enum Color { Red, Green }\n"
        "using C = Color;\n"
        "using C2 = C;\n"
        "class A : Unit { C2 c = C2.Green; bool g() { return c == C.Green; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Color c")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Color::Green")) << r.cpp;
}

// An alias in a class body is for that class only.
TEST(ModuleDeclTranslation, AliasInAClassBody)
{
    const auto r = translate_alias(
        "class A : Unit {\n"
        "    using Num = int;\n"
        "    using Pt = ns.Thing;\n"
        "    Num n;\n"
        "    Pt p;\n"
        "    Num Get(Pt q) { Num r = q.x; return r; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "using Num = int32_t;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t n;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "ns::Thing p;")) << r.cpp;
    // The alias of a class is not visible in another class: the name stays as written.
    const auto r2 = translate_alias("class A : Unit { using Num = int; }\nclass B : Unit { void f() { Num n = 1; } }\n");
    ASSERT_TRUE(r2.ok) << r2.diagnostics;
    EXPECT_TRUE(has(r2.cpp, "Num n = 1;")) << r2.cpp;
    EXPECT_FALSE(has(r2.cpp, "int32_t n = 1;")) << r2.cpp;
}

// An alias can use only an earlier alias. A later alias and a cycle are errors, and the alias stays out of the map.
TEST(ModuleDeclTranslation, ErrorAliasUsesALaterAlias)
{
    expect_alias_error("using A = std.vector<B>;\nusing B = int;\nclass C : Unit { }\n",
        "the type alias 'A' uses a later alias 'B'");
}

TEST(ModuleDeclTranslation, ErrorCyclicAlias)
{
    expect_alias_error("using A = B;\nusing B = A;\nclass C : Unit { }\n", "the type alias 'A' is cyclic");
    expect_alias_error("using A = B;\nusing B = A;\nclass C : Unit { }\n",
        "the type alias 'B' uses the alias 'A' that has an error");
}

TEST(ModuleDeclTranslation, ErrorAliasForALaterAliasInAClass)
{
    expect_alias_error("class C : Unit { using A = B; using B = int; }\n", "the type alias 'A' uses a later alias 'B'");
}

// A name that is a type of the language or a C++ keyword is not a name for an alias.
TEST(ModuleDeclTranslation, ErrorAliasWithTheNameOfABuiltinType)
{
    for (const char* name : {"int", "uint", "int32", "byte", "bool", "float", "long", "Task", "delete"})
    {
        const std::string n = name;
        SCOPED_TRACE(n);
        expect_alias_error("using " + n + " = ns.Thing;\nclass C : Unit { }\n",
            "'" + n + "' is a type of the language or a C++ keyword");
    }
}

TEST(ModuleDeclTranslation, ErrorAliasToItself)
{
    expect_alias_error("using A = A;\nclass C : Unit { }\n", "the type alias 'A' uses itself");
    expect_alias_error("using A = std.vector<A>;\nclass C : Unit { }\n", "the type alias 'A' uses itself");
}

TEST(ModuleDeclTranslation, ErrorDuplicateAlias)
{
    expect_alias_error("using A = int;\nusing A = float;\nclass C : Unit { }\n", "duplicate type alias 'A'");
    expect_alias_error("class C : Unit { using A = int; using A = int; }\n", "duplicate type alias 'A'");
}

TEST(ModuleDeclTranslation, ErrorAliasWithTheNameOfAClass)
{
    expect_alias_error("using C = int;\nclass C : Unit { }\n", "the type alias 'C' has the name of a class");
    expect_alias_error("class C : Unit { using C = int; }\n", "the type alias 'C' has the name of a class");
}

TEST(ModuleDeclTranslation, ErrorNamespaceInAClassBody)
{
    expect_alias_error("class C : Unit { using ns; }\n", "a class has only a type alias");
}
