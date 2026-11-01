// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::coalesce. The translator writes "x ?? y" as a call with a lambda for "y".
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>

namespace
{

struct Foo
{
    int v = 0;
};

TEST(Coalesce, TruthyLeftSkipsTheRightSide)
{
    int runs = 0;
    auto a = std::make_shared<Foo>();
    const auto r = hot::coalesce(a, [&]() -> decltype(auto) { return (++runs, std::make_shared<Foo>()); });
    EXPECT_EQ(r, a);
    EXPECT_EQ(runs, 0);
}

TEST(Coalesce, FalsyLeftRunsTheRightSideOnce)
{
    int runs = 0;
    std::shared_ptr<Foo> a;
    const auto r = hot::coalesce(a, [&]() -> decltype(auto) { return (++runs, std::make_shared<Foo>()); });
    EXPECT_NE(r, nullptr);
    EXPECT_EQ(runs, 1);
}

TEST(Coalesce, NullRightSideGivesTheCommonType)
{
    int i = 0;
    int* p = &i;
    int* n = nullptr;
    int* r1 = hot::coalesce(p, [&]() -> decltype(auto) { return (nullptr); });
    int* r2 = hot::coalesce(n, [&]() -> decltype(auto) { return (nullptr); });
    EXPECT_EQ(r1, &i);
    EXPECT_EQ(r2, nullptr);
}

TEST(Coalesce, LvalueRightSideIsCopied)
{
    std::shared_ptr<Foo> a, b = std::make_shared<Foo>();
    const auto r = hot::coalesce(a, [&]() -> decltype(auto) { return (b); });
    EXPECT_EQ(r, b);
}

TEST(Coalesce, OptionalLeftWithPlainRightGivesTheValueType)
{
    std::optional<int> some = 7, none;
    const int r1 = hot::coalesce(some, []() -> auto { return 5; });
    const int r2 = hot::coalesce(none, []() -> auto { return 5; });
    EXPECT_EQ(r1, 7);
    EXPECT_EQ(r2, 5);
    static_assert(std::is_same_v<decltype(hot::coalesce(some, std::declval<int (&)()>())), int>);
}

TEST(Coalesce, OptionalOnBothSidesKeepsTheOptional)
{
    std::optional<int> none, other = 3;
    const auto r = hot::coalesce(none, [&]() -> auto { return other; });
    EXPECT_EQ(r, std::optional<int>(3));
}

TEST(Coalesce, PrvalueMemberOnTheRightSideDoesNotDangle)
{
    struct Holder
    {
        std::shared_ptr<Foo> h = std::make_shared<Foo>();
    };
    std::shared_ptr<Foo> a;
    const auto r = hot::coalesce(a, [&]() -> auto { return (Holder().h); });
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->v, 0);
}

} // namespace
