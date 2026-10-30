// Tests for the parser of a string with "${expr}" (design 7.3, DECISIONS item 10).
// The printer writes the script form of the unlowered node.
#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace hotc;
using namespace expr_test;

TEST(StringInterpolationParse, PlainStringStaysALiteral)
{
    for (const char* t : {"\"hp\"", "\"a {b} c\"", "\"price $5\"", "\"\""})
    {
        Parsed p(t);
        ASSERT_TRUE(p.expr != nullptr) << t;
        EXPECT_TRUE(std::holds_alternative<ast::StringExpr>(p.expr->v)) << t;
        EXPECT_EQ(ast::print(*p.expr), t);
    }
}

TEST(StringInterpolationParse, ExpressionParts)
{
    Parsed p("\"hp = ${hp}, name ${p.Name()}!\"");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    auto* ip = std::get_if<ast::InterpExpr>(&p.expr->v);
    ASSERT_NE(ip, nullptr);
    ASSERT_EQ(ip->args.size(), 2u);
    ASSERT_EQ(ip->lits.size(), 3u);
    EXPECT_EQ(ip->lits[0], "hp = ");
    EXPECT_EQ(ip->lits[1], ", name ");
    EXPECT_EQ(ip->lits[2], "!");
    EXPECT_TRUE(std::holds_alternative<ast::NameExpr>(ip->args[0]->v));
    EXPECT_TRUE(std::holds_alternative<ast::CallExpr>(ip->args[1]->v));
    EXPECT_EQ(round_trip("\"hp = ${hp}, name ${p.Name()}!\""),
        "\"hp = ${hp}, name ${p::Name()}!\""); // the printer writes C++ scope
}

TEST(StringInterpolationParse, FormatSpec)
{
    Parsed p("\"${x:.2f}|${y:>8}|${z}\"");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    auto* ip = std::get_if<ast::InterpExpr>(&p.expr->v);
    ASSERT_NE(ip, nullptr);
    ASSERT_EQ(ip->specs.size(), 3u);
    EXPECT_EQ(ip->specs[0], ".2f");
    EXPECT_EQ(ip->specs[1], ">8");
    EXPECT_EQ(ip->specs[2], "");
}

TEST(StringInterpolationParse, ColonThatIsNotASpec)
{
    // The colon of "?:", of "::" and of a nested string is part of the expression.
    Parsed p("\"${a ? b : c}|${A.B}|${f(\"x:y\")}|${a ? b : c:>3}\"");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    auto* ip = std::get_if<ast::InterpExpr>(&p.expr->v);
    ASSERT_NE(ip, nullptr);
    ASSERT_EQ(ip->args.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<ast::TernaryExpr>(ip->args[0]->v));
    EXPECT_EQ(ip->specs[0], "");
    EXPECT_EQ(ip->specs[2], "");
    EXPECT_TRUE(std::holds_alternative<ast::TernaryExpr>(ip->args[3]->v));
    EXPECT_EQ(ip->specs[3], ">3");
}

TEST(StringInterpolationParse, NestedStringAndBraceInExpression)
{
    EXPECT_EQ(round_trip("\"a ${f(\"}\", '}')} b\""), "\"a ${f(\"}\", '}')} b\"");
    EXPECT_EQ(round_trip("\"${f(\"${x}\")}\""), "\"${f(\"${x}\")}\"");
}

TEST(StringInterpolationParse, EscapedDollarIsNotAnInterpolation)
{
    Parsed p("\"cost \\${x}\"");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    auto* ip = std::get_if<ast::InterpExpr>(&p.expr->v);
    ASSERT_NE(ip, nullptr);
    EXPECT_TRUE(ip->args.empty());
    ASSERT_EQ(ip->lits.size(), 1u);
    EXPECT_EQ(ip->lits[0], "cost \\${x}");
}

TEST(StringInterpolationParse, InterpolationIsAnOperand)
{
    EXPECT_EQ(round_trip("f(\"a${b}\", 1) + \"c${d}\""), "f(\"a${b}\", 1) + \"c${d}\"");
}

TEST(StringInterpolationParse, Errors)
{
    Parsed empty("\"a ${} b\"");
    EXPECT_FALSE(empty.diag.items.empty());
    Parsed bad("\"a ${1 +} b\"");
    EXPECT_FALSE(bad.diag.items.empty());
    Parsed open("\"a ${x\"");
    EXPECT_FALSE(open.diag.items.empty());
}
