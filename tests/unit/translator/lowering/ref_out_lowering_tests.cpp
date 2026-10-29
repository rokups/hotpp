// Tests for the lowering of a "T&" parameter, a "T&" local and a "&e" argument.
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

TEST(RefOutLowering, ReferenceArgumentAndReferenceLocal)
{
    Fixture f;
    f.param("v");
    EXPECT_EQ(f.body("Fill(v);"), "Fill(v);");
    EXPECT_EQ(f.body("Vec3& r = this.health;"), "Vec3& r = __st->health;");
    EXPECT_EQ(f.body("this.target = &v;"), "__st->target = &v;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(RefOutLowering, ReferenceLambdaParameterKeepsItsReferenceType)
{
    Fixture f;
    EXPECT_NE(f.expr("(Vec3& a, int& b) => { }").find("[=](Vec3& a, int32_t& b)"),
              std::string::npos);
}

TEST(RefOutLowering, PrefixAmpersandIsNotAnError)
{
    Fixture f;
    f.body("var p = &this.health;");
    EXPECT_EQ(f.errors().size(), 0u);
}
