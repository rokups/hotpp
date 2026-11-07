// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the reserved words "ref" and "out", the "T&" lambda parameter and the prefix "&".
#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace hotc;
using namespace expr_test;

TEST(RefOutExpressionParse, ReferenceArgumentHasNoMarker)
{
    EXPECT_EQ(round_trip("f(a, b.c)"), "f(a, b::c)");
    EXPECT_EQ(round_trip("Fill(v) + 1"), "Fill(v) + 1");
}

TEST(RefOutExpressionParse, RefAndOutAreRemoved)
{
    // The old forms are parse errors. The words are reserved.
    for (const char* text : {"ref x", "f(ref a)", "f(out b.c)", "this.target = ref source", "ref a[1]"})
    {
        Parsed p(text);
        EXPECT_TRUE(p.expr == nullptr || !p.diag.items.empty()) << text;
    }
    for (const char* text : {"ref + out", "ref", "out"})
    {
        Parsed p(text);
        EXPECT_TRUE(p.expr == nullptr || !p.diag.items.empty()) << text;
    }
    EXPECT_EQ(round_trip("refx + outer"), "refx + outer");
    EXPECT_EQ(round_trip("reference + outer"), "reference + outer");
}

TEST(RefOutExpressionParse, ReferenceLambdaParameters)
{
    Parsed p("(ref Vec3 v, ref int n, w) => { }");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l != nullptr);
    ASSERT_EQ(l->params.size(), 3u);
    EXPECT_TRUE(ast::is_ref_type(l->params[0].type));
    EXPECT_EQ(l->params[0].type, "Vec3&");
    EXPECT_TRUE(ast::is_ref_type(l->params[1].type));
    EXPECT_FALSE(ast::is_ref_type(l->params[2].type));
    EXPECT_EQ(l->params[2].type, "");
    EXPECT_EQ(ast::print(*p.expr), "[=](Vec3& v, int32_t& n, auto w) {}");
}

// The binary "&" and "&&" stay. The prefix "&" is an error (see expression_error_parse_tests.cpp).
TEST(RefOutExpressionParse, BinaryAmpersandStays)
{
    EXPECT_EQ(round_trip("a & b"), "a & b");
    EXPECT_EQ(round_trip("a && b"), "a && b");
    EXPECT_EQ(round_trip("a & *b"), "a & hot::star(b)");
}
