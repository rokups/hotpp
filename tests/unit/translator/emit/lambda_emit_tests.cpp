// Tests for the printer of the lambda.
#include "emit.h"
#include "support/emit_test_support.h"

#include <gtest/gtest.h>

#include <string>

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

TEST(LambdaEmit, ReferenceLambdaParameters)
{
    LambdaExpr l;
    l.params = {{"x", "Vec3&"}, {"y", "int32_t&"}, {"z", ""}};
    EXPECT_EQ(print(*make_expr(std::move(l))), "[=](Vec3& x, int32_t& y, auto z) {}");
}
