// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of the integer divide and modulo (hot::div, hot::mod and the compound
// forms), the shifts (hot::shl, hot::shr) and the cast from a float to an integer.
#include "lower.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;
using namespace lowering_test;

namespace
{

// Declares the locals "i" and "j" (int), "u" (uint), "l" (long), "f" (float) and "d" (double).
void add_numbers(Fixture& f)
{
    for (const char* n : {"i", "j", "u", "l", "f", "d"})
        f.param(n);
    f.cx.local_type["i"] = "int";
    f.cx.local_type["j"] = "int";
    f.cx.local_type["u"] = "uint";
    f.cx.local_type["l"] = "long";
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

TEST(ShiftLowering, VariableCountUsesTheHelper)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("i << j"), "hot::shl(i, j)");
    EXPECT_EQ(f.expr("i >> j"), "hot::shr(i, j)");
    EXPECT_EQ(f.expr("u << i"), "hot::shl(u, i)");
    EXPECT_EQ(f.expr("1 << j"), "hot::shl(1, j)");
    EXPECT_EQ(f.expr("(i + 1) << j"), "hot::shl((i + 1), j)");
    EXPECT_EQ(f.expr("this.fired >> this.n"), "hot::shr(__st->fired, __st->n)");
    EXPECT_EQ(f.expr("1 + (i << j)"), "1 + (hot::shl(i, j))");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ShiftLowering, LiteralCountInRangeStaysPlain)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("i << 0"), "i << 0");
    EXPECT_EQ(f.expr("i << 3"), "i << 3");
    EXPECT_EQ(f.expr("i << 31"), "i << 31");
    EXPECT_EQ(f.expr("i >> 31"), "i >> 31");
    EXPECT_EQ(f.expr("u << 0x1f"), "u << 0x1f");
    EXPECT_EQ(f.expr("l << 63"), "l << 63");
    EXPECT_EQ(f.expr("1 << 24"), "1 << 24");
    EXPECT_EQ(f.expr("this.fired << 4"), "__st->fired << 4");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ShiftLowering, LiteralCountOutOfRangeUsesTheHelper)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("i << 32"), "hot::shl(i, 32)");
    EXPECT_EQ(f.expr("i >> 33"), "hot::shr(i, 33)");
    EXPECT_EQ(f.expr("i << -1"), "hot::shl(i, -1)");
    EXPECT_EQ(f.expr("l << 64"), "hot::shl(l, 64)");
    EXPECT_EQ(f.expr("1 << 40"), "hot::shl(1, 40)");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ShiftLowering, UnknownAndNonIntegerOperands)
{
    Fixture f;
    add_numbers(f);
    f.param("k"); // a local without a known type
    EXPECT_EQ(f.expr("k << 3"), "hot::shl(k, 3)");
    EXPECT_EQ(f.expr("k >> i"), "hot::shr(k, i)");
    EXPECT_EQ(f.expr("f << 1"), "f << 1");
    EXPECT_EQ(f.expr("i << f"), "i << f");
    EXPECT_EQ(f.expr("k << std.endl"), "k << \x01" "std::endl"); // a stream manipulator is a function template
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ShiftLowering, CompoundFormsPassTheTargetByReference)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.body("i <<= j;"), "hot::shl_assign(i, j);");
    EXPECT_EQ(f.body("i >>= j;"), "hot::shr_assign(i, j);");
    EXPECT_EQ(f.body("i <<= 2;"), "i <<= 2;");
    EXPECT_EQ(f.body("i >>= 40;"), "hot::shr_assign(i, 40);");
    EXPECT_EQ(f.body("l <<= 40;"), "l <<= 40;");
    EXPECT_EQ(f.body("f <<= 1;"), "f <<= 1;");
    EXPECT_EQ(f.body("this.fired <<= this.n;"), "hot::shl_assign(__st->fired, __st->n);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(ShiftLowering, PropertyTargets)
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
    EXPECT_EQ(f.body("Count <<= i;"), "Player__set_Count(__self, hot::shl(Player__get_Count(__self), (i)));");
    EXPECT_EQ(f.body("Count >>= i;"), "Player__set_Count(__self, hot::shr(Player__get_Count(__self), (i)));");
    EXPECT_EQ(f.body("Count <<= 2;"), "Player__set_Count(__self, Player__get_Count(__self) << (2));");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(FloatToIntLowering, CastFromFloatUsesTheHelper)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("(int)f"), "hot::float_to_int<int32_t>(f)");
    EXPECT_EQ(f.expr("(int)d"), "hot::float_to_int<int32_t>(d)");
    EXPECT_EQ(f.expr("(uint)f"), "hot::float_to_int<uint32_t>(f)");
    EXPECT_EQ(f.expr("(long)d"), "hot::float_to_int<int64_t>(d)");
    EXPECT_EQ(f.expr("(int)3.9"), "hot::float_to_int<int32_t>(3.9f)");
    EXPECT_EQ(f.expr("(int)this.health"), "hot::float_to_int<int32_t>(__st->health)");
    EXPECT_EQ(f.expr("(int)f + 1"), "hot::float_to_int<int32_t>(f) + 1");
    EXPECT_EQ(f.expr("(int)(f * 2.0)"), "hot::float_to_int<int32_t>((f * 2.0f))");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(FloatToIntLowering, FunctionalCastsUseTheHelper)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("int(f)"), "hot::float_to_int<int32_t>(f)");
    EXPECT_EQ(f.expr("uint(d)"), "hot::float_to_int<uint32_t>(d)");
    EXPECT_EQ(f.expr("static_cast<int>(f)"), "hot::float_to_int<int32_t>(f)");
    EXPECT_EQ(f.expr("static_cast<long>(d)"), "hot::float_to_int<int64_t>(d)");
    EXPECT_EQ(f.expr("int(i)"), "int(i)");
    EXPECT_EQ(f.expr("static_cast<int>(i)"), "static_cast<int32_t>(i)");
    EXPECT_EQ(f.expr("static_cast<float>(i)"), "static_cast<float>(i)");
    EXPECT_EQ(f.expr("float(i)"), "float(i)");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(FloatToIntLowering, PointerCastUsesTheHelper)
{
    Fixture f;
    add_numbers(f);
    f.param("p"); // a local without a known type
    EXPECT_EQ(f.expr("(nuint)p"), "hot::float_to_int<uintptr_t>(p)");
}

TEST(FloatToIntLowering, UnknownOperandUsesTheHelper)
{
    Fixture f;
    add_numbers(f);
    f.param("k"); // a local without a known type
    EXPECT_EQ(f.expr("(int)k"), "hot::float_to_int<int32_t>(k)");
}

TEST(FloatToIntLowering, OtherCastsStayPlain)
{
    Fixture f;
    add_numbers(f);
    EXPECT_EQ(f.expr("(int)i"), "(int32_t)i");
    EXPECT_EQ(f.expr("(int)u"), "(int32_t)u");
    EXPECT_EQ(f.expr("(int)3"), "(int32_t)3");
    EXPECT_EQ(f.expr("(float)i"), "(float)i");
    EXPECT_EQ(f.expr("(float)d"), "(float)d");
    EXPECT_EQ(f.expr("(bool)f"), "(bool)f");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}
