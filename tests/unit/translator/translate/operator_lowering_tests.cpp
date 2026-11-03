// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the use of operators on bare class values (boxes). The operator
// of a class gives the type of the expression. The box "==" needs the operator of the class. An
// operator on a handle is an error.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

hotc::TranslateResult translate_classes(const std::string& body)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    return hotc::translate(temp_options(*dir, body));
}

// The class Money has the operators. The class Box has the field "x" and no operator.
const char* kClasses =
    "class Money {\n"
    "    int cents = 0;\n"
    "    Money(int c) { this.cents = c; }\n"
    "    Money operator+(in Money o) { return new Money(this.cents + o.cents); }\n"
    "    Money operator-() { return new Money(-this.cents); }\n"
    "    bool operator==(in Money o) { return this.cents == o.cents; }\n"
    "    bool operator<(in Money o) { return this.cents < o.cents; }\n"
    "    bool operator>(in Money o) { return this.cents > o.cents; }\n"
    "}\n"
    "class Cell {\n"
    "    int x = 0;\n"
    "}\n"
    "class Row {\n"
    "    int n = 0;\n"
    "    Cell operator[](int i) { return new Cell(); }\n"
    "}\n";

hotc::TranslateResult translate_use(const std::string& body)
{
    return translate_classes(std::string(kClasses) + "class User {\n" + body + "}\n");
}

} // namespace

TEST(OperatorLowering, MemberOfAnOperatorResultUsesTheResultType)
{
    const hotc::TranslateResult r = translate_use(
        "    int F(Money a, Money b) { return (a + b).cents; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::shell_of<Money_Shell>((a + b), \"Money\")")) << r.cpp;
}

TEST(OperatorLowering, MemberOfAChainResultUsesTheResultType)
{
    const hotc::TranslateResult r = translate_use(
        "    int F(Money a, Money b) { return (a + b + a).cents; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(OperatorLowering, VarOfAnOperatorResultHoldsABox)
{
    const hotc::TranslateResult r = translate_use(
        "    int F(Money a, Money b) { var c = a + b; return c.cents; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const auto c = a + b;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::shell_of<Money_Shell>(c, \"Money\")")) << r.cpp;
}

TEST(OperatorLowering, TypedLocalOfAnOperatorResultDoesNotCopyTheBoxAgain)
{
    const hotc::TranslateResult r = translate_use(
        "    int F(Money a, Money b) { Money c = a + b; return c.cents; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const hot::Value<Money> c = a + b;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "as_value")) << r.cpp;
}

TEST(OperatorLowering, AssignmentOfAnOperatorResultDoesNotCopyTheBoxAgain)
{
    const hotc::TranslateResult r = translate_use(
        "    void F(Money a, Money b) { Money c = new Money(1); c = a + b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "c = a + b;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "as_value")) << r.cpp;
}

TEST(OperatorLowering, UnaryResultIsABox)
{
    const hotc::TranslateResult r = translate_use(
        "    int F(Money a) { var n = -a; return n.cents; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const auto n = -a;")) << r.cpp;
}

TEST(OperatorLowering, BoxEqualIsAllowedWhenTheClassWritesEqual)
{
    const hotc::TranslateResult r = translate_use(
        "    bool F(Money a, Money b) { return a == b; }\n"
        "    bool G(Money a, Money b) { return a != b; }\n"
        "    bool H(Money a, Money b) { return a < b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return a == b;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return a != b;")) << r.cpp;
}

TEST(OperatorLowering, CompareResultIsABool)
{
    const hotc::TranslateResult r = translate_use(
        "    bool F(Money a, Money b) { var e = a == b; return e; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "e = a == b;")) << r.cpp;
}

TEST(OperatorLowering, BoxEqualStaysAnErrorWithoutTheOperator)
{
    const hotc::TranslateResult r = translate_use(
        "    bool F(Cell a, Cell b) { return a == b; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'==' is not valid for a value of a script class"), std::string::npos)
        << r.diagnostics;
}

TEST(OperatorLowering, BoxNotEqualStaysAnErrorWithoutTheOperator)
{
    const hotc::TranslateResult r = translate_use(
        "    bool F(Cell a, Cell b) { return a != b; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'!=' is not valid for a value of a script class"), std::string::npos)
        << r.diagnostics;
}

TEST(OperatorLowering, IndexResultUsesTheReturnTypeOfTheOperator)
{
    const hotc::TranslateResult r = translate_use(
        "    int F(Row v) { return v[0].x; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::shell_of<Cell_Shell>(hot::index(v, 0), \"Cell\")")) << r.cpp;
}

TEST(OperatorLowering, HandleIdentityCompareStays)
{
    const hotc::TranslateResult r = translate_use(
        "    bool F($Money a, $Money b) { return a == b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(OperatorLowering, BinaryOperatorOnAHandleIsAnError)
{
    const hotc::TranslateResult r = translate_use(
        "    Money F($Money h, Money x) { return h + x; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("operator '+' needs a value, not a handle"), std::string::npos) << r.diagnostics;
}

TEST(OperatorLowering, CompareOperatorOnAHandleIsAnError)
{
    const hotc::TranslateResult r = translate_use(
        "    bool F($Money h, Money x) { return h < x; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("operator '<' needs a value, not a handle"), std::string::npos) << r.diagnostics;
}

TEST(OperatorLowering, UnaryOperatorOnAHandleIsAnError)
{
    const hotc::TranslateResult r = translate_use(
        "    Money F($Money h) { return -h; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("operator '-' needs a value, not a handle"), std::string::npos) << r.diagnostics;
}

TEST(OperatorLowering, IndexOnAHandleIsAnError)
{
    const hotc::TranslateResult r = translate_use(
        "    Cell F($Row h) { if (h != null) { return h[0]; } return new Cell(); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("operator '[]' needs a value, not a handle"), std::string::npos) << r.diagnostics;
}

namespace
{

bool has_error(const hotc::TranslateResult& r, const std::string& text)
{
    return !r.ok && r.diagnostics.find(text) != std::string::npos;
}

const char* kInherit =
    "class Base {\n"
    "    int n = 0;\n"
    "    Base operator+(in Base o) { return new Base(); }\n"
    "}\n"
    "class Derived : Base {\n"
    "}\n"
    "class Plain {\n"
    "    int n = 0;\n"
    "}\n";

const char* kStruct =
    "struct Pt {\n"
    "    int x;\n"
    "    Pt operator+(in Pt o) { return this; }\n"
    "    Pt operator-() { return this; }\n"
    "}\n";

hotc::TranslateResult translate_with(const char* classes, const std::string& body)
{
    return translate_classes(std::string(classes) + "class User {\n" + body + "}\n");
}

} // namespace

TEST(OperatorLowering, BoxEqualToNullKeepsTheScriptError)
{
    const hotc::TranslateResult r = translate_use("    bool F(Money a) { return a == null; }\n");
    EXPECT_TRUE(has_error(r, "'==' is not valid for a value of a script class")) << r.diagnostics;
}

TEST(OperatorLowering, BoxEqualToAHandleKeepsTheScriptError)
{
    const hotc::TranslateResult r = translate_use("    bool F(Money a, $Money h) { return a == h; }\n");
    EXPECT_TRUE(has_error(r, "'==' is not valid for a value of a script class")) << r.diagnostics;
}

TEST(OperatorLowering, BoxNotEqualToNullKeepsTheScriptError)
{
    const hotc::TranslateResult r = translate_use("    bool F(Money a) { return a != null; }\n");
    EXPECT_TRUE(has_error(r, "'!=' is not valid for a value of a script class")) << r.diagnostics;
}

TEST(OperatorLowering, BoxEqualToAnOtherClassKeepsTheScriptError)
{
    const hotc::TranslateResult r = translate_use("    bool F(Money a, Cell c) { return a == c; }\n");
    EXPECT_TRUE(has_error(r, "'==' is not valid for a value of a script class")) << r.diagnostics;
}

TEST(OperatorLowering, BoxCompareToAHandleIsAnError)
{
    const hotc::TranslateResult r = translate_use("    bool F(Money a, $Money h) { return a < h; }\n");
    EXPECT_TRUE(has_error(r, "operator '<' needs a value, not a handle")) << r.diagnostics;
}

TEST(OperatorLowering, BoxPlusNullIsAnError)
{
    const hotc::TranslateResult r = translate_use("    Money F(Money a) { return a + null; }\n");
    EXPECT_TRUE(has_error(r, "operator '+' needs a value, not null")) << r.diagnostics;
}

TEST(OperatorLowering, CompoundOnAnInheritedOperatorIsAnError)
{
    const hotc::TranslateResult r = translate_with(kInherit, "    void F(Derived d, Base x) { d += x; }\n");
    EXPECT_TRUE(has_error(r, "operator '+=' is not available on 'Derived': 'operator+' comes from the parent 'Base'"))
        << r.diagnostics;
}

TEST(OperatorLowering, CompoundOnTheDeclaringClassIsValid)
{
    const hotc::TranslateResult r = translate_with(kInherit, "    void F(Base b, Base x) { b += x; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(OperatorLowering, PlusOnAnInheritedOperatorIsValid)
{
    const hotc::TranslateResult r = translate_with(kInherit, "    Base F(Derived d, Base x) { return d + x; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(OperatorLowering, CompoundWithoutTheBinaryOperatorIsAnError)
{
    const hotc::TranslateResult r = translate_with(kInherit, "    void F(Plain p) { p += p; }\n");
    EXPECT_TRUE(has_error(r, "operator '+=' needs 'operator+' in 'Plain'")) << r.diagnostics;
}

TEST(OperatorLowering, CompoundNeedsTheBinaryOperatorToReturnTheClass)
{
    const hotc::TranslateResult r = translate_with(
        "class Dist {\n    int n = 0;\n    int operator-(in Dist o) { return 1; }\n}\n",
        "    void F(Dist a, Dist b) { a -= b; }\n");
    EXPECT_TRUE(has_error(r, "operator '-=' needs 'operator-' of 'Dist' to return 'Dist'")) << r.diagnostics;
}

TEST(OperatorLowering, CompoundOnAHandleIsAnError)
{
    const hotc::TranslateResult r = translate_use("    void F($Money h, Money b) { h += b; }\n");
    EXPECT_TRUE(has_error(r, "operator '+=' needs a value, not a handle")) << r.diagnostics;
}

TEST(OperatorLowering, CompoundOnAnOptionalIsAnError)
{
    const hotc::TranslateResult r = translate_with(kStruct, "    void F(Pt? a, Pt b) { a += b; }\n");
    EXPECT_TRUE(has_error(r, "operator '+=' needs a value, not an optional")) << r.diagnostics;
}

TEST(OperatorLowering, OperatorOnAnOptionalIsAnError)
{
    const hotc::TranslateResult r = translate_with(kStruct, "    Pt F(Pt? a, Pt b) { return a + b; }\n");
    EXPECT_TRUE(has_error(r, "operator '+' needs a value, not an optional")) << r.diagnostics;
}

TEST(OperatorLowering, UnaryOperatorOnAnOptionalIsAnError)
{
    const hotc::TranslateResult r = translate_with(kStruct, "    Pt F(Pt? a) { return -a; }\n");
    EXPECT_TRUE(has_error(r, "operator '-' needs a value, not an optional")) << r.diagnostics;
}

TEST(OperatorLowering, AssignToAnIndexThatReturnsAValueIsAnError)
{
    const hotc::TranslateResult r = translate_use("    void F(Row r) { r[0] = new Cell(); }\n");
    EXPECT_TRUE(has_error(r, "operator '[]' must return 'ref T' to be assigned")) << r.diagnostics;
}

TEST(OperatorLowering, CompoundAndIncrementOfAnIndexThatReturnsAValueAreErrors)
{
    const char* seq = "class Seq {\n    int operator[](int i) { return i; }\n}\n";
    const hotc::TranslateResult a = translate_with(seq, "    void F(Seq s) { s[0] += 1; }\n");
    EXPECT_TRUE(has_error(a, "operator '[]' must return 'ref T' to be assigned")) << a.diagnostics;
    const hotc::TranslateResult b = translate_with(seq, "    void F(Seq s) { s[0]++; }\n");
    EXPECT_TRUE(has_error(b, "operator '[]' must return 'ref T' to be assigned")) << b.diagnostics;
    const hotc::TranslateResult c = translate_with(seq, "    int F(Seq s) { return s[0]; }\n");
    EXPECT_TRUE(c.ok) << c.diagnostics;
}

TEST(OperatorLowering, AssignToARefIndexIsValid)
{
    const hotc::TranslateResult r = translate_classes(
        "struct Pair {\n"
        "    int[2] data;\n"
        "    ref int operator[](int i) { return data[i]; }\n"
        "}\n"
        "class User {\n"
        "    void F(Pair p) { p[0] = 1; p[1] += 2; }\n"
        "}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A borrowed "in" parameter has no owner of its own (hot::borrow_value). The script cannot make a
// handle or a weak handle from a box, so no code reaches the empty owner.
TEST(OperatorLowering, InParameterOfAnOperatorCannotBecomeAHandle)
{
    const hotc::TranslateResult r = translate_classes(
        "class Node {\n"
        "    int n = 0;\n"
        "    Node operator+(in Node o) { $Node h = o; return new Node(); }\n"
        "}\n");
    EXPECT_TRUE(has_error(r, "a value cannot convert to a handle")) << r.diagnostics;
}

TEST(OperatorLowering, InParameterOfAnOperatorCannotBecomeAWeakHandle)
{
    const hotc::TranslateResult r = translate_classes(
        "class Node {\n"
        "    int n = 0;\n"
        "    Node operator+(in Node o) { [weak] $Node w = o; return new Node(); }\n"
        "}\n");
    EXPECT_FALSE(r.ok) << r.cpp;
}
