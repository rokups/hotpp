// Tests for the printer of the expressions. The tests build the nodes by hand.
#include "emit.h"
#include "support/emit_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc::ast;
using namespace emit_test;

TEST(ExpressionEmit, Assignment)
{
    ExprP e = bin("=", arrow(id("st"), "fired"), bin("+", arrow(id("st"), "fired"), num("1")));
    EXPECT_EQ(print(*es(std::move(e))), "st->fired = st->fired + 1;");
}

TEST(ExpressionEmit, CallTwoArgs)
{
    EXPECT_EQ(print(*call(name({"Math", "Max"}), list(id("a"), num("0.0f")))), "Math::Max(a, 0.0f)");
}

TEST(ExpressionEmit, Ternary)
{
    EXPECT_EQ(print(*make_expr(TernaryExpr{id("a"), id("b"), id("c")})), "a ? b : c");
}

TEST(ExpressionEmit, NestedCalls)
{
    ExprP inner = call(arrow(id("self"), "Name"), {});
    ExprP e = call(name({"std", "printf"}), list(str("\"%s\""), call(dot(std::move(inner), "c_str"), {})));
    EXPECT_EQ(print(*e),
        "std::printf(\"%s\", self->Name()."
        "c_str())");
}

TEST(ExpressionEmit, New)
{
    NewExpr n;
    n.type = "std::vector<float>";
    n.args = list(num("1"), id("a"));
    EXPECT_EQ(print(*make_expr(std::move(n))), "new std::vector<float>(1, a)");
}

TEST(ExpressionEmit, NameParts)
{
    NameExpr n;
    n.parts = {"std", "vector"};
    n.template_args = "<int>";
    EXPECT_EQ(print(*make_expr(std::move(n))), "std::vector<int>");
}

TEST(ExpressionEmit, Unary)
{
    EXPECT_EQ(print(*un("!", id("x"))), "!x");
    EXPECT_EQ(print(*un("-", id("x"))), "-x");
    EXPECT_EQ(print(*un("++", id("i"), true)), "i++");
    EXPECT_EQ(print(*un("-", un("-", id("x")))), "- -x");
    EXPECT_EQ(print(*un("*", id("p"))), "*p");
}

TEST(ExpressionEmit, IndexParenNull)
{
    ExprP e = make_expr(IndexExpr{id("a"), make_expr(ParenExpr{bin("+", id("i"), num("1"))})});
    EXPECT_EQ(print(*e), "a[(i + 1)]");
    EXPECT_EQ(print(*make_expr(NullExpr{})), "nullptr");
}

TEST(ExpressionEmit, Raw)
{
    EXPECT_EQ(print(*make_stmt(RawStmt{"int q = 0;"})), "int q = 0;");
    EXPECT_EQ(print(*make_expr(RawExpr{"state_of(self)"})), "state_of(self)");
}
