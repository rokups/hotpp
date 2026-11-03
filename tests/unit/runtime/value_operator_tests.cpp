// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the operators of hot::Value<T>. A test class with operators stands for a script class.
// The operators of a Value call the operators of the class on the objects.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace
{

struct Pt
{
    int x = 0;

    explicit Pt(int v) : x(v) {}

    hot::Value<Pt> operator+(const Pt& o) const { return hot::Value<Pt>(std::make_shared<Pt>(x + o.x)); }
    hot::Value<Pt> operator+(int o) const { return hot::Value<Pt>(std::make_shared<Pt>(x + o)); }
    hot::Value<Pt> operator-() const { return hot::Value<Pt>(std::make_shared<Pt>(-x)); }
    Pt& operator+=(const Pt& o)
    {
        x += o.x;
        return *this;
    }
    bool operator==(const Pt& o) const { return x == o.x; }
    bool operator!=(const Pt& o) const { return x != o.x; }
    bool operator<(const Pt& o) const { return x < o.x; }
    bool operator!() const { return x == 0; }
    int operator[](int i) const { return x * 10 + i; }
};

// A class with no operator at all.
struct Plain
{
};

template <class A, class B, class = void> struct can_add : std::false_type
{
};
template <class A, class B>
struct can_add<A, B, std::void_t<decltype(std::declval<A>() + std::declval<B>())>> : std::true_type
{
};
template <class A, class = void> struct can_negate : std::false_type
{
};
template <class A> struct can_negate<A, std::void_t<decltype(-std::declval<A>())>> : std::true_type
{
};
template <class A, class = void> struct can_index : std::false_type
{
};
template <class A> struct can_index<A, std::void_t<decltype(std::declval<A>()[0])>> : std::true_type
{
};

hot::Value<Pt> make(int v)
{
    return hot::Value<Pt>(std::make_shared<Pt>(v));
}

} // namespace

TEST(ValueOperators, BinaryOnTwoBoxes)
{
    auto a = make(2);
    auto b = make(5);
    hot::Value<Pt> c = a + b;
    EXPECT_EQ(c->x, 7);
    EXPECT_EQ((a + b + a)->x, 9); // the result of an operator is a box again
}

TEST(ValueOperators, BinaryWithNativeOperand)
{
    auto a = make(2);
    EXPECT_EQ((a + 40)->x, 42);
}

TEST(ValueOperators, Compare)
{
    auto a = make(2);
    auto b = make(2);
    auto c = make(3);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a != b);
    EXPECT_TRUE(a < c);
    EXPECT_FALSE(c < a);
}

TEST(ValueOperators, Unary)
{
    auto a = make(2);
    EXPECT_EQ((-a)->x, -2);
    EXPECT_FALSE(!a);
    EXPECT_TRUE(!make(0));
}

TEST(ValueOperators, Index)
{
    auto a = make(2);
    EXPECT_EQ(a[1], 21);
    const auto& r = a;
    EXPECT_EQ(r[3], 23);
}

TEST(ValueOperators, CompoundKeepsTheObject)
{
    auto a = make(2);
    auto b = make(5);
    const Pt* before = a.get();
    a += b;
    EXPECT_EQ(a->x, 7);
    EXPECT_EQ(a.get(), before);
    EXPECT_EQ(b->x, 5);
}

TEST(ValueOperators, NoMatchWithoutClassOperator)
{
    static_assert(can_add<hot::Value<Pt>, hot::Value<Pt>>::value, "Pt has operator+");
    static_assert(!can_add<hot::Value<Plain>, hot::Value<Plain>>::value, "Plain has no operator+");
    static_assert(!can_add<hot::Value<Plain>, int>::value, "Plain has no operator+");
    static_assert(!can_add<hot::Value<Pt>, hot::Value<Plain>>::value, "Pt has no operator+(Plain)");
    static_assert(can_negate<hot::Value<Pt>>::value, "Pt has operator-");
    static_assert(!can_negate<hot::Value<Plain>>::value, "Plain has no operator-");
    static_assert(can_index<hot::Value<Pt>>::value, "Pt has operator[]");
    static_assert(!can_index<hot::Value<Plain>>::value, "Plain has no operator[]");
    // The operators do not match when no operand is a Value.
    static_assert(!can_add<Plain, Plain>::value, "no operator");
    SUCCEED();
}

namespace
{

// A stand-in for a shell: assign_value reads "cls" and "state" only.
struct FakeShell : hot::ShellBase
{
};

} // namespace

TEST(ValueOperators, AssignValueOfOtherClassesThrowsAScriptError)
{
    hot::ClassRecord a{};
    hot::ClassRecord b{};
    a.name = "Base";
    b.name = "Derived";
    FakeShell dst;
    dst.cls = &b;
    auto made = std::make_shared<FakeShell>();
    made->cls = &a;
    hot::Value<FakeShell> src(made);
    try
    {
        hot::assign_value(dst, std::move(src));
        FAIL() << "no exception";
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_NE(std::string(e.what()).find("cannot assign an object of class Base"), std::string::npos) << e.what();
    }
}
