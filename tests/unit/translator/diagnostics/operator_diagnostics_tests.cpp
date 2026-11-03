// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the checks of the script operators (check_operators and the parser rules).
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

bool has(const hotc::TranslateResult& r, const std::string& text)
{
    return r.diagnostics.find(text) != std::string::npos;
}

// The members go into "struct V { ... }".
hotc::TranslateResult in_struct(const std::string& members)
{
    return translate_text("struct V {\n    int x;\n" + members + "}\n");
}

} // namespace

TEST(OperatorDiagnostics, ValidOperatorsHaveNoError)
{
    const hotc::TranslateResult r = in_struct(
        "    V operator+(in V o) { return this; }\n"
        "    V operator+() { return this; }\n"
        "    V operator-() { return this; }\n"
        "    V operator-(in V o) { return this; }\n"
        "    V operator<<(in int n) { return this; }\n"
        "    bool operator==(in V o) { return true; }\n"
        "    bool operator<(in V o) { return true; }\n"
        "    bool operator>(in V o) { return true; }\n"
        "    bool operator<=(in V o) { return true; }\n"
        "    bool operator>=(in V o) { return true; }\n"
        "    int operator[](in int i) { return i; }\n"
        "    bool operator!() { return true; }\n"
        "    V operator~() { return this; }\n");
    EXPECT_EQ(count_errors(r.diagnostics), 0) << r.diagnostics;
}

TEST(OperatorDiagnostics, ValidOperatorInClass)
{
    const hotc::TranslateResult r = translate_text(
        "class C {\n    int x;\n    int operator+(in int o) { return o; }\n}\n");
    EXPECT_EQ(count_errors(r.diagnostics), 0) << r.diagnostics;
}

TEST(OperatorDiagnostics, SymbolNotAllowed)
{
    for (const char* sym : {"&&", "||", "++", "--", "->", "()", "<=>", "="})
    {
        const hotc::TranslateResult r = in_struct(std::string("    V operator") + sym + "(in V o) { return this; }\n");
        EXPECT_TRUE(has(r, std::string("operator '") + sym + "' cannot be declared in a script")) << sym << r.diagnostics;
    }
}

TEST(OperatorDiagnostics, NotEqualIsGenerated)
{
    const hotc::TranslateResult r = in_struct("    bool operator!=(in V o) { return true; }\n");
    EXPECT_TRUE(has(r, "operator '!=' is generated from 'operator=='; remove it")) << r.diagnostics;
}

TEST(OperatorDiagnostics, CompoundAssignmentIsGenerated)
{
    const char* syms[] = {"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    for (const char* sym : syms)
    {
        const std::string s = sym;
        const hotc::TranslateResult r = in_struct("    V operator" + s + "(in V o) { return this; }\n");
        EXPECT_TRUE(has(r, "operator '" + s + "' is generated from 'operator" + s.substr(0, s.size() - 1) + "'; remove it"))
            << s << r.diagnostics;
    }
}

TEST(OperatorDiagnostics, BinaryTakesOneParameter)
{
    const hotc::TranslateResult r = in_struct(
        "    V operator*() { return this; }\n"
        "    V operator/(in V a, in V b) { return this; }\n"
        "    int operator[]() { return 1; }\n");
    EXPECT_TRUE(has(r, "operator '*' takes one parameter")) << r.diagnostics;
    EXPECT_TRUE(has(r, "operator '/' takes one parameter")) << r.diagnostics;
    EXPECT_TRUE(has(r, "operator '[]' takes one parameter")) << r.diagnostics;
}

TEST(OperatorDiagnostics, UnaryTakesNoParameter)
{
    const hotc::TranslateResult r = in_struct(
        "    bool operator!(in V o) { return true; }\n"
        "    V operator~(in V o) { return this; }\n");
    EXPECT_TRUE(has(r, "operator '!' takes no parameter")) << r.diagnostics;
    EXPECT_TRUE(has(r, "operator '~' takes no parameter")) << r.diagnostics;
}

TEST(OperatorDiagnostics, PlusAndMinusTakeAtMostOneParameter)
{
    const hotc::TranslateResult r = in_struct("    V operator-(in V a, in V b) { return this; }\n");
    EXPECT_TRUE(has(r, "operator '-' takes no parameter or one parameter")) << r.diagnostics;
}

TEST(OperatorDiagnostics, CompareMustReturnBool)
{
    const hotc::TranslateResult r = in_struct(
        "    int operator==(in V o) { return 1; }\n"
        "    void operator<(in V o) { }\n"
        "    int operator>(in V o) { return 1; }\n"
        "    int operator<=(in V o) { return 1; }\n"
        "    int operator>=(in V o) { return 1; }\n");
    for (const char* sym : {"==", "<", ">", "<=", ">="})
        EXPECT_TRUE(has(r, std::string("operator '") + sym + "' must return bool")) << sym << r.diagnostics;
}

TEST(OperatorDiagnostics, IndexCannotReturnVoid)
{
    const hotc::TranslateResult r = in_struct("    void operator[](in int i) { }\n");
    EXPECT_TRUE(has(r, "operator '[]' cannot return void")) << r.diagnostics;
}

TEST(OperatorDiagnostics, OrderingPair)
{
    const char* sides[][2] = {{"<", ">"}, {">", "<"}, {"<=", ">="}, {">=", "<="}};
    for (const auto& p : sides)
    {
        const hotc::TranslateResult r = in_struct(std::string("    bool operator") + p[0] + "(in V o) { return true; }\n");
        EXPECT_TRUE(has(r, std::string("operator '") + p[0] + "' needs 'operator" + p[1] + "'")) << r.diagnostics;
    }
}

TEST(OperatorDiagnostics, ModifiersAreErrors)
{
    for (const char* mod : {"static", "virtual", "override", "abstract", "async"})
    {
        const hotc::TranslateResult r =
            in_struct(std::string("    ") + mod + " V operator+(in V o) { return this; }\n");
        EXPECT_TRUE(has(r, std::string("an operator cannot be '") + mod + "'")) << mod << r.diagnostics;
    }
}

TEST(OperatorDiagnostics, InterfaceHasNoOperator)
{
    const hotc::TranslateResult r = translate_text("interface I {\n    I operator+(in I o);\n}\n");
    EXPECT_TRUE(has(r, "an operator needs a class or a struct")) << r.diagnostics;
}

TEST(OperatorDiagnostics, ModuleLevelHasNoOperator)
{
    const hotc::TranslateResult r = translate_text("int operator+(in int o) { return o; }\n");
    EXPECT_TRUE(has(r, "an operator needs a class or a struct")) << r.diagnostics;
}

TEST(OperatorDiagnostics, CallByIdentifier)
{
    const hotc::TranslateResult r = translate_text(
        "struct V {\n"
        "    int x;\n"
        "    V operator+(in V o) { return this; }\n"
        "}\n"
        "class C {\n"
        "    void f() {\n"
        "        V a;\n"
        "        V b;\n"
        "        var c = a.op_add(b);\n"
        "    }\n"
        "}\n");
    EXPECT_TRUE(has(r, "'op_add' is an operator; write 'a + b'")) << r.diagnostics;
}

TEST(OperatorDiagnostics, DuplicateOperator)
{
    const hotc::TranslateResult r = in_struct(
        "    V operator+(in V o) { return this; }\n"
        "    V operator+(in V o) { return o; }\n");
    EXPECT_TRUE(has(r, "operator '+' is declared twice (overloads are not supported)")) << r.diagnostics;
}

TEST(OperatorDiagnostics, BinaryOperatorCannotReturnVoid)
{
    const hotc::TranslateResult r = in_struct("    void operator+(in V o) { }\n");
    EXPECT_TRUE(has(r, "operator '+' cannot return void")) << r.diagnostics;
}

TEST(OperatorDiagnostics, UnaryOperatorCannotReturnVoid)
{
    const hotc::TranslateResult r = in_struct("    void operator-() { }\n");
    EXPECT_TRUE(has(r, "operator '-' cannot return void")) << r.diagnostics;
}

TEST(OperatorDiagnostics, OperatorIdentifierIsReservedForAMethod)
{
    const hotc::TranslateResult r = in_struct("    V op_add(in V o) { return this; }\n");
    EXPECT_TRUE(has(r, "'op_add' is reserved for an operator; write 'operator+'")) << r.diagnostics;
}

TEST(OperatorDiagnostics, OperatorIdentifierIsReservedInAClass)
{
    const hotc::TranslateResult r = translate_text("class C {\n    int op_index(int i) { return i; }\n}\n");
    EXPECT_TRUE(has(r, "'op_index' is reserved for an operator; write 'operator[]'")) << r.diagnostics;
}

TEST(OperatorDiagnostics, CallByIdentifierInAFieldInitializer)
{
    const hotc::TranslateResult r = translate_text(
        "struct V {\n"
        "    int x;\n"
        "    V operator+(in V o) { return this; }\n"
        "}\n"
        "class C {\n"
        "    V a;\n"
        "    int y = a.op_add(a).x;\n"
        "}\n");
    EXPECT_TRUE(has(r, "'op_add' is an operator; write 'a + b'")) << r.diagnostics;
}

TEST(OperatorDiagnostics, CallByIdentifierInAModuleVariable)
{
    const hotc::TranslateResult r = translate_text(
        "struct V {\n"
        "    int x;\n"
        "    V operator+(in V o) { return this; }\n"
        "}\n"
        "V g;\n"
        "int y = g.op_add(g).x;\n");
    EXPECT_TRUE(has(r, "'op_add' is an operator; write 'a + b'")) << r.diagnostics;
}

TEST(OperatorDiagnostics, ConstOperatorCannotWriteAField)
{
    const hotc::TranslateResult r = in_struct("    V operator+(in V o) { x = o.x; return this; }\n");
    EXPECT_TRUE(has(r, "operator '+' cannot change the object; return a new value instead")) << r.diagnostics;
}

TEST(OperatorDiagnostics, ConstOperatorCannotChangeAFieldWithACompoundOrIncrement)
{
    const hotc::TranslateResult r = in_struct(
        "    V operator-(in V o) { this.x += o.x; return this; }\n"
        "    V operator*(in V o) { x++; return this; }\n");
    EXPECT_TRUE(has(r, "operator '-' cannot change the object; return a new value instead")) << r.diagnostics;
    EXPECT_TRUE(has(r, "operator '*' cannot change the object; return a new value instead")) << r.diagnostics;
}

TEST(OperatorDiagnostics, CompareCannotWriteAField)
{
    const hotc::TranslateResult r = in_struct("    bool operator==(in V o) { x = 0; return true; }\n");
    EXPECT_TRUE(has(r, "operator '==' cannot change the object; return a new value instead")) << r.diagnostics;
}

TEST(OperatorDiagnostics, ConstOperatorCannotCallAFunctionThatWritesAField)
{
    const hotc::TranslateResult r = in_struct(
        "    void reset() { x = 0; }\n"
        "    V operator!() { reset(); return this; }\n");
    EXPECT_TRUE(has(r, "operator '!' cannot change the object; return a new value instead")) << r.diagnostics;
}

TEST(OperatorDiagnostics, OperatorMayWriteALocalAndCallAReadingFunction)
{
    const hotc::TranslateResult r = in_struct(
        "    int peek() { return x; }\n"
        "    int operator[](in int i) { int t = x; t += i; return t + peek(); }\n");
    EXPECT_EQ(count_errors(r.diagnostics), 0) << r.diagnostics;
}

TEST(OperatorDiagnostics, RefIndexOperatorMayWrite)
{
    const hotc::TranslateResult r = in_struct("    ref int operator[](in int i) { x = i; return x; }\n");
    EXPECT_EQ(count_errors(r.diagnostics), 0) << r.diagnostics;
}
