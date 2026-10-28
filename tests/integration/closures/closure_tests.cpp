// Tests of the script closure: the pin count follows copies and moves.
// The tests do not use a backend.
#include <gtest/gtest.h>

#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

#include "Actor.h"
#include "spp/script_runtime.h"

namespace {

class Closure : public ::testing::Test {
protected:
    void SetUp() override { script::active_generation = 9001; }
    void TearDown() override { script::active_generation = 0; }
};

TEST_F(Closure, PinsFollowTheCopiesAndMoves)
{
    EXPECT_EQ(script::pin_count(9001), 0u);
    {
        script::Closure<void()> a(9001, "a", [] {});
        EXPECT_EQ(script::pin_count(9001), 1u);
        script::Closure<void()> b = a;
        EXPECT_EQ(script::pin_count(9001), 2u);
        script::Closure<void()> c = std::move(b);
        EXPECT_EQ(script::pin_count(9001), 2u);
        a = c;
        EXPECT_EQ(script::pin_count(9001), 2u);
        std::function<void()> f = c;  // a std::function holds one more pin
        EXPECT_EQ(script::pin_count(9001), 3u);
    }
    EXPECT_EQ(script::pin_count(9001), 0u);
}

TEST_F(Closure, GuardRunsTheBodyOnlyInTheLiveGeneration)
{
    int runs = 0;
    script::Closure<int(int)> c(9001, "c", [&runs](int x) { ++runs; return x + 1; });
    EXPECT_EQ(c(1), 2);
    EXPECT_EQ(runs, 1);
    const uint64_t stale0 = script::stale_call_count();
    script::active_generation = 9002;
    EXPECT_EQ(c(1), 0);  // R(): a default value
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(script::stale_call_count(), stale0 + 1);
    script::active_generation = 9001;
    EXPECT_EQ(c(5), 6);
}

TEST_F(Closure, ConvertsToStdFunctionAndToADelegate)
{
    static_assert(std::is_convertible_v<script::Closure<void(float)>, std::function<void(float)>>);
    float seen = 0;
    auto c = script::make_closure(9001, "m", [&seen](float x) { seen = x; });
    static_assert(std::is_same_v<decltype(c), script::Closure<void(float)>>);
    Delegate<void(float)> d = c;
    ASSERT_TRUE(d.IsBound());
    d(2.5f);
    EXPECT_EQ(seen, 2.5f);
    std::function<void(float)> f = c;
    f(4.0f);
    EXPECT_EQ(seen, 4.0f);
    EXPECT_EQ(script::pin_count(9001), 3u);  // c, d, f
}

TEST_F(Closure, PinCountIsAtomic)
{
    // Not a proof, but a thread sanitizer or a lost update shows here.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([] {
            for (int i = 0; i < 10000; ++i) {
                script::pin(9100);
                script::unpin(9100);
            }
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(script::pin_count(9100), 0u);
}

}  // namespace
