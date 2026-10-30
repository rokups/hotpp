// Tests of the script closure: the pin count follows copies and moves.
// The tests do not use a backend.
#include "Actor.h"
#include "spp/runtime.h"

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
    void SetUp() override { spp::active_generation = 9001; }
    void TearDown() override { spp::active_generation = 0; }
};

TEST_F(Closure, PinsFollowTheCopiesAndMoves)
{
    EXPECT_EQ(spp::pin_count(9001), 0u);
    {
        spp::Closure<void()> a(9001, "a", [] {});
        EXPECT_EQ(spp::pin_count(9001), 1u);
        spp::Closure<void()> b = a;
        EXPECT_EQ(spp::pin_count(9001), 2u);
        spp::Closure<void()> c = std::move(b);
        EXPECT_EQ(spp::pin_count(9001), 2u);
        a = c;
        EXPECT_EQ(spp::pin_count(9001), 2u);
        std::function<void()> f = c; // a std::function holds one more pin
        EXPECT_EQ(spp::pin_count(9001), 3u);
    }
    EXPECT_EQ(spp::pin_count(9001), 0u);
}

TEST_F(Closure, GuardRunsTheBodyOnlyInTheLiveGeneration)
{
    int runs = 0;
    spp::Closure<int(int)> c(9001, "c", [&runs](int x)
    {
        ++runs;
        return x + 1;
    });
    EXPECT_EQ(c(1), 2);
    EXPECT_EQ(runs, 1);
    const uint64_t stale0 = spp::stale_call_count();
    spp::active_generation = 9002;
    EXPECT_EQ(c(1), 0); // R(): a default value
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(spp::stale_call_count(), stale0 + 1);
    spp::active_generation = 9001;
    EXPECT_EQ(c(5), 6);
}

TEST_F(Closure, ConvertsToStdFunctionAndToADelegate)
{
    static_assert(std::is_convertible_v<spp::Closure<void(float)>, std::function<void(float)>>);
    float seen = 0;
    auto c = spp::make_closure(9001, "m", [&seen](float x) { seen = x; });
    static_assert(std::is_same_v<decltype(c), spp::Closure<void(float)>>);
    Delegate<void(float)> d = c;
    ASSERT_TRUE(d.IsBound());
    d(2.5f);
    EXPECT_EQ(seen, 2.5f);
    std::function<void(float)> f = c;
    f(4.0f);
    EXPECT_EQ(seen, 4.0f);
    EXPECT_EQ(spp::pin_count(9001), 3u); // c, d, f
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
                spp::pin(9100);
                spp::unpin(9100);
            }
        });
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(spp::pin_count(9100), 0u);
}

} // namespace
