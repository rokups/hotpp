// Tests for the lowering of the arrow lambda.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

using namespace sppc;
using namespace lowering_test;

TEST(LambdaLowering, Lambda)
{
    Fixture f;
    EXPECT_EQ(f.body("var f = (float x, int* q) => { q.foo(); this.n = x; };"),
              "auto f = spp::make_closure(GEN, \"Player::run lambda\", [=](float x, int32_t* q) "
              "{ [[maybe_unused]] auto* st = state_of(self); spp::deref(q).foo(); st->n = x; });");
    EXPECT_EQ(f.expr("(Actor* a) => { a.x = 1; }"),
              "spp::make_closure(GEN, \"Player::run lambda\", [=](Actor* a) "
              "{ [[maybe_unused]] auto* st = state_of(self); spp::deref(a).x = 1; })");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(LambdaLowering, LambdaNoParameters)
{
    Fixture f;
    EXPECT_EQ(f.expr("() => { this.fired = this.fired + 1; }"),
              "spp::make_closure(GEN, \"Player::run lambda\", [=]() "
              "{ [[maybe_unused]] auto* st = state_of(self); st->fired = st->fired + 1; })");
}

TEST(LambdaLowering, ErrorLambdaInFieldInitializer)
{
    Fixture f;
    f.cx.allow_this = false;
    f.expr("1 +\n  (int x) => { }");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:3: a lambda is not allowed in a field initializer");
}

TEST(LambdaLowering, ErrorBadLambdaParameter)
{
    // The parser always gives a name, so the test builds the node by hand.
    Fixture f;
    LineMap map("\n  (x) => { }");
    f.cx.map = &map;
    ast::LambdaExpr l;
    l.pos = 3;
    l.params.push_back({"", "int"});
    ast::ExprP e = ast::make_expr(std::move(l));
    lower_expr(e, f.cx, f.diag);
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:3: a lambda parameter needs a name");
}
