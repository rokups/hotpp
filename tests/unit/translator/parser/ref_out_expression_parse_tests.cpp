// Tests for "ref" and "out" in an expression: arguments, lambda parameters and the removed "&".
#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

using namespace sppc;
using namespace expr_test;

TEST(RefOutExpressionParse, RefAndOutArguments)
{
    EXPECT_EQ(round_trip("f(ref a, out b.c)"), "f(spp::ref_arg(a), spp::ref_arg(b::c))");
    EXPECT_EQ(round_trip("ref a[1]"), "spp::ref_arg(a[1])");
    EXPECT_EQ(round_trip("this.target = ref source"), "this.target = spp::ref_arg(source)");
    EXPECT_EQ(round_trip("Fill(out v) + 1"), "Fill(spp::ref_arg(v)) + 1");
}

TEST(RefOutExpressionParse, RefNode)
{
    Parsed p("f(ref a, out b)");
    ASSERT_TRUE(p.expr != nullptr);
    const auto* call = std::get_if<ast::CallExpr>(&p.expr->v);
    ASSERT_TRUE(call != nullptr);
    ASSERT_EQ(call->args.size(), 2u);
    const auto* r = std::get_if<ast::RefExpr>(&call->args[0]->v);
    const auto* o = std::get_if<ast::RefExpr>(&call->args[1]->v);
    ASSERT_TRUE(r != nullptr);
    ASSERT_TRUE(o != nullptr);
    EXPECT_FALSE(r->is_out);
    EXPECT_TRUE(o->is_out);
    EXPECT_EQ(r->pos, 2u);
    EXPECT_EQ(o->pos, 9u);
}

TEST(RefOutExpressionParse, RefAndOutAreReserved)
{
    // A name that only starts with the word is still a name.
    EXPECT_EQ(round_trip("reference + outer"), "reference + outer");
    EXPECT_NE(one_error("ref"), "<none>");
    EXPECT_NE(one_error("a + out"), "<none>");
}

TEST(RefOutExpressionParse, RefAndOutLambdaParameters)
{
    Parsed p("(ref Vec3 v, out int n, ref w) => { }");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l != nullptr);
    ASSERT_EQ(l->params.size(), 3u);
    EXPECT_EQ(l->params[0].mode, ast::PassMode::Ref);
    EXPECT_EQ(l->params[0].type, "Vec3");
    EXPECT_EQ(l->params[1].mode, ast::PassMode::Out);
    EXPECT_EQ(l->params[2].mode, ast::PassMode::Ref);
    EXPECT_EQ(l->params[2].type, "");
    EXPECT_EQ(ast::print(*p.expr), "[=](Vec3& v, int32_t& n, auto& w) {}");
}

TEST(RefOutExpressionParse, PrefixAmpersandIsRemoved)
{
    EXPECT_EQ(one_error("&x"), "1:1: use 'ref' instead of '&'");
    EXPECT_EQ(one_error("f(a, &b)"), "1:6: use 'ref' instead of '&'");
    EXPECT_EQ(one_error("this.p = &source"), "1:10: use 'ref' instead of '&'");
    EXPECT_EQ(round_trip("a & b"), "a & b");
}
