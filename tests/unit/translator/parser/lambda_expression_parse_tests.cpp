// Tests for the parser of the arrow lambda in an expression.
#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

using namespace sppc;
using namespace expr_test;

TEST(LambdaExpressionParse, Lambda)
{
    Parsed p("(float x, int* q) => { }");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(ast::print(*p.expr), "[=](float x, int32_t* q) {}");
    EXPECT_EQ(p.diag.items.size(), 0u);
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->body.pos, 21u);  // the "{"
    EXPECT_EQ(l->body.end_pos, 23u);  // the "}"

    Parsed e("() => { x; }");
    ASSERT_TRUE(e.expr);
    EXPECT_EQ(ast::print(*e.expr), "[=]() { x; }");

    // A lambda as an argument; and the paren form still works.
    Parsed a("f((int n) => { }, (a + b) * c)");
    ASSERT_TRUE(a.expr);
    EXPECT_EQ(ast::print(*a.expr), "f([=](int32_t n) {}, (a + b) * c)");
    EXPECT_EQ(round_trip("(a.b)"), "(a::b)");

    // An untyped parameter has an empty type. The printer gives "auto".
    Parsed u("(a, b) => { }");
    ASSERT_TRUE(u.expr);
    const auto* ul = std::get_if<ast::LambdaExpr>(&u.expr->v);
    ASSERT_TRUE(ul);
    ASSERT_EQ(ul->params.size(), 2u);
    EXPECT_EQ(ul->params[0].name, "a");
    EXPECT_EQ(ul->params[0].type, "");
    EXPECT_EQ(u.diag.items.size(), 0u);

    // Qualified, const reference and template types.
    EXPECT_EQ(round_trip("(const std.string& s, std.map<int, float> m) => { }"),
              "[=](const std::string& s, std::map<int32_t, float> m) {}");
    EXPECT_EQ(round_trip("(std.function<void(int)> f) => { }"),
              "[=](std::function<void(int32_t)> f) {}");
    // A paren expression is still a paren expression.
    EXPECT_EQ(round_trip("(a)"), "(a)");
}

TEST(LambdaExpressionParse, OldLambdaParameterIsAnError)
{
    EXPECT_EQ(one_error("(x: int) => { }"), "1:2: parameter syntax changed; write 'Type name'");
}
