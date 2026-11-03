// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the operators of a script class. The tests check the text of the
// members of the shell and the form of the "in C" parameter and of the "C" return.
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

const char* kMoney =
    "class Money {\n"
    "    int cents = 0;\n"
    "    Money(int c) { this.cents = c; }\n"
    "    Money operator+(in Money o) { return new Money(this.cents + o.cents); }\n"
    "    Money operator*(int f) { return new Money(this.cents * f); }\n"
    "    int operator-(in Money o) { return this.cents - o.cents; }\n"
    "    bool operator==(in Money o) { return this.cents == o.cents; }\n"
    "    bool operator<(in Money o) { return this.cents < o.cents; }\n"
    "    bool operator>(in Money o) { return this.cents > o.cents; }\n"
    "}\n";

} // namespace

TEST(OperatorClassTranslation, InParameterOfAClassIsAcceptedAsABox)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Value<Money> op_add(hot::In<hot::Value<Money>> o);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool op_eq(hot::In<hot::Value<Money>> o);")) << r.cpp;
}

TEST(OperatorClassTranslation, InParameterOfAClassIsAcceptedInAnOrdinaryMethod)
{
    const hotc::TranslateResult r = translate_classes(
        "class Pt {\n    int x = 1;\n    int Diff(in Pt o) { return this.x - o.x; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t Diff(hot::In<hot::Value<Pt>> o);")) << r.cpp;
}

TEST(OperatorClassTranslation, RefParameterOfAClassIsStillAnError)
{
    const hotc::TranslateResult r = translate_classes(
        "class Pt {\n    int x = 1;\n    void Fix(ref Pt o) { o.x = 2; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("a reference to a value of a script class is not supported; write $Pt"), std::string::npos)
        << r.diagnostics;
}

TEST(OperatorClassTranslation, ShellMemberUsesTheCxxOperatorName)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Value<Money> operator+(const Money& o) const")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::Value<Money> operator*(int32_t f) const")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t operator-(const Money& o) const")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool operator<(const Money& o) const")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool operator>(const Money& o) const")) << r.cpp;
}

TEST(OperatorClassTranslation, ShellMemberReadsTheSlotAndBorrowsTheArgument)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp,
        "return hot::call<hot::Value<Money>(hot::PlainObject*, hot::In<hot::Value<Money>>)>(const_cast<Money_Shell*>(this), "
        "SLOT_op_add, hot::borrow_value(o));"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SLOT_op_mul, f);")) << r.cpp;
}

TEST(OperatorClassTranslation, SlotThunkAndManifestKeepTheIdentifier)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Money__op_add__thunk(")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SLOT_op_add = ")) << r.cpp;
    EXPECT_TRUE(has(r.slots_out, "Money::op_add ")) << r.slots_out;
    EXPECT_FALSE(has(r.cpp, " op_add(const Money& o)")) << r.cpp;
}

TEST(OperatorClassTranslation, GeneratesNotEqualFromAWrittenEqual)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "bool operator!=(const Money& o) const { return !(*this == o); }")) << r.cpp;
}

TEST(OperatorClassTranslation, NoNotEqualAndNoEqualWithoutAWrittenEqual)
{
    const hotc::TranslateResult r = translate_classes(
        "class Id {\n    int n = 0;\n    bool operator<(in Id o) { return this.n < o.n; }\n"
        "    bool operator>(in Id o) { return this.n > o.n; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "operator!=")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "operator==")) << r.cpp;
}

TEST(OperatorClassTranslation, CompoundOperatorsOnlyForABinaryOperatorThatReturnsTheClass)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Money_Shell& operator+=(const Money& o)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::assign_value(*this, (*this + o));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Money_Shell& operator*=(int32_t f)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::assign_value(*this, (*this * f));")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "operator-=")) << r.cpp; // the written "-" returns int
    EXPECT_FALSE(has(r.cpp, "operator<=")) << r.cpp;
}

TEST(OperatorClassTranslation, CompoundOperatorIsNotConst)
{
    const hotc::TranslateResult r = translate_classes(kMoney);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "operator+=(const Money& o) const")) << r.cpp;
}
