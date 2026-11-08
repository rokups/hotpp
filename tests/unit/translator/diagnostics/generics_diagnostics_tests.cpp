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

// The number of errors in the text of the diagnostics.
size_t error_count(const std::string& diagnostics)
{
    size_t n = 0;
    for (size_t at = diagnostics.find(": error: "); at != std::string::npos; at = diagnostics.find(": error: ", at + 1))
        ++n;
    return n;
}

TEST(GenericsDiagnostics, ConstraintsAreErrors)
{
    const std::string what = "a constraint on a type parameter is not supported";
    const struct
    {
        const char* script;
        const char* where;
    } cases[] = {
        {"struct Box<T> where T : Shape { T v; }\nint F() { Box<int> b; return 0; }\n", "t.hot:1:15"},
        {"class Bag<T : Shape> { T v; }\nint F() => 1;\n", "t.hot:1:10"},
        {"T Pick<T : Shape>(T x) => x;\nint F() => Pick<int>(1);\n", "t.hot:1:7"},
        {"T Pick<T>(T x) where T : Shape => x;\nint F() => Pick<int>(1);\n", "t.hot:1:16"},
    };
    for (const auto& c : cases)
    {
        SCOPED_TRACE(c.script);
        expect_error(c.script, what, c.where);
        // The scan blanks the constraint: the parse gives no second error.
        EXPECT_EQ(error_count(translate_text(c.script).diagnostics), 1u);
    }
    // A generic method gives one error. A constraint after it gives one more error, and no parse error.
    const std::string method = "class C { int Get<T>() where T : Shape => 1; }\nint F() => 1;\n";
    EXPECT_EQ(error_count(translate_text(method).diagnostics), 2u) << translate_text(method).diagnostics;
    EXPECT_EQ(error_count(translate_text("class C { int Get<T>() => 1; }\nint F() => 1;\n").diagnostics), 1u);
}

TEST(GenericsNames, RunTimeFramesShowTheScriptName)
{
    const hotc::TranslateResult r = translate_text(
        "class Node<T> { T v; T Get() => v; }\n"
        "T First<T>(T x) => x;\n"
        "int Use() { var n = new $Node<int>(); return First<int>(n.Get()); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The traceback frames and the stale call text have the script name.
    EXPECT_NE(r.cpp.find("HOT_TRACE_FRAME(\"Node<int>.Get\""), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("HOT_TRACE_FRAME(\"First<int>\""), std::string::npos);
    EXPECT_NE(r.cpp.find("hot::stale_call(\"Node<int>::Get\")"), std::string::npos);
    // The registry keys keep the C++ name.
    EXPECT_NE(r.cpp.find("\"Node_G_int::Get\""), std::string::npos);
}

TEST(GenericsNames, CompilerMessagesShowTheScriptName)
{
    const hotc::TranslateResult r = translate_text("struct Box<T> { T value; }\nint Use() { Box<int> b; return b.value; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const hotc::SourceMap::GenericNames& names = r.source_map.generic_names();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0].first, "Box_G_int");
    EXPECT_EQ(names[0].second, "Box<int>");
    // A Clang message that names the instantiation gets the script name. A longer name stays.
    const std::string out = hotc::rewrite_diagnostic(
        "x.cpp:3:1: error: no member named 'size' in 'Box_G_int'\nnote: 'Box_G_int2' and 'Box_G_int'\n", {}, {},
        &r.source_map);
    EXPECT_NE(out.find("'Box<int>' has no member 'size'"), std::string::npos) << out;
    EXPECT_NE(out.find("'Box_G_int2' and 'Box<int>'"), std::string::npos) << out;
    EXPECT_EQ(hotc::show_generic_names("Box_G_int__Get Box_G_int", names), "Box_G_int__Get Box<int>");
}

} // namespace
