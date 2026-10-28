// Tests for the printer of the lambda.
#include <gtest/gtest.h>

#include <string>

#include "emit.h"
#include "support/emit_test_support.h"

using namespace sppc::ast;
using namespace emit_test;

TEST(LambdaEmit, Lambda)
{
    LambdaExpr l;
    l.params = {{"x", "int"}, {"y", ""}};
    l.body.stmts = slist(es(bin("=", id("a"), id("x"))));
    EXPECT_EQ(print(*make_expr(std::move(l))), "[=](int x, auto y) { a = x; }");
    EXPECT_EQ(print(*make_expr(LambdaExpr{})), "[=]() {}");
}

TEST(LambdaEmit, RefLambdaParameters)
{
    LambdaExpr l;
    l.params = {{"x", "Vec3&", PassMode::Ref}, {"y", "", PassMode::Out}, {"z", ""}};
    EXPECT_EQ(print(*make_expr(std::move(l))), "[=](Vec3& x, auto& y, auto z) {}");
}
