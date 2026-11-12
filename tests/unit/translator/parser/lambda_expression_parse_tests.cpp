// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the arrow lambda in an expression.
#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace hotc;
using namespace expr_test;

TEST(LambdaExpressionParse, Lambda)
{
    Parsed p("(float x, int* q) => { }");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(ast::print(*p.expr), "[=](float x, int32_t* q) {}");
    EXPECT_EQ(p.diag.items.size(), 0u);
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->body.pos, 21u); // the "{"
    EXPECT_EQ(l->body.end_pos, 23u); // the "}"

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
    EXPECT_EQ(round_trip("(in std.string s, std.map<int, float> m) => { }"),
        "[=](const \x01std::string& s, \x01std::map<int32_t, float> m) {}");
    EXPECT_EQ(round_trip("(std.function<void(int)> f) => { }"), "[=](\x01std::function<void(int32_t)> f) {}");
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
    EXPECT_EQ(round_trip("(int x) => (int y) => x + y"), "[=](int32_t x) { return [=](int32_t y) { return x + y; }; }");
    // A block body is still a block.
    EXPECT_EQ(round_trip("() => { return 1; }"), "[=]() { return 1; }");
}

// ---------------------------------------------------------------- the attributes of a lambda
TEST(LambdaAttributeParse, Name)
{
    Parsed p("[name(\"onFire\")] (CarveRequest r) => { }");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->name, "onFire");
    EXPECT_EQ(l->name_pos, 1u);
    EXPECT_TRUE(l->aliases.empty());
    ASSERT_EQ(l->params.size(), 1u);
    EXPECT_EQ(l->params[0].name, "r");
    // The printer shows the same C++ as a lambda without the attribute.
    EXPECT_EQ(ast::print(*p.expr), "[=](CarveRequest r) {}");
}

TEST(LambdaAttributeParse, NameAndAlias)
{
    Parsed p("[name(\"onFire\"), alias(\"old\")] () => 1");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->name, "onFire");
    ASSERT_EQ(l->aliases.size(), 1u);
    EXPECT_EQ(l->aliases[0], "old");
    EXPECT_EQ(round_trip("[ name( \"a\" ) , alias(\"b\") ]\n(int x) => x"), "[=](int32_t x) { return x; }");
}

TEST(LambdaAttributeParse, AsArgument)
{
    EXPECT_EQ(round_trip("f([name(\"a)\")] () => { }, 2)"), "f([=]() {}, 2)");
}

TEST(LambdaAttributeParse, ListLiteralStaysValid)
{
    Parsed p("[name(\"x\")]");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_TRUE(std::get_if<ast::CollectionExpr>(&p.expr->v));
    EXPECT_TRUE(std::get_if<ast::CollectionExpr>(&Parsed("[f(1), 2]").expr->v));
}

namespace
{
// The text of the one error: "line:col: text". The lambda node stays in the tree.
std::string attr_error(const std::string& text)
{
    Parsed p(text);
    EXPECT_EQ(p.diag.items.size(), 1u) << text << "\n" << p.diag.format("t");
    if (p.diag.items.empty())
        return "<none>";
    const Diag& d = p.diag.items[0];
    return std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg;
}
} // namespace

TEST(LambdaAttributeParse, NotFollowedByLambda)
{
    Parsed p("[name(\"x\")] (a + b)");
    EXPECT_FALSE(p.diag.items.empty());
    if (!p.diag.items.empty())
        EXPECT_NE(p.diag.items[0].msg.find("needs a lambda"), std::string::npos) << p.diag.format("t");
}

TEST(LambdaAttributeParse, UnknownAttribute)
{
    EXPECT_EQ(attr_error("[bogus(\"x\")] () => { }"), "1:2: unknown attribute 'bogus' on a lambda");
}

TEST(LambdaAttributeParse, NameNeedsString)
{
    EXPECT_EQ(attr_error("[name(5)] () => { }"), "1:2: [name(...)] needs a string argument");
    EXPECT_EQ(attr_error("[name(a)] () => { }"), "1:2: [name(...)] needs a string argument");
    EXPECT_EQ(attr_error("[alias(5)] () => { }"), "1:2: [alias(...)] needs a string argument");
}

TEST(LambdaAttributeParse, DuplicateName)
{
    EXPECT_EQ(attr_error("[name(\"a\"), name(\"b\")] () => { }"), "1:13: duplicate attribute 'name'");
}
