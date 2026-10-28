// Tests for the lowering of a string with "${expr}" to std::format or fmt::format.
#include <gtest/gtest.h>

#include <string>

#include "support/lowering_test_support.h"

using namespace sppc;
using namespace lowering_test;

TEST(StringInterpolationLowering, StdFormatByDefault)
{
    Fixture f;
    bool used = false;
    f.cx.uses_format = &used;
    EXPECT_EQ(f.expr("\"hp = ${this.n}, h ${this.health}\""),
              "std::format(\"hp = {}, h {}\", st->n, st->health)");
    EXPECT_TRUE(used);
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(StringInterpolationLowering, FmtFormat)
{
    Fixture f;
    f.cx.format_lib = "fmt";
    EXPECT_EQ(f.expr("\"x ${this.n}\""), "fmt::format(\"x {}\", st->n)");
}

TEST(StringInterpolationLowering, SpecAndBraces)
{
    Fixture f;
    EXPECT_EQ(f.expr("\"{a} ${this.health:.2f} }b\""), "std::format(\"{{a}} {:.2f} }}b\", st->health)");
}

TEST(StringInterpolationLowering, EscapedDollar)
{
    Fixture f;
    bool used = false;
    f.cx.uses_format = &used;
    // No argument: a plain literal. The braces stay single.
    EXPECT_EQ(f.expr("\"cost \\${x} {y}\""), "\"cost ${x} {y}\"");
    EXPECT_FALSE(used);
    // With an argument: the "$" stays and the braces double.
    EXPECT_EQ(f.expr("\"\\${x} ${this.n}\""), "std::format(\"${{x}} {}\", st->n)");
    EXPECT_TRUE(used);
}

TEST(StringInterpolationLowering, PlainStringIsUnchanged)
{
    Fixture f;
    bool used = false;
    f.cx.uses_format = &used;
    EXPECT_EQ(f.expr("\"a {b} c\""), "\"a {b} c\"");
    EXPECT_FALSE(used);
}

TEST(StringInterpolationLowering, ArgumentIsLowered)
{
    Fixture f;
    EXPECT_EQ(f.expr("\"${this.health > 1 ? \"hi\" : \"lo\"}\""),
              "std::format(\"{}\", st->health > 1 ? \"hi\" : \"lo\")");
}
