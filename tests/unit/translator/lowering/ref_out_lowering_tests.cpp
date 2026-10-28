// Tests for the lowering of "ref" and "out".
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

TEST(RefOutLowering, RefArgumentAndRefLocal)
{
    Fixture f;
    f.param("v");
    EXPECT_EQ(f.body("Fill(out v);"), "Fill(spp::ref_arg(v));");
    EXPECT_EQ(f.body("this.target = ref v;"), "st->target = spp::ref_arg(v);");
    EXPECT_EQ(f.body("var r = ref this.health;"), "auto& r = st->health;");
    EXPECT_EQ(f.body("r = 2.0;"), "r = 2.0f;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(RefOutLowering, RefLambdaParameterGetsAReferenceType)
{
    Fixture f;
    EXPECT_NE(f.expr("(ref Vec3 a, out int b) => { }").find("[=](Vec3& a, int32_t& b)"),
              std::string::npos);
}

TEST(RefOutLowering, PrefixAmpersandIsAnError)
{
    Fixture f;
    f.body("var p = &this.health;");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:9: use 'ref' instead of '&'");
}
