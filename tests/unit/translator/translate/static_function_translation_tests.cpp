// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the rules of static methods and free functions: the names, the calls and the bodies.
// The translator checks them and makes the code. The code has a prototype and a definition.
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
    EXPECT_FALSE(r.ok) << body;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << r.diagnostics;
}

void expect_ok(const std::string& body)
{
    const hotc::TranslateResult r = translate_script(body);
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

} // namespace

TEST(StaticFunctionTranslation, ValidFreeFunctionAndStaticAreAccepted)
{
    expect_ok("int Twice(int x) { return x * 2; }\n"
              "class A : Unit {\n"
              "    static int Make(int x) { return x + 1; }\n"
              "    static int Both() { return Make(1) + Twice(2); }\n"
              "    int Use() { return A.Make(2) + Make(3) + Twice(4); }\n"
              "}\n"
              "class B : A {\n"
              "    int Other() { return A.Make(1) + B.Make(2) + Both(); }\n"
              "}\n"
              "class C : Unit {\n"
              "    int Run() { return A.Make(1); }\n"
              "}\n");
}

TEST(StaticFunctionTranslation, FreeFunctionTwiceIsAnError)
{
    expect_error("int F() { return 1; }\nint F(int x) { return x; }\nclass A : Unit { }\n",
        "function 'F' is declared twice (overloads are not supported)");
}

TEST(StaticFunctionTranslation, StaticTwiceIsAnError)
{
    expect_error("class A : Unit {\n    static int F() { return 1; }\n    static int F(int x) { return x; }\n}\n",
        "function 'F' is declared twice (overloads are not supported)");
}

TEST(StaticFunctionTranslation, StaticAndInstanceMethodWithOneNameIsAnError)
{
    expect_error("class A : Unit {\n    static int F() { return 1; }\n    int F(int x) { return x; }\n}\n",
        "function 'F' is declared twice (overloads are not supported)");
    expect_error("class A : Unit {\n    int F(int x) { return x; }\n    static int F() { return 1; }\n}\n",
        "function 'F' is declared twice (overloads are not supported)");
}

TEST(StaticFunctionTranslation, StaticAndFieldWithOneNameIsAnError)
{
    expect_error("class A : Unit {\n    int F;\n    static int F() { return 1; }\n}\n", "'F' is a field and a function");
}

TEST(StaticFunctionTranslation, FreeFunctionNameClashes)
{
    expect_error("int F = 1;\nint F() { return 1; }\nclass A : Unit { }\n",
        "'F' is declared twice (a function and a module variable)");
    expect_error("int F() { return 1; }\nclass F : Unit { }\n", "'F' is declared twice (a function and a class)");
    expect_error("int F() { return 1; }\nstruct F { int x; }\nclass A : Unit { }\n",
        "'F' is declared twice (a function and a struct)");
    expect_error("int F() { return 1; }\nenum F { A, B }\nclass A2 : Unit { }\n",
        "'F' is declared twice (a function and an enum)");
    expect_error("int F() { return 1; }\ninterface F { void M(); }\nclass A : Unit { }\n",
        "'F' is declared twice (a function and an interface)");
}

TEST(StaticFunctionTranslation, GeneratedNameCollision)
{
    expect_error("class C : Unit {\n    static int F() { return 1; }\n    int s_F() { return 2; }\n}\n",
        "the static method 'C.F' and the method 'C.s_F' have the same generated name 'C__s_F'. Rename one of them");
    expect_error("class C : Unit {\n    int s_F() { return 2; }\n    static int F() { return 1; }\n}\n",
        "have the same generated name 'C__s_F'");
}

TEST(StaticFunctionTranslation, ImportedScriptCannotHaveFreeFunctions)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, "using \"base.hot\";\nclass Kid : Base { }\n");
    o.modules.emplace_back("base.hot",
        "using \"w.h\";\nint Helper() { return 1; }\nclass Base : Widget { }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("an imported script cannot have free functions: 'Helper' is in 'base.hot'"),
        std::string::npos)
        << r.diagnostics;
}

// A static method of an imported class goes with its class: the output has its prototype and its body.
TEST(StaticFunctionTranslation, ImportedClassKeepsItsStaticMethod)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o =
        temp_options(dir, "using \"base.hot\";\nclass Kid : Base { int Use() { return Base.Make(2); } }\n");
    o.modules.emplace_back("base.hot",
        "using \"w.h\";\nclass Base : Widget { static int Make(int x) { return x + 1; } }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("static int32_t Base__s_Make(int32_t x);"), std::string::npos);
    EXPECT_NE(r.cpp.find("static int32_t Base__s_Make(int32_t x)\n"), std::string::npos);
}

// A function is not in the manifest. A prototype comes before the first body. The default argument
// is on the prototype only.
TEST(StaticFunctionTranslation, FunctionsHaveAPrototypeAndNoManifestEntry)
{
    const hotc::TranslateResult r = translate_script(
        "int Add(int a, int b = 4) { return a + b; }\n"
        "class A : Unit {\n"
        "    static int Make(int x) { return x + 1; }\n"
        "    int Use() { return A.Make(1) + Add(1); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t proto = r.cpp.find("static int32_t hot_fn_Add(int32_t a, int32_t b = 4);");
    const size_t body = r.cpp.find("static int32_t hot_fn_Add(int32_t a, int32_t b)\n");
    const size_t call = r.cpp.find("hot_fn_Add(1)");
    ASSERT_NE(proto, std::string::npos);
    ASSERT_NE(body, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(proto, call);
    EXPECT_LT(call, body);
    EXPECT_NE(r.cpp.find("static int32_t A__s_Make(int32_t x);"), std::string::npos);
    for (const char* name : {"Add", "Make", "hot_fn_", "__s_"})
        EXPECT_EQ(r.slots_out.find(name), std::string::npos) << name << "\n" << r.slots_out;
    EXPECT_EQ(r.cpp.find("hot_fn_Add\""), std::string::npos); // no table entry by name
}

TEST(StaticFunctionTranslation, ThisIsNotAllowedInAStaticBody)
{
    expect_error("class A : Unit {\n    int hp;\n    static int F() { this.hp = 1; return 0; }\n}\n",
        "'this' is not allowed in a static method");
    expect_error("class A : Unit {\n    static $A F() { return this; }\n}\n", "'this' is not allowed in a static method");
}

TEST(StaticFunctionTranslation, BareMembersNeedAnObject)
{
    expect_error("class A : Unit {\n    int hp;\n    static int F() { return hp; }\n}\n",
        "'hp' needs an object: 'F' is a static method");
    expect_error("class A : Unit {\n    int hp;\n    static void F() { hp = 2; }\n}\n",
        "'hp' needs an object: 'F' is a static method");
    expect_error("class A : Unit {\n    int Hp { get; set; }\n    static int F() { return Hp; }\n}\n",
        "'Hp' needs an object: 'F' is a static method");
    expect_error("class A : Unit {\n    void Inst() { }\n    static void F() { Inst(); }\n}\n",
        "'Inst' needs an object: 'F' is a static method");
}

TEST(StaticFunctionTranslation, StaticBodyCanUseAnotherObject)
{
    expect_ok("class A : Unit {\n    int hp;\n    int Hp { get; set; }\n    void Inst() { }\n"
              "    static int F($A other)\n    {\n        if (other == null)\n            return 0;\n"
              "        other.Inst();\n        return other.hp + other.Hp;\n    }\n}\n");
}

TEST(StaticFunctionTranslation, FreeFunctionHasNoThis)
{
    expect_error("int F() { return this.x; }\nclass A : Unit { }\n", "'this' is not allowed in a free function");
}

TEST(StaticFunctionTranslation, FreeFunctionBodyIsChecked)
{
    // The null check runs on the body of a free function and of a static method.
    expect_error("class A : Unit { int v; }\nint F($A a) { return a.v; }\n", "can be null here");
    expect_error("class A : Unit {\n    int v;\n    static int F($A a) { return a.v; }\n}\n", "can be null here");
}

TEST(StaticFunctionTranslation, InstanceCallOfAStaticIsAnError)
{
    expect_error("class A : Unit {\n    static int F() { return 1; }\n}\n"
                 "class B : Unit {\n    int G() { var a = new $A(); return a.F(); }\n}\n",
        "'F' is a static method; call it as 'A.F(...)'");
    expect_error("class A : Unit {\n    static int F() { return 1; }\n    int G() { return this.F(); }\n}\n",
        "'F' is a static method; call it as 'A.F(...)'");
}

TEST(StaticFunctionTranslation, FunctionNameWithoutACallIsAnError)
{
    expect_error("int F() { return 1; }\nclass A : Unit {\n    void G() { var f = F; }\n}\n",
        "'F' is a function; call it, or write a lambda");
    expect_error("class A : Unit {\n    static int F() { return 1; }\n}\n"
                 "class B : Unit {\n    void G() { var f = A.F; }\n}\n",
        "'F' is a function; call it, or write a lambda");
}

TEST(StaticFunctionTranslation, AccessOfAStaticIsChecked)
{
    expect_error("class A : Unit {\n    private static int F() { return 1; }\n}\n"
                 "class B : Unit {\n    int G() { return A.F(); }\n}\n",
        "'F' is private in 'A'");
    expect_ok("class A : Unit {\n    private static int F() { return 1; }\n    int G() { return A.F() + F(); }\n}\n");
}

TEST(StaticFunctionTranslation, ModuleInitializerCannotCallAFunction)
{
    expect_error("int F() { return 1; }\nint v = F();\nclass A : Unit { }\n",
        "a module variable initializer cannot call the function 'F': the module state is not complete yet");
    expect_error("class A : Unit {\n    static int F() { return 1; }\n}\nint v = A.F();\n",
        "a module variable initializer cannot call the function 'F': the module state is not complete yet");
}

TEST(StaticFunctionTranslation, ConvertHookCannotCallAFunction)
{
    expect_error("int F() { return 1; }\nint v;\nint convert_v(float old) { return F(); }\nclass A : Unit { }\n",
        "a convert hook cannot call the function 'F': the new module state is not complete yet");
}

TEST(StaticFunctionTranslation, FieldInitializerCannotCallAFunction)
{
    const std::string msg = "a field initializer cannot call the function 'F': ";
    expect_error("int F() { return 1; }\nclass A : Unit {\n    int v = F();\n}\n", msg);
    expect_error("class A : Unit {\n    static int F() { return 1; }\n    int v = F();\n}\n", msg);
    expect_error("class A : Unit {\n    static int F() { return 1; }\n}\n"
                 "class B : Unit {\n    int v = A.F();\n}\n",
        msg);
    expect_error("int F() { return 1; }\nstruct S {\n    int v = F();\n}\nclass A : Unit { }\n", msg);
}

TEST(StaticFunctionTranslation, DefaultArgumentCannotCallAFunction)
{
    const std::string msg = "a default argument cannot call the function 'F': ";
    expect_error("int F() { return 1; }\nclass A : Unit {\n    int M(int a = F()) { return a; }\n}\n", msg);
    expect_error("int F() { return 1; }\nint H(int a = F()) { return a; }\nclass A : Unit { }\n", msg);
    expect_error("class A : Unit {\n    static int F() { return 1; }\n"
                 "    static int S(int a = F()) { return a; }\n}\n",
        msg);
}

TEST(StaticFunctionTranslation, ErrorInInitializerHasNoWrongSymbol)
{
    const hotc::TranslateResult r =
        translate_script("int F() { return 1; }\nint H(int a = F()) { return a; }\nclass A : Unit { }\n");
    EXPECT_EQ(r.diagnostics.find("HotFns__s_F"), std::string::npos) << r.diagnostics;
}
