// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::div, hot::mod, hot::div_assign and hot::mod_assign (arith.h).
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
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
