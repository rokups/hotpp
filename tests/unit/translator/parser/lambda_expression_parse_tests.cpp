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

TEST(LambdaExpressionParse, ExpressionBodyIsAReturnBlock)
{
    Parsed p("(int x) => x * 2");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(p.diag.items.size(), 0u);
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l);
    ASSERT_EQ(l->body.stmts.size(), 1u);
    EXPECT_TRUE(std::get_if<ast::ReturnStmt>(&l->body.stmts[0]->v));
    EXPECT_EQ(ast::print(*p.expr), "[=](int32_t x) { return x * 2; }");

    EXPECT_EQ(round_trip("() => this.hp > 0"), "[=]() { return this.hp > 0; }");
    EXPECT_EQ(round_trip("(a, b) => a + b"), "[=](auto a, auto b) { return a + b; }");
    // The body ends at the "," of the argument list and at the ")" of the call.
    EXPECT_EQ(round_trip("f((int n) => n + 1, 5)"), "f([=](int32_t n) { return n + 1; }, 5)");
    EXPECT_EQ(round_trip("f(() => g(1))"), "f([=]() { return g(1); })");
    // A conditional is part of the body. A nested lambda is the body.
    EXPECT_EQ(round_trip("(int x) => x ? 1 : 2"), "[=](int32_t x) { return x ? 1 : 2; }");
    EXPECT_EQ(round_trip("(int x) => (int y) => x + y"),
              "[=](int32_t x) { return [=](int32_t y) { return x + y; }; }");
    // A block body is still a block.
    EXPECT_EQ(round_trip("() => { return 1; }"), "[=]() { return 1; }");
}
