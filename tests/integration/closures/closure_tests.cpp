// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the script closure: the pin count follows copies and moves.
// The tests do not use a backend.
#include "Actor.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{

class Closure : public ::testing::Test
{
protected:
    void SetUp() override { hot::active_generation = 9001; }
    void TearDown() override { hot::active_generation = 0; }
};

TEST_F(Closure, PinsFollowTheCopiesAndMoves)
{
    EXPECT_EQ(hot::pin_count(9001), 0u);
    {
        hot::Closure<void()> a(9001, "a", [] {});
        EXPECT_EQ(hot::pin_count(9001), 1u);
        hot::Closure<void()> b = a;
        EXPECT_EQ(hot::pin_count(9001), 2u);
        hot::Closure<void()> c = std::move(b);
        EXPECT_EQ(hot::pin_count(9001), 2u);
        a = c;
        EXPECT_EQ(hot::pin_count(9001), 2u);
        std::function<void()> f = c; // a std::function holds one more pin
        EXPECT_EQ(hot::pin_count(9001), 3u);
    }
    EXPECT_EQ(hot::pin_count(9001), 0u);
}

TEST_F(Closure, GuardRunsTheBodyOnlyInTheLiveGeneration)
{
    int runs = 0;
    hot::Closure<int(int)> c(9001, "c", [&runs](int x)
    {
        ++runs;
        return x + 1;
    });
    EXPECT_EQ(c(1), 2);
    EXPECT_EQ(runs, 1);
    const uint64_t stale0 = hot::stale_call_count();
    hot::active_generation = 9002;
    EXPECT_EQ(c(1), 0); // R(): a default value
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
    hot::active_generation = 9001;
    EXPECT_EQ(c(5), 6);
}

TEST_F(Closure, ConvertsToStdFunctionAndToADelegate)
{
    static_assert(std::is_convertible_v<hot::Closure<void(float)>, std::function<void(float)>>);
    float seen = 0;
    auto c = hot::make_closure(9001, "m", [&seen](float x) { seen = x; });
    static_assert(std::is_same_v<decltype(c), hot::Closure<void(float)>>);
    Delegate<void(float)> d = c;
    ASSERT_TRUE(d.IsBound());
    d(2.5f);
    EXPECT_EQ(seen, 2.5f);
    std::function<void(float)> f = c;
    f(4.0f);
    EXPECT_EQ(seen, 4.0f);
    EXPECT_EQ(hot::pin_count(9001), 3u); // c, d, f
}

// A std::function from a closure holds the guarded lambda, not the closure. So its call has one
// indirect call. The pin and the guard stay with the std::function.
TEST_F(Closure, StdFunctionHoldsTheGuardedLambda)
{
    int runs = 0;
    hot::Closure<int(int)> c(9001, "c", [&runs](int x)
    {
        ++runs;
        return x + 1;
    });
    std::function<int(int)> from_lvalue = c;
    const hot::Closure<int(int)>& cref = c;
    std::function<int(int)> from_const = cref;
    std::function<int(int)> from_rvalue = hot::make_closure(9001, "r", [](int x) { return x * 2; });
    for (const std::function<int(int)>* f : {&from_lvalue, &from_const, &from_rvalue})
        EXPECT_NE(f->target_type(), typeid(hot::Closure<int(int)>));
    EXPECT_EQ(hot::pin_count(9001), 4u); // c, from_lvalue, from_const, from_rvalue
    EXPECT_EQ(from_lvalue(1), 2);
    EXPECT_EQ(from_rvalue(3), 6);
    const uint64_t stale0 = hot::stale_call_count();
    hot::active_generation = 9002;
    EXPECT_EQ(from_const(1), 0);
    EXPECT_EQ(from_rvalue(1), 0);
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(hot::stale_call_count(), stale0 + 2);
    hot::active_generation = 9001;
    // An empty closure gives a std::function that logs a stale call.
    std::function<int(int)> empty = hot::Closure<int(int)>();
    EXPECT_EQ(empty(1), 0);
    EXPECT_EQ(hot::stale_call_count(), stale0 + 3);
}

TEST_F(Closure, PinCountIsAtomic)
{
    // Not a proof, but a thread sanitizer or a lost update shows here.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([]
        {
            for (int i = 0; i < 10000; ++i)
            {
                hot::pin(9100);
                hot::unpin(9100);
            }
        });
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(hot::pin_count(9100), 0u);
}

} // namespace
