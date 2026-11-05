// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::div, hot::mod, hot::div_assign and hot::mod_assign (arith.h).
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <type_traits>

TEST(Arith, DivideAndModuloByZeroThrow)
{
    EXPECT_THROW(hot::div(1, 0), hot::ArithmeticError);
    EXPECT_THROW(hot::mod(1, 0), hot::ArithmeticError);
    EXPECT_THROW(hot::div(int64_t(1), int64_t(0)), hot::ArithmeticError);
    EXPECT_THROW(hot::mod(1u, 0u), hot::ArithmeticError);
    EXPECT_THROW(hot::div(int8_t(1), int8_t(0)), hot::ArithmeticError);
    EXPECT_THROW(hot::div(1, 0u), hot::ScriptError); // ArithmeticError is a ScriptError
    try
    {
        hot::div(1, 0);
        FAIL();
    }
    catch (const hot::ArithmeticError& e)
    {
        EXPECT_STREQ(e.what(), "integer divide by zero");
    }
    try
    {
        hot::mod(1, 0);
        FAIL();
    }
    catch (const hot::ArithmeticError& e)
    {
        EXPECT_STREQ(e.what(), "integer modulo by zero");
    }
}

TEST(Arith, ValuesMatchTheOperator)
{
    EXPECT_EQ(hot::div(7, 2), 3);
    EXPECT_EQ(hot::div(-7, 2), -3);
    EXPECT_EQ(hot::mod(-7, 2), -1);
    EXPECT_EQ(hot::div(7u, 2u), 3u);
    EXPECT_EQ(hot::div(int64_t(1) << 40, 2), int64_t(1) << 39);
    EXPECT_EQ(hot::div(-32768, int16_t(-1)), 32768);
}

TEST(Arith, MinimumDividedByMinusOneWraps)
{
    EXPECT_EQ(hot::div(std::numeric_limits<int>::min(), -1), std::numeric_limits<int>::min());
    EXPECT_EQ(hot::mod(std::numeric_limits<int>::min(), -1), 0);
    EXPECT_EQ(hot::div(std::numeric_limits<int64_t>::min(), int64_t(-1)), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(hot::mod(std::numeric_limits<int64_t>::min(), int64_t(-1)), 0);
    EXPECT_EQ(hot::div(5, -1), -5);
    EXPECT_EQ(hot::mod(5, -1), 0);
    // An unsigned value is never -1.
    EXPECT_EQ(hot::div(std::numeric_limits<uint32_t>::max(), 1u), std::numeric_limits<uint32_t>::max());
}

TEST(Arith, ResultTypeIsTheTypeOfTheOperator)
{
    static_assert(std::is_same<decltype(hot::div(int8_t(1), int8_t(1))), int>::value, "small types promote");
    static_assert(std::is_same<decltype(hot::div(int16_t(1), int16_t(1))), int>::value, "small types promote");
    static_assert(std::is_same<decltype(hot::mod(1, int64_t(1))), int64_t>::value, "int64");
    static_assert(std::is_same<decltype(hot::div(1u, 1)), unsigned>::value, "unsigned");
    static_assert(std::is_same<decltype(hot::div(1.0f, 2)), float>::value, "float");
    static_assert(std::is_same<decltype(hot::div(1.0, 2.0)), double>::value, "double");
    SUCCEED();
}

TEST(Arith, FloatsStayUnchecked)
{
    EXPECT_TRUE(std::isinf(hot::div(1.0f, 0.0f)));
    EXPECT_TRUE(std::isinf(hot::div(1.0, 0)));
    EXPECT_TRUE(std::isinf(hot::div(1, 0.0)));
    EXPECT_TRUE(std::isnan(hot::div(0.0, 0.0)));
}

TEST(Arith, CompoundFormsChangeTheTarget)
{
    int a = 9;
    EXPECT_EQ(hot::div_assign(a, 2), 4);
    EXPECT_EQ(a, 4);
    hot::mod_assign(a, 3);
    EXPECT_EQ(a, 1);
    EXPECT_THROW(hot::div_assign(a, 0), hot::ArithmeticError);
    EXPECT_THROW(hot::mod_assign(a, 0), hot::ArithmeticError);
    EXPECT_EQ(a, 1);
    int8_t b = -128;
    hot::div_assign(b, -1); // 128 does not fit: the target wraps
    EXPECT_EQ(b, int8_t(-128));
    int m = std::numeric_limits<int>::min();
    hot::div_assign(m, -1);
    EXPECT_EQ(m, std::numeric_limits<int>::min());
    hot::mod_assign(m, -1);
    EXPECT_EQ(m, 0);
    float f = 1.0f;
    hot::div_assign(f, 0);
    EXPECT_TRUE(std::isinf(f));
    int v[2] = {7, 9};
    int i = 0;
    auto next = [&i, &v]() -> int& { ++i; return v[1]; };
    hot::div_assign(next(), 3);
    EXPECT_EQ(i, 1);
    EXPECT_EQ(v[1], 3);
}

TEST(Arith, ConstantExpression)
{
    constexpr int q = hot::div(9, 2);
    constexpr int r = hot::mod(9, 2);
    EXPECT_EQ(q, 4);
    EXPECT_EQ(r, 1);
}

TEST(ArithShift, CountOutOfRangeThrows)
{
    EXPECT_THROW(hot::shl(1, 32), hot::ArithmeticError);
    EXPECT_THROW(hot::shr(1, 32), hot::ArithmeticError);
    EXPECT_THROW(hot::shl(1, -1), hot::ArithmeticError);
    EXPECT_THROW(hot::shr(1, -1), hot::ArithmeticError);
    EXPECT_THROW(hot::shl(1u, 32u), hot::ArithmeticError);
    EXPECT_THROW(hot::shl(int64_t(1), 64), hot::ArithmeticError);
    EXPECT_THROW(hot::shr(int64_t(1), int64_t(-1)), hot::ArithmeticError);
    EXPECT_THROW(hot::shl(int8_t(1), 32), hot::ArithmeticError); // int8_t promotes to int
    EXPECT_THROW(hot::shl(uint8_t(1), int8_t(-1)), hot::ArithmeticError);
    EXPECT_THROW(hot::shl(1, uint64_t(1) << 40), hot::ArithmeticError);
    EXPECT_THROW(hot::shl(1, 32), hot::ScriptError); // ArithmeticError is a ScriptError
    try
    {
        hot::shl(1, 32);
        FAIL();
    }
    catch (const hot::ArithmeticError& e)
    {
        EXPECT_STREQ(e.what(), "shift count out of range");
    }
}

TEST(ArithShift, CountInRangeGivesTheOperatorValue)
{
    EXPECT_EQ(hot::shl(1, 0), 1);
    EXPECT_EQ(hot::shl(1, 31), INT32_MIN); // into the sign bit: wraps
    EXPECT_EQ(hot::shl(3, 4), 48);
    EXPECT_EQ(hot::shr(48, 4), 3);
    EXPECT_EQ(hot::shr(1, 31), 0);
    EXPECT_EQ(hot::shl(uint32_t(1), 31), 0x80000000u);
    EXPECT_EQ(hot::shr(0x80000000u, 31), 1u);
    EXPECT_EQ(hot::shl(int64_t(1), 63), INT64_MIN);
    EXPECT_EQ(hot::shl(int64_t(1), 40), int64_t(1) << 40);
    EXPECT_EQ(hot::shr(int64_t(1) << 40, 40), 1);
    EXPECT_EQ(hot::shl(1, int8_t(5)), 32);
    EXPECT_EQ(hot::shl(1, uint64_t(5)), 32);
}

TEST(ArithShift, NegativeValues)
{
    EXPECT_EQ(hot::shl(-1, 1), -2);
    EXPECT_EQ(hot::shl(-1, 31), INT32_MIN);
    EXPECT_EQ(hot::shl(-1, 4), -16);
    EXPECT_EQ(hot::shl(INT32_MAX, 1), -2); // wraps
    EXPECT_EQ(hot::shl(INT32_MIN, 1), 0);
    EXPECT_EQ(hot::shl(int64_t(-1), 63), INT64_MIN);
    EXPECT_EQ(hot::shr(-8, 1), -4); // arithmetic shift
    EXPECT_EQ(hot::shr(-1, 31), -1);
    EXPECT_EQ(hot::shr(INT32_MIN, 31), -1);
    EXPECT_EQ(hot::shr(int64_t(-1), 63), -1);
}

TEST(ArithShift, SmallTypesPromoteToInt)
{
    static_assert(std::is_same<decltype(hot::shl(int8_t(1), 1)), int>::value, "the type of the operator");
    static_assert(std::is_same<decltype(hot::shr(uint16_t(1), 1)), int>::value, "the type of the operator");
    static_assert(std::is_same<decltype(hot::shl(1u, 1)), unsigned>::value, "the type of the operator");
    static_assert(std::is_same<decltype(hot::shl(int64_t(1), 1)), int64_t>::value, "the type of the operator");
    EXPECT_EQ(hot::shl(int8_t(1), 8), 256); // not cut to 8 bits
    EXPECT_EQ(hot::shl(int8_t(-1), 7), -128);
    EXPECT_EQ(hot::shl(uint8_t(255), 24), -16777216);
    EXPECT_EQ(hot::shr(int16_t(-32768), 15), -1);
    EXPECT_EQ(hot::shl(true, 3), 8);
}

TEST(ArithShift, CompoundForms)
{
    int a = 3;
    EXPECT_EQ(hot::shl_assign(a, 2), 12);
    EXPECT_EQ(a, 12);
    EXPECT_EQ(hot::shr_assign(a, 1), 6);
    EXPECT_EQ(a, 6);
    EXPECT_THROW(hot::shl_assign(a, 32), hot::ArithmeticError);
    EXPECT_THROW(hot::shr_assign(a, -1), hot::ArithmeticError);
    EXPECT_EQ(a, 6); // a failed shift does not change the target
    uint8_t b = 0x81;
    hot::shl_assign(b, 1);
    EXPECT_EQ(b, 0x02); // the result is cut to the type of the target
    int8_t c = -128;
    hot::shr_assign(c, 7);
    EXPECT_EQ(c, -1);
    int64_t d = 1;
    hot::shl_assign(d, 40);
    EXPECT_EQ(d, int64_t(1) << 40);
    EXPECT_THROW(hot::shl_assign(d, 64), hot::ArithmeticError);
}

namespace
{
struct Bits
{
    int v;
    Bits operator<<(int n) const { return Bits{v * 10 + n}; }
    Bits operator>>(int n) const { return Bits{v * 100 + n}; }
    Bits& operator<<=(int n) { v += n; return *this; }
    Bits& operator>>=(int n) { v -= n; return *this; }
};
} // namespace

TEST(ArithShift, OtherTypesUseTheOperator)
{
    EXPECT_EQ(hot::shl(Bits{1}, 40).v, 50); // a count of 40 is fine for a class
    EXPECT_EQ(hot::shr(Bits{1}, -1).v, 99);
    Bits b{5};
    hot::shl_assign(b, 3);
    EXPECT_EQ(b.v, 8);
    hot::shr_assign(b, 2);
    EXPECT_EQ(b.v, 6);
    std::ostringstream os;
    hot::shl(os, 5);
    hot::shl(hot::shl(os, " "), "x");
    EXPECT_EQ(os.str(), "5 x");
}

TEST(ArithFloatToInt, InRangeCutsTheFraction)
{
    EXPECT_EQ(hot::float_to_int<int>(3.9f), 3);
    EXPECT_EQ(hot::float_to_int<int>(-3.9f), -3);
    EXPECT_EQ(hot::float_to_int<int>(3.9), 3);
    EXPECT_EQ(hot::float_to_int<int>(-3.9), -3);
    EXPECT_EQ(hot::float_to_int<int>(0.0), 0);
    EXPECT_EQ(hot::float_to_int<int>(-0.5), 0);
    EXPECT_EQ(hot::float_to_int<int>(2147483520.0f), 2147483520);
    EXPECT_EQ(hot::float_to_int<int>(-2147483648.0), INT32_MIN);
    EXPECT_EQ(hot::float_to_int<int64_t>(1e18), int64_t(1000000000000000000));
    EXPECT_EQ(hot::float_to_int<uint8_t>(200.7), 200);
    EXPECT_EQ(hot::float_to_int<unsigned>(4294967040.0f), 4294967040u);
    EXPECT_EQ(hot::float_to_int<unsigned>(-0.5), 0u);
}

TEST(ArithFloatToInt, OutOfRangeSaturates)
{
    EXPECT_EQ(hot::float_to_int<int>(1e20), INT32_MAX);
    EXPECT_EQ(hot::float_to_int<int>(-1e20), INT32_MIN);
    EXPECT_EQ(hot::float_to_int<int>(1e20f), INT32_MAX);
    EXPECT_EQ(hot::float_to_int<int>(2147483648.0), INT32_MAX);
    EXPECT_EQ(hot::float_to_int<int>(2147483647.5), INT32_MAX);
    EXPECT_EQ(hot::float_to_int<int>(-2147483649.0), INT32_MIN);
    EXPECT_EQ(hot::float_to_int<int>(std::numeric_limits<float>::infinity()), INT32_MAX);
    EXPECT_EQ(hot::float_to_int<int>(-std::numeric_limits<double>::infinity()), INT32_MIN);
    EXPECT_EQ(hot::float_to_int<int64_t>(1e30), INT64_MAX);
    EXPECT_EQ(hot::float_to_int<int64_t>(-1e30), INT64_MIN);
    EXPECT_EQ(hot::float_to_int<int64_t>(9223372036854775808.0), INT64_MAX);
    EXPECT_EQ(hot::float_to_int<int8_t>(300.0), 127);
    EXPECT_EQ(hot::float_to_int<int8_t>(-300.0f), -128);
    EXPECT_EQ(hot::float_to_int<int16_t>(40000.0), 32767);
    EXPECT_EQ(hot::float_to_int<uint8_t>(256.0), 255);
    EXPECT_EQ(hot::float_to_int<unsigned>(5e9), 4294967295u);
    EXPECT_EQ(hot::float_to_int<unsigned>(-5.0), 0u);
    EXPECT_EQ(hot::float_to_int<uint64_t>(1e30), UINT64_MAX);
    EXPECT_EQ(hot::float_to_int<uint64_t>(-1e30), 0u);
}

TEST(ArithFloatToInt, NaNGivesZero)
{
    EXPECT_EQ(hot::float_to_int<int>(std::numeric_limits<float>::quiet_NaN()), 0);
    EXPECT_EQ(hot::float_to_int<int64_t>(std::numeric_limits<double>::quiet_NaN()), 0);
    EXPECT_EQ(hot::float_to_int<unsigned>(std::nan("")), 0u);
    EXPECT_EQ(hot::float_to_int<int8_t>(std::nan("")), 0);
}

namespace
{
enum class Color { Red = 3 };
} // namespace

namespace
{
struct IntLike
{
    int v;
    operator int() { return v; } // not const
};
struct Unrelated
{
};
struct Fields
{
    int bf : 5;
    unsigned ubf : 3;
};

// True when hot::float_to_int<To>(From) compiles.
template <class To, class From, class = void>
constexpr bool converts = false;
template <class To, class From>
constexpr bool converts<To, From, std::void_t<decltype(hot::float_to_int<To>(std::declval<From>()))>> = true;
} // namespace

TEST(ArithFloatToInt, OtherTypesUseTheStaticCast)
{
    EXPECT_EQ(hot::float_to_int<int>(7), 7);
    EXPECT_EQ(hot::float_to_int<int8_t>(300), int8_t(44)); // the cast wraps
    EXPECT_EQ(hot::float_to_int<int>(Color::Red), 3);
    EXPECT_EQ(hot::float_to_int<bool>(0.5), true);
    EXPECT_EQ(hot::float_to_int<bool>(0.0), false);
    EXPECT_EQ(hot::float_to_int<int>(IntLike{9}), 9);
    EXPECT_EQ(hot::float_to_int<int>(size_t(12)), 12);
    EXPECT_EQ(hot::float_to_int<size_t>(12), 12u);
    static_assert(hot::float_to_int<int>(3.9) == 3, "constexpr");
}

TEST(ArithFloatToInt, TypesThatStaticCastRejectDoNotCompile)
{
    static_assert(converts<int, double>, "float");
    static_assert(converts<uint64_t, float>, "float");
    static_assert(converts<int, Color>, "enum");
    static_assert(converts<int, IntLike>, "class with operator int");
    static_assert(converts<size_t, int>, "size_t");
    static_assert(converts<int, size_t>, "size_t");
    static_assert(converts<bool, double>, "bool");
    static_assert(converts<uint64_t, int*>, "pointer");
    static_assert(converts<uintptr_t, const char*>, "pointer");
    static_assert(converts<bool, int*>, "pointer to bool");
    static_assert(!converts<uint8_t, int*>, "pointer to a type that is too small");
    static_assert(!converts<int*, uint64_t>, "integer to pointer");
    static_assert(!converts<int*, double>, "float to pointer");
    static_assert(!converts<int, Unrelated>, "unrelated class");
    SUCCEED();
}

TEST(ArithFloatToInt, PointerKeepsItsAddress)
{
    int x = 0;
    EXPECT_EQ(hot::float_to_int<uintptr_t>(&x), reinterpret_cast<uintptr_t>(&x));
    EXPECT_EQ(hot::float_to_int<uint64_t>(&x), uint64_t(reinterpret_cast<uintptr_t>(&x)));
    const char* p = "a";
    EXPECT_EQ(hot::float_to_int<intptr_t>(p), reinterpret_cast<intptr_t>(p));
    EXPECT_NE(hot::float_to_int<uintptr_t>(&x), 0u);
    EXPECT_TRUE(hot::float_to_int<bool>(&x));
    EXPECT_EQ(hot::float_to_int<uintptr_t>(static_cast<int*>(nullptr)), 0u);
}

TEST(ArithFloatToInt, BitFieldArgument)
{
    Fields s{};
    s.bf = -5;
    s.ubf = 6;
    EXPECT_EQ(hot::float_to_int<int>(s.bf), -5);
    EXPECT_EQ(hot::float_to_int<int>(s.ubf), 6);
    EXPECT_EQ(hot::float_to_int<uint8_t>(s.bf), uint8_t(251));
    EXPECT_EQ(hot::shl(s.ubf, 1), 12); // the shift takes the value, so a bit-field works
    EXPECT_EQ(hot::shr(s.bf, 1), -3);
}
