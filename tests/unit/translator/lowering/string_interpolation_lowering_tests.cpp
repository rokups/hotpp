// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of a string with "${expr}" to std::format or fmt::format.
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;
using namespace lowering_test;

TEST(StringInterpolationLowering, StdFormatByDefault)
{
    Fixture f;
    bool used = false;
    f.cx.uses_format = &used;
    EXPECT_EQ(f.expr("\"hp = ${this.n}, h ${this.health}\""), "std::format(\"hp = {}, h {}\", __st->n, __st->health)");
    EXPECT_TRUE(used);
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(StringInterpolationLowering, FmtFormat)
{
    Fixture f;
    f.cx.format_lib = "fmt";
    EXPECT_EQ(f.expr("\"x ${this.n}\""), "fmt::format(\"x {}\", __st->n)");
}

TEST(StringInterpolationLowering, SpecAndBraces)
{
    Fixture f;
    EXPECT_EQ(f.expr("\"{a} ${this.health:.2f} }b\""), "std::format(\"{{a}} {:.2f} }}b\", __st->health)");
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
    EXPECT_EQ(f.expr("\"\\${x} ${this.n}\""), "std::format(\"${{x}} {}\", __st->n)");
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
    EXPECT_EQ(
        f.expr("\"${this.health > 1 ? \"hi\" : \"lo\"}\""), "std::format(\"{}\", __st->health > 1 ? \"hi\" : \"lo\")");
}
