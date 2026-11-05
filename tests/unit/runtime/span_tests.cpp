// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::span, the C++17 view of a contiguous range.
#include "hot/runtime/span.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

static_assert(!std::is_constructible_v<hot::span<int>, std::vector<int>>);
static_assert(std::is_constructible_v<hot::span<const int>, std::vector<int>>);
static_assert(std::is_constructible_v<hot::span<int>, std::vector<int>&>);
static_assert(!std::is_constructible_v<hot::span<int>, const std::vector<int>&>);
static_assert(std::is_constructible_v<hot::span<const int>, const std::vector<int>&>);
static_assert(std::is_trivially_copyable_v<hot::span<int>>);
static_assert(std::is_convertible_v<hot::span<int>, hot::span<const int>>);
static_assert(!std::is_convertible_v<hot::span<const int>, hot::span<int>>);
static_assert(hot::span<int>::extent == hot::dynamic_extent);

TEST(Span, ZeroCountIsNotAmbiguous)
{
    // 0 converts to a pointer and to size_t. The second argument 0 is a count, as in std::span.
    int a[3] = {1, 2, 3};
    int* p = a;
    hot::span<int> s(p, 0);
    EXPECT_EQ(s.data(), p);
    EXPECT_EQ(s.size(), 0u);
    hot::span<int> t{p, 0};
    EXPECT_TRUE(t.empty());
    hot::span<int> u = {p, 0};
    EXPECT_TRUE(u.empty());
}

TEST(Span, PointerAndSizeTCount)
{
    int a[3] = {1, 2, 3};
    const std::size_t n = 2;
    hot::span<int> s(a, n);
    EXPECT_EQ(s.size(), 2u);
    EXPECT_EQ(s[1], 2);
}

TEST(Span, FirstAndLastPointers)
{
    int a[4] = {1, 2, 3, 4};
    hot::span<int> s(a, a + 3);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s[2], 3);
    int* first = a + 1;
    int* last = a + 4;
    hot::span<int> t(first, last);
    EXPECT_EQ(t.size(), 3u);
    const int* clast = a + 2;
    hot::span<const int> c(static_cast<const int*>(a), clast);
    EXPECT_EQ(c.size(), 2u);
}

TEST(Span, DefaultIsEmpty)
{
    hot::span<int> s;
    EXPECT_EQ(s.data(), nullptr);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.begin(), s.end());
}

TEST(Span, PointerAndSize)
{
    int a[3] = {1, 2, 3};
    hot::span<int> s(a, 3);
    EXPECT_EQ(s.data(), a);
    EXPECT_EQ(s.size(), 3u);
    hot::span<int> r(a, a + 2);
    EXPECT_EQ(r.size(), 2u);
}

TEST(Span, FromCArray)
{
    int a[4] = {1, 2, 3, 4};
    hot::span<int> s = a;
    EXPECT_EQ(s.size(), 4u);
    EXPECT_EQ(s.data(), a);
    const int c[2] = {5, 6};
    hot::span<const int> cs = c;
    EXPECT_EQ(cs.size(), 2u);
    hot::span<const int> ca = a;
    EXPECT_EQ(ca.size(), 4u);
}

TEST(Span, FromStdArray)
{
    std::array<int, 3> a{1, 2, 3};
    hot::span<int> s = a;
    EXPECT_EQ(s.size(), 3u);
    s[0] = 7;
    EXPECT_EQ(a[0], 7);
    const std::array<int, 3> c{4, 5, 6};
    hot::span<const int> cs = c;
    EXPECT_EQ(cs.size(), 3u);
    EXPECT_EQ(cs[2], 6);
}

TEST(Span, FromVectorLvalue)
{
    std::vector<int> v{1, 2, 3};
    hot::span<int> s = v;
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.data(), v.data());
    s[1] = 8;
    EXPECT_EQ(v[1], 8);
}

TEST(Span, FromConstVector)
{
    const std::vector<int> v{1, 2, 3};
    hot::span<const int> s = v;
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.data(), v.data());
}

TEST(Span, FromRvalueRangeOnlyForConst)
{
    auto sum = [](hot::span<const int> s)
    {
        int r = 0;
        for (int x : s)
            r += x;
        return r;
    };
    EXPECT_EQ(sum(std::vector<int>{1, 2, 3}), 6);
}

TEST(Span, FromString)
{
    std::string str = "abc";
    hot::span<char> s = str;
    EXPECT_EQ(s.size(), 3u);
    s[0] = 'x';
    EXPECT_EQ(str, "xbc");
}

TEST(Span, ConvertsToConst)
{
    std::vector<int> v{1, 2, 3};
    hot::span<int> s = v;
    hot::span<const int> cs = s;
    EXPECT_EQ(cs.data(), v.data());
    EXPECT_EQ(cs.size(), 3u);
}

TEST(Span, RangeFor)
{
    std::vector<int> v{1, 2, 3, 4};
    hot::span<int> s = v;
    int sum = 0;
    for (int x : s)
        sum += x;
    EXPECT_EQ(sum, 10);
    int rsum = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it)
        rsum = rsum * 10 + *it;
    EXPECT_EQ(rsum, 4321);
}

TEST(Span, Accessors)
{
    int a[3] = {10, 20, 30};
    hot::span<int> s = a;
    EXPECT_EQ(s.front(), 10);
    EXPECT_EQ(s.back(), 30);
    EXPECT_EQ(s[1], 20);
    EXPECT_EQ(s.data(), a);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.size_bytes(), 3 * sizeof(int));
    EXPECT_FALSE(s.empty());
}

TEST(Span, FirstLastSubspan)
{
    int a[5] = {1, 2, 3, 4, 5};
    hot::span<int> s = a;
    auto f = s.first(2);
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f[1], 2);
    auto l = s.last(2);
    EXPECT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0], 4);
    auto m = s.subspan(1, 3);
    EXPECT_EQ(m.size(), 3u);
    EXPECT_EQ(m.front(), 2);
    EXPECT_EQ(m.back(), 4);
    auto t = s.subspan(2);
    EXPECT_EQ(t.size(), 3u);
    EXPECT_EQ(t.front(), 3);
    EXPECT_TRUE(s.subspan(5).empty());
}

TEST(Span, DeductionGuides)
{
    int a[3] = {1, 2, 3};
    hot::span sa(a);
    static_assert(std::is_same_v<decltype(sa), hot::span<int>>);
    std::array<int, 3> arr{1, 2, 3};
    hot::span sarr(arr);
    static_assert(std::is_same_v<decltype(sarr), hot::span<int>>);
    const std::array<int, 3> carr{1, 2, 3};
    hot::span scarr(carr);
    static_assert(std::is_same_v<decltype(scarr), hot::span<const int>>);
    std::vector<int> v{1, 2, 3};
    hot::span sv(v);
    static_assert(std::is_same_v<decltype(sv), hot::span<int>>);
    const std::vector<int> cv{1, 2, 3};
    hot::span scv(cv);
    static_assert(std::is_same_v<decltype(scv), hot::span<const int>>);
    hot::span sp(a, 2);
    static_assert(std::is_same_v<decltype(sp), hot::span<int>>);
    hot::span copy(sa);
    static_assert(std::is_same_v<decltype(copy), hot::span<int>>);
    EXPECT_EQ(sa.size(), 3u);
    EXPECT_EQ(sv.size(), 3u);
    EXPECT_EQ(sp.size(), 2u);
}

} // namespace
