// Tests for the removed words "ref" and "out", the "T&" lambda parameter and the prefix "&".
#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

using namespace sppc;
using namespace expr_test;

TEST(RefOutExpressionParse, ReferenceArgumentHasNoMarker)
{
    EXPECT_EQ(round_trip("f(a, b.c)"), "f(a, b::c)");
    EXPECT_EQ(round_trip("Fill(v) + 1"), "Fill(v) + 1");
}

TEST(RefOutExpressionParse, RefAndOutAreRemoved)
{
    // The words are plain names now. The old forms are parse errors.
    for (const char* text : {"ref x", "f(ref a)", "f(out b.c)", "this.target = ref source", "ref a[1]"}) {
        Parsed p(text);
        EXPECT_TRUE(p.expr == nullptr || !p.diag.items.empty()) << text;
    }
    EXPECT_EQ(round_trip("ref + out"), "ref + out");
    EXPECT_EQ(round_trip("reference + outer"), "reference + outer");
}

TEST(RefOutExpressionParse, ReferenceLambdaParameters)
{
    Parsed p("(Vec3& v, int& n, w) => { }");
    ASSERT_TRUE(p.expr != nullptr) << p.diag.format("t");
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l != nullptr);
    ASSERT_EQ(l->params.size(), 3u);
    EXPECT_TRUE(ast::is_ref_type(l->params[0].type));
    EXPECT_EQ(l->params[0].type, "Vec3&");
    EXPECT_TRUE(ast::is_ref_type(l->params[1].type));
    EXPECT_FALSE(ast::is_ref_type(l->params[2].type));
    EXPECT_EQ(l->params[2].type, "");
    EXPECT_EQ(ast::print(*p.expr), "[=](Vec3& v, int32_t& n, auto w) {}");
}

TEST(RefOutExpressionParse, PrefixAmpersandIsAddressOf)
{
    EXPECT_EQ(round_trip("&x"), "&x");
    EXPECT_EQ(round_trip("f(a, &b)"), "f(a, &b)");
    EXPECT_EQ(round_trip("this.p = &source"), "this.p = &source");
    EXPECT_EQ(round_trip("a & b"), "a & b");
    EXPECT_EQ(round_trip("a && b"), "a && b");
    EXPECT_EQ(round_trip("a & &b"), "a & &b");
}
