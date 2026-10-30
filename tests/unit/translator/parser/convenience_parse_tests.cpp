// Tests for the parser of three conveniences: "=>" bodies, default values and "??".
#include "emit.h"
#include "parser/parser.h"
#include "support/expression_parse_test_support.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace sppc;
using namespace script_test;
using expr_test::Parsed;

namespace
{
ast::Script parse(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    return parse_script(text, map, sd);
}
} // namespace

TEST(ExpressionBodyParse, ValueMethodReturnsTheExpression)
{
    Diagnostics sd;
    const ast::Script s = parse("class W : B {\n    int Get() => count + 1;\n}\n", sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    ASSERT_EQ(s.classes[0].methods.size(), 1u);
    const ast::Method& m = s.classes[0].methods[0];
    EXPECT_TRUE(m.expr_body);
    EXPECT_EQ(m.ret, "int32_t");
    ASSERT_EQ(m.body.stmts.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ast::ReturnStmt>(m.body.stmts[0]->v));
}

TEST(ExpressionBodyParse, VoidMethodRunsTheExpression)
{
    Diagnostics sd;
    const ast::Script s = parse("class W : B {\n    override void Tick(float dt) => Move(dt);\n}\n", sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    const ast::Method& m = s.classes[0].methods[0];
    EXPECT_TRUE(m.is_override);
    ASSERT_EQ(m.body.stmts.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ast::ExprStmt>(m.body.stmts[0]->v));
}

TEST(ExpressionBodyParse, NextMemberStillParses)
{
    Diagnostics sd;
    EXPECT_EQ(members_of("class W : B {\n    int A() => 1;\n    int n;\n    void B() { }\n}\n", sd),
        (std::vector<std::string>{"field int32_t|n|-|", "method int32_t|A", "method |B"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(ExpressionBodyParse, ReadOnlyProperty)
{
    Diagnostics sd;
    const ast::Script s = parse("class W : B {\n    int hits;\n    int Power => hits * 2;\n}\n", sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    ASSERT_EQ(s.classes[0].props.size(), 1u);
    const ast::Property& p = s.classes[0].props[0];
    EXPECT_EQ(p.name, "Power");
    EXPECT_TRUE(p.has_get);
    EXPECT_FALSE(p.has_set);
    ASSERT_TRUE(p.get_body.has_value());
    ASSERT_EQ(p.get_body->stmts.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ast::ReturnStmt>(p.get_body->stmts[0]->v));
}

TEST(ExpressionBodyParse, Errors)
{
    Diagnostics sd;
    parse("class W : B {\n    int Get() => ;\n}\n", sd);
    EXPECT_GE(sd.items.size(), 1u);
    Diagnostics sc;
    parse("class W : B {\n    W() => 1;\n}\n", sc);
    ASSERT_GE(sc.items.size(), 1u);
    EXPECT_NE(sc.items[0].msg.find("constructor needs a block body"), std::string::npos) << sc.format("t");
}

TEST(DefaultValueParse, ParametersKeepTheirDefaultExpressions)
{
    Diagnostics sd;
    const ast::Script s =
        parse("class W : B {\n    void Heal(float amount = 1.0, int n = f(1, 2), std.string s) { }\n}\n", sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    const auto& ps = s.classes[0].methods[0].params;
    ASSERT_EQ(ps.size(), 3u);
    EXPECT_EQ(ps[0].type, "float");
    EXPECT_EQ(ps[0].name, "amount");
    ASSERT_TRUE(ps[0].def);
    ASSERT_TRUE(ps[0].def->expr);
    EXPECT_EQ(ast::print(*ps[0].def->expr), "1.0");
    ASSERT_TRUE(ps[1].def);
    ASSERT_TRUE(ps[1].def->expr);
    EXPECT_EQ(ast::print(*ps[1].def->expr), "f(1, 2)");
    EXPECT_FALSE(ps[2].def);
}

TEST(DefaultValueParse, ConstructorAndNoDefault)
{
    Diagnostics sd;
    const ast::Script s = parse("class W : B {\n    W(int a = 3) { }\n    void F(int b) { }\n}\n", sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    ASSERT_EQ(s.classes[0].ctors.size(), 1u);
    EXPECT_TRUE(s.classes[0].ctors[0].params[0].def);
    EXPECT_FALSE(s.classes[0].methods[0].params[0].def);
}

TEST(DefaultValueParse, EmptyDefaultIsAnError)
{
    Diagnostics sd;
    parse("class W : B {\n    void F(int a = ) { }\n}\n", sd);
    ASSERT_GE(sd.items.size(), 1u);
    EXPECT_NE(sd.items[0].msg.find("expected a default value"), std::string::npos) << sd.format("t");
}

TEST(CoalesceParse, PrecedenceAndAssociativity)
{
    // "??" is looser than "||" and tighter than "?:".
    Parsed a("x ?? y || z");
    ASSERT_TRUE(a.expr);
    const auto* top = std::get_if<ast::BinaryExpr>(&a.expr->v);
    ASSERT_TRUE(top);
    EXPECT_EQ(top->op, "??");
    const auto* rhs = std::get_if<ast::BinaryExpr>(&top->rhs->v);
    ASSERT_TRUE(rhs);
    EXPECT_EQ(rhs->op, "||");
    // Right associative: "a ?? b ?? c" is "a ?? (b ?? c)".
    Parsed b("a ?? b ?? c");
    ASSERT_TRUE(b.expr);
    const auto* bt = std::get_if<ast::BinaryExpr>(&b.expr->v);
    ASSERT_TRUE(bt);
    EXPECT_TRUE(std::holds_alternative<ast::NameExpr>(bt->lhs->v));
    const auto* br = std::get_if<ast::BinaryExpr>(&bt->rhs->v);
    ASSERT_TRUE(br);
    EXPECT_EQ(br->op, "??");
    // The condition operator is the loosest.
    Parsed c("p ? x ?? y : z");
    ASSERT_TRUE(c.expr);
    const auto* ct = std::get_if<ast::TernaryExpr>(&c.expr->v);
    ASSERT_TRUE(ct);
    const auto* ca = std::get_if<ast::BinaryExpr>(&ct->a->v);
    ASSERT_TRUE(ca);
    EXPECT_EQ(ca->op, "??");
    // Assignment is looser still.
    Parsed d("v = x ?? y");
    ASSERT_TRUE(d.expr);
    const auto* dt = std::get_if<ast::BinaryExpr>(&d.expr->v);
    ASSERT_TRUE(dt);
    EXPECT_EQ(dt->op, "=");
}

TEST(CoalesceParse, ConditionStillParses)
{
    EXPECT_EQ(expr_test::round_trip("a ? b : c"), "a ? b : c");
    EXPECT_EQ(expr_test::round_trip("x ?? y"), "x ?? y");
}

TEST(IsBindingParse, BindingNameAfterTheType)
{
    Parsed a("x is Grunt g");
    ASSERT_TRUE(a.expr);
    const auto* t = std::get_if<ast::TypeTestExpr>(&a.expr->v);
    ASSERT_TRUE(t);
    EXPECT_FALSE(t->is_as);
    EXPECT_EQ(t->type, "Grunt");
    EXPECT_EQ(t->bind, "g");
    // A chain to the right keeps the binding at the left.
    Parsed b("x is ns.Grunt g && g.rage > 1");
    ASSERT_TRUE(b.expr);
    const auto* top = std::get_if<ast::BinaryExpr>(&b.expr->v);
    ASSERT_TRUE(top);
    EXPECT_EQ(top->op, "&&");
    const auto* l = std::get_if<ast::TypeTestExpr>(&top->lhs->v);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->bind, "g");
    EXPECT_EQ(l->type, "ns::Grunt");
}

TEST(IsBindingParse, PlainTestHasNoBinding)
{
    for (const char* text :
        {"x is Grunt", "x is Grunt && y", "x is Grunt || y", "x is Grunt ? 1 : 2", "x is Grunt as Foo"})
    {
        Parsed a(text);
        ASSERT_TRUE(a.expr) << text;
        EXPECT_TRUE(a.diag.items.empty()) << text;
    }
    Parsed p("x is Grunt");
    EXPECT_TRUE(std::get<ast::TypeTestExpr>(p.expr->v).bind.empty());
}
