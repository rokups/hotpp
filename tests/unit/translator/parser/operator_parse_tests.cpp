// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of operator members and for the names in operator_names.h.
#include "operator_names.h"
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;
using namespace script_test;

namespace
{

struct OpRow
{
    const char* sym;
    const char* ident;
    bool binary;
    bool compare;
};

const OpRow kBinary[] = {
    {"+", "op_add", true, false},
    {"-", "op_sub", true, false},
    {"*", "op_mul", true, false},
    {"/", "op_div", true, false},
    {"%", "op_mod", true, false},
    {"&", "op_and", true, false},
    {"|", "op_or", true, false},
    {"^", "op_xor", true, false},
    {"<<", "op_shl", true, false},
    {">>", "op_shr", true, false},
    {"==", "op_eq", false, true},
    {"<", "op_lt", false, true},
    {">", "op_gt", false, true},
    {"<=", "op_le", false, true},
    {">=", "op_ge", false, true},
    {"[]", "op_index", false, false},
};

} // namespace

TEST(OperatorParse, BinaryAndCompareRows)
{
    for (const OpRow& r : kBinary)
    {
        const std::string sym = r.sym;
        ModParsed p(member_class("T operator" + sym + "(in T o) { return this; }"));
        EXPECT_TRUE(p.errors().empty()) << sym;
        ASSERT_EQ(p.script.classes.at(0).methods.size(), 1u) << sym;
        EXPECT_EQ(p.method().name, r.ident) << sym;
        EXPECT_EQ(p.method().op, sym);
        EXPECT_EQ(p.method().params.size(), 1u);
        EXPECT_EQ(op_ident(sym, 1), std::optional<std::string>(r.ident));
        EXPECT_EQ(op_symbol(r.ident), sym);
        EXPECT_EQ(is_binary_op_ident(r.ident), r.binary);
        EXPECT_EQ(is_compare_op_ident(r.ident), r.compare);
        EXPECT_EQ(cxx_member_name(p.method()), "operator" + sym);
    }
}

TEST(OperatorParse, UnaryRows)
{
    ModParsed p(member_class("bool operator!() => true;\n    T operator~() => this;"));
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.classes.at(0).methods.size(), 2u);
    EXPECT_EQ(p.method(0).name, "op_not");
    EXPECT_EQ(p.method(0).op, "!");
    EXPECT_EQ(p.method(1).name, "op_compl");
    EXPECT_EQ(p.method(1).op, "~");
    EXPECT_EQ(op_symbol("op_not"), "!");
    EXPECT_EQ(op_symbol("op_compl"), "~");
}

TEST(OperatorParse, MinusAndPlusDependOnArity)
{
    ModParsed p(member_class("T operator-() => this;\n    T operator-(in T o) => this;\n"
                             "    T operator+() => this;\n    T operator+(in T o) => this;"));
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.classes.at(0).methods.size(), 4u);
    EXPECT_EQ(p.method(0).name, "op_neg");
    EXPECT_EQ(p.method(1).name, "op_sub");
    EXPECT_EQ(p.method(2).name, "op_pos");
    EXPECT_EQ(p.method(3).name, "op_add");
    EXPECT_EQ(p.method(0).op, "-");
    EXPECT_EQ(p.method(2).op, "+");
    EXPECT_EQ(op_symbol("op_neg"), "-");
    EXPECT_EQ(op_symbol("op_pos"), "+");
    EXPECT_FALSE(is_binary_op_ident("op_neg"));
    EXPECT_FALSE(op_ident("!", 1).has_value());
    EXPECT_FALSE(op_ident("[]", 0).has_value());
}

TEST(OperatorParse, RefIndexOperator)
{
    ModParsed p(member_class("ref float operator[](int i) { return data[i]; }"));
    EXPECT_TRUE(p.errors().empty()) << (p.errors().empty() ? "" : p.errors()[0]);
    EXPECT_EQ(p.method().name, "op_index");
    EXPECT_EQ(p.method().op, "[]");
    EXPECT_EQ(p.method().ret, "ref float");
    ASSERT_EQ(p.method().params.size(), 1u);
    EXPECT_EQ(p.method().params[0].name, "i");
}

TEST(OperatorParse, ExpressionAndBlockBodies)
{
    ModParsed p(member_class("T operator+(in T o) => this;\n    T operator-(in T o) { return this; }"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_TRUE(p.method(0).expr_body);
    EXPECT_FALSE(p.method(1).expr_body);
    EXPECT_EQ(p.method(1).name, "op_sub");
}

TEST(OperatorParse, SpaceBeforeSymbol)
{
    ModParsed p(member_class("bool operator == (in T o) => true;"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_EQ(p.method().name, "op_eq");
    EXPECT_EQ(p.method().op, "==");
}

TEST(OperatorParse, PlainMethodHasNoOperator)
{
    ModParsed p(member_class("int operator_count() => 1;\n    void F() { }"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_EQ(p.method(0).name, "operator_count");
    EXPECT_TRUE(p.method(0).op.empty());
    EXPECT_EQ(cxx_member_name(p.method(0)), "operator_count");
    EXPECT_EQ(p.method(1).name, "F");
    EXPECT_TRUE(p.method(1).op.empty());
}

TEST(OperatorParse, UnknownSymbolKeepsRawName)
{
    ModParsed p(member_class("T operator+=(in T o) => this;"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_EQ(p.method().name, "operator+=");
    EXPECT_EQ(p.method().op, "+=");
    EXPECT_FALSE(op_ident("+=", 1).has_value());
    EXPECT_FALSE(op_ident("!=", 1).has_value());
}

TEST(OperatorParse, LongestSymbolWins)
{
    ModParsed p(member_class("int operator<=>(in T o) => 0;\n    T operator<<=(in T o) => this;\n"
                             "    T operator()(in T o) => this;"));
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.classes.at(0).methods.size(), 3u);
    EXPECT_EQ(p.method(0).op, "<=>");
    EXPECT_EQ(p.method(1).op, "<<=");
    EXPECT_EQ(p.method(2).op, "()");
}

TEST(OperatorParse, MissingParameterListIsAnError)
{
    ModParsed p(member_class("T operator +;"));
    ASSERT_FALSE(p.errors().empty());
    EXPECT_NE(p.errors()[0].find("an operator needs a parameter list"), std::string::npos) << p.errors()[0];
}

TEST(OperatorParse, MissingSymbolIsAnError)
{
    ModParsed p(member_class("T operator;"));
    ASSERT_FALSE(p.errors().empty());
    EXPECT_NE(p.errors()[0].find("expected an operator symbol"), std::string::npos) << p.errors()[0];
}

TEST(OperatorParse, StructMember)
{
    ModParsed p("struct V {\n    float x;\n    V operator+(in V o) => this;\n}\n");
    EXPECT_TRUE(p.errors().empty());
    EXPECT_EQ(p.method().name, "op_add");
    EXPECT_EQ(p.method().op, "+");
}
