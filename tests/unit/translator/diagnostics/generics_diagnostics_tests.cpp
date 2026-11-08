// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Script generics (doc/generics.md): the translation of the instantiations and the errors of the
// forms that version 1 does not support. Each unsupported form gives an error, never C++ code.
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

// The translation fails with one error that has the text "what" at "where" ("t.hot:L:C").
void expect_error(const std::string& script, const std::string& what, const std::string& where)
{
    const hotc::TranslateResult r = translate_text(script);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find(what), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find(where + ": error: " + what), std::string::npos) << r.diagnostics;
}

TEST(GenericsTranslation, EachUseGivesOnePlainDeclaration)
{
    const hotc::TranslateResult r = translate_text(
        "struct Box<T> { T value; T Get() => value; }\n"
        "class Stack<T> { std.vector<T> items = std.vector<T>(); void Push(T v) { items.push_back(v); } }\n"
        "T First<T>(T a, T b) => a;\n"
        "int Use() {\n"
        "    Box<int> a; Box<int> b; Box<std.string> s;\n"
        "    var st = new $Stack<float>();\n"
        "    st.Push(1.5);\n"
        "    return First<int>(a.Get(), b.value);\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("struct Box_G_int {"), std::string::npos);
    EXPECT_NE(r.cpp.find("struct Box_G_std_string {"), std::string::npos);
    EXPECT_NE(r.cpp.find("Stack_G_float_Shell"), std::string::npos);
    EXPECT_NE(r.cpp.find("hot_fn_First_G_int("), std::string::npos);
    // One instantiation for each set of type arguments. The generic itself gives no code.
    EXPECT_EQ(r.cpp.find("struct Box_G_int {", r.cpp.find("struct Box_G_int {") + 1), std::string::npos);
    EXPECT_EQ(r.cpp.find("struct Box {"), std::string::npos);
    EXPECT_EQ(r.cpp.find("hot_fn_First("), std::string::npos);
}

TEST(GenericsTranslation, NestedUsesAndAliasesInstantiate)
{
    const hotc::TranslateResult r = translate_text(
        "struct Pair<A, B> { A a; B b; }\n"
        "struct Box<T> { T value; Pair<T, T> twice; }\n"
        "using IntBox = Box<int>;\n"
        "int Use() { IntBox b; Box<Box<float>> n; return b.twice.a; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("struct Pair_G_int_G_int {"), std::string::npos);
    EXPECT_NE(r.cpp.find("struct Box_G_Box_G_float {"), std::string::npos);
    EXPECT_NE(r.cpp.find("struct Pair_G_float_G_float {"), std::string::npos);
}

TEST(GenericsTranslation, UnusedGenericGivesNoCodeButItsSyntaxIsChecked)
{
    const hotc::TranslateResult ok = translate_text("struct Box<T> { T value; }\nint F() => 1;\n");
    ASSERT_TRUE(ok.ok) << ok.diagnostics;
    EXPECT_EQ(ok.cpp.find("Box"), std::string::npos);
    expect_error("struct Box<T> { T value; void F() { var x = ; } }\n", "expected an expression", "t.hot:1:45");
}

TEST(GenericsDiagnostics, BareGenericTypeNeedsTypeArguments)
{
    expect_error("struct Box<T> { T value; }\nint F() { Box b; return 0; }\n",
        "the generic type 'Box' needs type arguments: write 'Box<...>'", "t.hot:2:11");
}

TEST(GenericsDiagnostics, GenericFunctionNeedsExplicitTypeArguments)
{
    expect_error("T First<T>(T a) => a;\nint F() => First(1);\n",
        "the generic function 'First' needs explicit type arguments: write 'First<...>(...)'", "t.hot:2:12");
}

TEST(GenericsDiagnostics, WrongCountOfTypeArguments)
{
    expect_error("struct Box<T> { T value; }\nint F() { Box<int, float> b; return 0; }\n",
        "the generic 'Box' has 1 type parameter, not 2", "t.hot:2:11");
}

TEST(GenericsDiagnostics, GenericMethodIsAnError)
{
    expect_error("class C { int F<T>(T a) { return 1; } }\n",
        "a generic method is not supported. Use a generic free function or a generic class", "t.hot:1:15");
}

TEST(GenericsDiagnostics, GenericInterfaceAndEnumAreErrors)
{
    expect_error("interface I<T> { void F(); }\n",
        "a generic interface is not supported. Only a struct, a class and a free function can be generic", "t.hot:1:11");
    expect_error("enum E<T> { A }\n",
        "a generic enum is not supported. Only a struct, a class and a free function can be generic", "t.hot:1:6");
}

TEST(GenericsDiagnostics, TypeParameterIsAPlainName)
{
    expect_error("struct Arr<int N> { int x; }\n", "a type parameter of a generic is a plain name: 'struct Box<T>'", "t.hot:1:11");
    expect_error("struct Box<int> { int x; }\n",
        "the type parameter 'int' of 'Box' is a type of the language. Use another name", "t.hot:1:8");
    expect_error("struct Box<T, T> { T x; }\n", "the type parameter 'T' of 'Box' is a duplicate", "t.hot:1:8");
}

TEST(GenericsDiagnostics, MainCannotBeGeneric)
{
    expect_error("int main<T>() { return 0; }\n", "the function 'main' cannot be generic", "t.hot:1:5");
}

TEST(GenericsDiagnostics, GenericNameMustBeUnique)
{
    expect_error("struct Box<T> { T x; }\nstruct Box<U> { U y; }\n", "duplicate generic 'Box'", "t.hot:2:8");
    expect_error("class Box { int x; }\nstruct Box<T> { T x; }\n",
        "the generic 'Box' has the name of another declaration of the module", "t.hot:2:8");
}

TEST(GenericsDiagnostics, GeneratedNameMustNotHitADeclaration)
{
    expect_error("struct Box<T> { T x; }\nstruct Box_G_int { int y; }\nint F() { Box<int> b; return 0; }\n",
        "the instantiation 'Box<int>' gets the C++ name 'Box_G_int', and a declaration of the module has this name",
        "t.hot:3:11");
}

TEST(GenericsDiagnostics, EndlessInstantiationStops)
{
    const std::string script = "struct Rec<T> { std.vector<Rec<Rec<T>>> r; }\nint F() { Rec<int> r; return 0; }\n";
    expect_error(script, "the instantiation 'Rec<Rec<Rec<", "t.hot:1:1");
    EXPECT_NE(translate_text(script).diagnostics.find("is too deep (more than 16 levels). A generic uses itself with a larger type argument"),
        std::string::npos);
}

TEST(GenericsDiagnostics, NativeStubTemplateIsNotAGeneric)
{
    // "class Name<*>;" stays a stub of a native template.
    const hotc::TranslateResult r = translate_text("[handle(\"std::shared_ptr<$T>\")] class Foo<*>;\nint F() => 1;\n");
    EXPECT_EQ(r.diagnostics.find("generic"), std::string::npos) << r.diagnostics;
}

} // namespace
