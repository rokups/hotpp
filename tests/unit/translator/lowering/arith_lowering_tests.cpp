// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of the integer divide and modulo: hot::div, hot::mod and the compound forms.
#include "lower.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;
using namespace lowering_test;

namespace
{

// Declares the locals "i" and "j" (int), "u" (uint), "f" (float) and "d" (double).
void add_numbers(Fixture& f)
{
    for (const char* n : {"i", "j", "u", "f", "d"})
        f.param(n);
    f.cx.local_type["i"] = "int";
    f.cx.local_type["j"] = "int";
    f.cx.local_type["u"] = "uint";
    f.cx.local_type["f"] = "float";
    f.cx.local_type["d"] = "double";
}

} // namespace

TEST(ArithLowering, IntegerOperandsUseTheHelper)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("i / j"), "hot::div(i, j)");
    EXPECT_EQ(f.expr("i % j"), "hot::mod(i, j)");
    EXPECT_EQ(f.expr("u / i"), "hot::div(u, i)");
    EXPECT_EQ(f.expr("i / 0"), "hot::div(i, 0)");
    EXPECT_EQ(f.expr("i % 0"), "hot::mod(i, 0)");
    EXPECT_EQ(f.expr("i / -1"), "hot::div(i, -1)");
    EXPECT_EQ(f.expr("i % (-1)"), "hot::mod(i, (-1))");
    EXPECT_EQ(f.expr("i / 0x0"), "hot::div(i, 0x0)");
    EXPECT_EQ(f.expr("1 + i / j"), "1 + hot::div(i, j)");
    EXPECT_EQ(f.expr("(i + 1) / (j - 1)"), "hot::div((i + 1), (j - 1))");
    EXPECT_EQ(f.expr("this.fired / this.n"), "hot::div(__st->fired, __st->n)");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ArithLowering, FloatOperandsStayPlain)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("f / d"), "f / d");
    EXPECT_EQ(f.expr("f / 0"), "f / 0");
    EXPECT_EQ(f.expr("i / f"), "i / f");
    EXPECT_EQ(f.expr("f / i"), "f / i");
    EXPECT_EQ(f.expr("i / 2.0"), "i / 2.0f"); // the lowering writes a float literal
    EXPECT_EQ(f.expr("i / 2.0f"), "i / 2.0f");
    EXPECT_EQ(f.expr("1.5 / i"), "1.5f / i");
    EXPECT_EQ(f.expr("this.health / 2"), "__st->health / 2");
    EXPECT_EQ(f.expr("this.health / this.fired"), "__st->health / __st->fired");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ArithLowering, NonzeroLiteralDivisorStaysPlain)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("i / 2"), "i / 2");
    EXPECT_EQ(f.expr("i % 10"), "i % 10");
    EXPECT_EQ(f.expr("i / -2"), "i / -2");
    EXPECT_EQ(f.expr("i / 0x10"), "i / 0x10");
    EXPECT_EQ(f.expr("u / 3u"), "u / 3u");
    EXPECT_EQ(f.expr("i / (2)"), "i / (2)");
    EXPECT_EQ(f.expr("i / 1"), "i / 1");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ArithLowering, UnknownTypesUseTheHelper)
{
    Fixture f;
    add_numbers(f);
    f.param("k"); // a local without a known type
    EXPECT_EQ(f.expr("k / i"), "hot::div(k, i)");
    EXPECT_EQ(f.expr("k % 3"), "k % 3");
    EXPECT_EQ(f.expr("k / j"), "hot::div(k, j)");
    EXPECT_EQ(f.expr("k / f"), "k / f");
}

TEST(ArithLowering, CompoundFormsPassTheTargetByReference)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.body("i /= j;"), "hot::div_assign(i, j);");
    EXPECT_EQ(f.body("i %= j;"), "hot::mod_assign(i, j);");
    EXPECT_EQ(f.body("i /= 0;"), "hot::div_assign(i, 0);");
    EXPECT_EQ(f.body("i /= 2;"), "i /= 2;");
    EXPECT_EQ(f.body("i %= 2;"), "i %= 2;");
    EXPECT_EQ(f.body("f /= j;"), "f /= j;");
    EXPECT_EQ(f.body("f /= 0;"), "f /= 0;");
    EXPECT_EQ(f.body("this.fired /= this.n;"), "hot::div_assign(__st->fired, __st->n);");
    EXPECT_EQ(f.body("this.health /= this.n;"), "__st->health /= __st->n;");
    EXPECT_EQ(f.body("var x = i / j;"), "auto x = hot::div(i, j);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ArithLowering, PropertyTargets)
{
    Fixture f;
    add_numbers(f);
    ast::Property p;
    p.name = "Count";
    p.type = "int";
    p.has_get = true;
    p.has_set = true;
    f.cd.props.push_back(std::move(p));
    add_props(f);
    f.cx.local_type["i"] = "int";
    f.param("i");
    EXPECT_EQ(f.body("Count /= i;"), "Player__set_Count(__self, hot::div(Player__get_Count(__self), (i)));");
    EXPECT_EQ(f.body("Count %= i;"), "Player__set_Count(__self, hot::mod(Player__get_Count(__self), (i)));");
    EXPECT_EQ(f.body("Count /= 2;"), "Player__set_Count(__self, Player__get_Count(__self) / (2));");
    EXPECT_EQ(f.body("Hp /= i;"), "Player__set_Hp(__self, Player__get_Hp(__self) / (i));");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}
