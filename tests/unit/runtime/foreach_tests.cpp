// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::Iter, the run-time guard of a "foreach" loop (foreach.h).
#include "hot/runtime/foreach.h"

#include <gtest/gtest.h>

#include <array>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

static_assert(hot::detail::is_indexed<std::vector<int>>::value);
static_assert(hot::detail::is_indexed<int[3]>::value);
static_assert(hot::detail::is_indexed<const std::string>::value);
static_assert(!hot::detail::is_indexed<std::map<int, int>>::value);
static_assert(hot::detail::is_unique_keyed<std::map<int, int>>::value);
static_assert(hot::detail::is_unique_keyed<std::set<int>>::value);
static_assert(!hot::detail::is_unique_keyed<std::multimap<int, int>>::value);

template <class C> std::vector<int> walk(C& c)
{
    std::vector<int> out;
    for (auto i = hot::iter(c); i.next();)
        out.push_back(i.ref());
    return out;
}

TEST(Foreach, VectorWalksAndWritesElements)
{
    std::vector<int> xs{1, 2, 3};
    for (auto i = hot::iter(xs); i.next();)
        i.ref() *= 10;
    EXPECT_EQ(xs, (std::vector<int>{10, 20, 30}));
    EXPECT_EQ(walk(xs), (std::vector<int>{10, 20, 30}));
}

TEST(Foreach, VectorPushBackThrows)
{
    std::vector<int> xs{1, 2, 3};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(xs); i.next();)
                xs.push_back(i.ref());
        },
        hot::ContainerChanged);
}

TEST(Foreach, RefAfterPushBackThrows)
{
    std::vector<int> xs{1, 2, 3};
    auto i = hot::iter(xs);
    ASSERT_TRUE(i.next());
    xs.push_back(4);
    EXPECT_THROW(i.ref() = 2, hot::ContainerChanged);
}

TEST(Foreach, EraseClearAndAssignThrow)
{
    std::vector<int> xs{1, 2, 3};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(xs); i.next();)
                xs.erase(xs.begin());
        },
        hot::ContainerChanged);
    xs = {1, 2, 3};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(xs); i.next();)
                xs.clear();
        },
        hot::ContainerChanged);
    xs = {1, 2, 3};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(xs); i.next();)
                xs = std::vector<int>(4, 1);
        },
        hot::ContainerChanged);
}

TEST(Foreach, SameSizeMoveAssignIsSafe)
{
    // A move assignment gives a new buffer. The index walk reads the new elements.
    std::vector<int> xs{1, 2, 3};
    std::vector<int> seen;
    for (auto i = hot::iter(xs); i.next();)
    {
        seen.push_back(i.ref());
        xs = std::vector<int>{7, 8, 9};
    }
    EXPECT_EQ(seen, (std::vector<int>{1, 8, 9}));
}

TEST(Foreach, StringAndArray)
{
    std::string s = "ab";
    std::string out;
    for (auto i = hot::iter(s); i.next();)
        out += i.ref();
    EXPECT_EQ(out, "ab");
    EXPECT_THROW(
        {
            for (auto i = hot::iter(s); i.next();)
                s += 'c';
        },
        hot::ContainerChanged);
    int a[3] = {1, 2, 3};
    EXPECT_EQ(walk(a), (std::vector<int>{1, 2, 3}));
    std::array<int, 2> b{4, 5};
    EXPECT_EQ(walk(b), (std::vector<int>{4, 5}));
}

TEST(Foreach, MapWalksAndWritesValues)
{
    std::map<int, int> m{{1, 10}, {2, 20}};
    for (auto i = hot::iter(m); i.next();)
        i.ref().second += 1;
    EXPECT_EQ(m.at(1), 11);
    EXPECT_EQ(m.at(2), 21);
}

TEST(Foreach, MapInsertEraseThrows)
{
    std::map<int, int> m{{1, 10}, {2, 20}};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(m); i.next();)
                m[i.ref().first + 100] = 0;
        },
        hot::ContainerChanged);
    m = {{1, 10}, {2, 20}};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(m); i.next();)
                m.erase(i.ref().first);
        },
        hot::ContainerChanged);
}

TEST(Foreach, MapEraseCurrentAndInsertSameSizeThrows)
{
    // The size does not change. The key of the current element is gone: no old iterator is used.
    std::map<int, int> m{{1, 10}, {2, 20}};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(m); i.next();)
            {
                const int k = i.ref().first;
                m.erase(k);
                m[k + 100] = 0;
            }
        },
        hot::ContainerChanged);
}

TEST(Foreach, UnorderedMapRehashThrows)
{
    std::unordered_map<int, int> m{{1, 10}};
    EXPECT_THROW(
        {
            for (auto i = hot::iter(m); i.next();)
            {
                m.erase(i.ref().first);
                m.rehash(m.bucket_count() * 8 + 64);
                m[5] = 1;
            }
        },
        hot::ContainerChanged);
}

TEST(Foreach, SetWalks)
{
    std::set<int> s{3, 1, 2};
    std::vector<int> out;
    for (auto i = hot::iter(s); i.next();)
        out.push_back(i.ref());
    EXPECT_EQ(out, (std::vector<int>{1, 2, 3}));
}

TEST(Foreach, TemporaryListIsKept)
{
    std::vector<int> out;
    for (auto i = hot::iter(std::list<int>{1, 2}); i.next();)
        out.push_back(i.ref());
    EXPECT_EQ(out, (std::vector<int>{1, 2}));
}

TEST(Foreach, PathIsEvaluatedAtEachStep)
{
    std::vector<std::vector<int>> rows{{1, 2}, {3}};
    int steps = 0;
    EXPECT_THROW(
        {
            for (auto i = hot::iter_path([&]() -> decltype(auto) { return (rows[0]); }); i.next();)
            {
                ++steps;
                rows.push_back({}); // the outer buffer moves: the old address of rows[0] is not used
            }
        },
        hot::ContainerChanged);
    EXPECT_EQ(steps, 1);
}

TEST(Foreach, EnumerateGivesIndexAndElement)
{
    std::vector<int> xs{5, 6};
    std::vector<std::size_t> idx;
    for (auto i = hot::iter(xs); i.next();)
    {
        idx.push_back(i.index());
        i.item().second += 1;
    }
    EXPECT_EQ(idx, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(xs, (std::vector<int>{6, 7}));
    EXPECT_THROW(
        {
            for (auto i = hot::iter(xs); i.next();)
                xs.push_back(0);
        },
        hot::ContainerChanged);
}

TEST(Foreach, Text)
{
    EXPECT_STREQ(hot::ContainerChanged().what(), "container changed during foreach");
    EXPECT_TRUE((std::is_base_of_v<hot::ScriptError, hot::ContainerChanged>));
}

} // namespace
