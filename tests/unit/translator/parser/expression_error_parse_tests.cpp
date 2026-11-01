// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the errors of the expression parser. Each error has a line, a column and a text.
#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace hotc;
using namespace expr_test;

TEST(ExpressionParseError, BraceInitIsAnError)
{
    const char* msg = "brace initialization is not supported; use parentheses";
    EXPECT_NE(one_error("Vec3{1,2,3}").find(msg), std::string::npos);
    EXPECT_NE(one_error("{1,2}").find(msg), std::string::npos);
}

TEST(ExpressionParseError, Errors)
{
    EXPECT_EQ(one_error("a +"), "1:4: expected an expression");
    EXPECT_EQ(one_error("(a"), "1:3: expected ')'");
    EXPECT_EQ(one_error("f(a,"), "1:5: expected an expression");
    EXPECT_EQ(one_error("a b"), "1:3: expected end of expression");
    EXPECT_EQ(one_error("f(a b)"), "1:5: expected ')'");
    EXPECT_EQ(one_error("a[1"), "1:4: expected ']'");
    EXPECT_EQ(one_error("{1, 2"), "1:1: brace initialization is not supported; use parentheses");
    EXPECT_EQ(one_error("if"), "1:1: expected an expression");
    EXPECT_EQ(one_error("a +\n  )"), "2:3: expected an expression");
    EXPECT_EQ(one_error(""), "1:1: expected an expression");
    EXPECT_EQ(one_error("\"abc"), "1:5: unterminated string or character literal");
}

// The script has no "::" and no "->". Each one gives an error at the token.
TEST(ExpressionParseError, ScopeAndArrowErrors)
{
    EXPECT_EQ(one_error("std::printf(1)"), "1:4: use '.' instead of '::'");
    EXPECT_EQ(one_error("a + b::c"), "1:6: use '.' instead of '::'");
    EXPECT_EQ(one_error("::a"), "1:1: use '.' instead of '::'");
    EXPECT_EQ(one_error("x = ::std.max(1)"), "1:5: use '.' instead of '::'");
    EXPECT_EQ(one_error("p->m"), "1:2: use '.' instead of '->'");
    EXPECT_EQ(one_error("f(a, x.y->z)"), "1:9: use '.' instead of '->'");
    EXPECT_EQ(one_error("this->m"), "1:5: use '.' instead of '->'");
    // The "::" in template arguments: the expression parses, the diagnostics have the error.
    Parsed t("std.vector<std::string>{}");
    ASSERT_EQ(t.diag.items.size(), 1u) << t.diag.format("t");
    EXPECT_EQ(t.diag.items[0].msg, "use '.' instead of '::'");
    EXPECT_EQ(t.diag.items[0].col, 15u);
    Parsed u("f<a::B>(x)");
    ASSERT_EQ(u.diag.items.size(), 1u) << u.diag.format("t");
    EXPECT_EQ(u.diag.items[0].msg, "use '.' instead of '::'");
    EXPECT_EQ(u.diag.items[0].col, 4u);
}

TEST(ExpressionParseError, NewBraceIsAnError)
{
    EXPECT_EQ(one_error("new Weapon{1}"), "1:11: expected '(' after the type of 'new'");
}

TEST(ExpressionParseError, CollectionLiteralErrors)
{
    // The first entry decides: a list or a map.
    Parsed a("[1, 2: 3]");
    ASSERT_EQ(a.diag.items.size(), 1u) << a.diag.format("t");
    EXPECT_EQ(a.diag.items[0].msg, "a list element has no value");
    EXPECT_EQ(a.diag.items[0].col, 5u);
    Parsed b("[1: 2, 3]");
    ASSERT_EQ(b.diag.items.size(), 1u) << b.diag.format("t");
    EXPECT_EQ(b.diag.items[0].msg, "a map entry needs a value: key: value");
    EXPECT_EQ(b.diag.items[0].col, 8u);
}
