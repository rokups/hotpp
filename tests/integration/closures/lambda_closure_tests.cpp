// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the box closure of a reloadable lambda (hot/runtime/lambda.h). The tests do not use a
// backend.
#include "hot/runtime.h"
#include "hot/runtime/lambda.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

struct TagAdd
{
    static constexpr const char* key = "Test::Add#0";
};
struct TagCount
{
    static constexpr const char* key = "Test::Count#0";
};

int g_alive = 0;     // the count of Probe objects that exist
int g_destroyed = 0; // the count of Probe destructions

// A capture that counts its objects and its destructions.
struct Probe
{
    int value;
    explicit Probe(int v) : value(v) { ++g_alive; }
    Probe(const Probe& o) : value(o.value) { ++g_alive; }
    Probe(Probe&& o) noexcept : value(o.value) { ++g_alive; }
    ~Probe()
    {
        --g_alive;
        ++g_destroyed;
    }
};

class LambdaClosure : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_alive = 0;
        g_destroyed = 0;
        hot::active_generation = 9301;
    }
    void TearDown() override { hot::active_generation = 0; }
};

hot::LambdaClosure<int(int)> make_add(uint32_t gen, int a)
{
    return hot::make_lambda<TagAdd, int(int)>(gen, hot::caps(a), [](auto& c, int p) { return std::get<0>(c) + p; });
}

hot::LambdaClosure<int()> make_probe(uint32_t gen, int v)
{
    return hot::make_lambda<TagCount, int()>(
        gen, hot::caps(Probe(v)), [](auto& c) { return std::get<0>(c).value++; });
}

TEST_F(LambdaClosure, SizeIsTwoWordsAndCopyable)
{
    static_assert(sizeof(hot::LambdaClosure<int(int)>) == 16);
    static_assert(std::is_copy_constructible_v<hot::LambdaClosure<int(int)>>);
    static_assert(std::is_nothrow_move_constructible_v<hot::LambdaClosure<int(int)>>);
    EXPECT_EQ(sizeof(hot::LambdaClosure<void()>), 16u);
}

TEST_F(LambdaClosure, CallRunsTheBody)
{
    auto c = make_add(9301, 5);
    EXPECT_EQ(c(2), 7);
}

TEST_F(LambdaClosure, CopySharesTheBoxAndTheCaptures)
{
    auto a = make_probe(9301, 10);
    auto b = a;
    EXPECT_EQ(a(), 10);
    EXPECT_EQ(b(), 11); // one capture block: the change shows in both
    EXPECT_EQ(a(), 12);
}

TEST_F(LambdaClosure, RefCountFreesTheCapturesOnce)
{
    const size_t boxes0 = hot::lambda_box_count();
    {
        auto a = make_probe(9301, 1);
        EXPECT_EQ(g_alive, 1);
        EXPECT_EQ(hot::lambda_box_count(), boxes0 + 1);
        {
            auto b = a;
            auto c = std::move(b);
            auto d = c;
            a = d;
            EXPECT_EQ(g_alive, 1);
            EXPECT_EQ(hot::lambda_box_count(), boxes0 + 1);
        }
        EXPECT_EQ(g_alive, 1);
    }
    EXPECT_EQ(g_alive, 0);
    EXPECT_EQ(hot::lambda_box_count(), boxes0);
    auto e = make_probe(9301, 2);
    EXPECT_EQ(g_alive, 1);
    e = hot::LambdaClosure<int()>();
    EXPECT_EQ(g_alive, 0);
    EXPECT_EQ(hot::lambda_box_count(), boxes0);
}

TEST_F(LambdaClosure, ConstructorsPinTheGeneration)
{
    EXPECT_EQ(hot::pin_count(9301), 0u);
    {
        auto a = make_add(9301, 1);
        EXPECT_EQ(hot::pin_count(9301), 1u);
        auto b = a;
        EXPECT_EQ(hot::pin_count(9301), 2u);
        auto c = std::move(b); // the move gives its pin to the new closure
        EXPECT_EQ(hot::pin_count(9301), 2u);
        a = c;
        EXPECT_EQ(hot::pin_count(9301), 2u);
        std::function<int(int)> f = c; // the std::function holds a copy
        EXPECT_EQ(hot::pin_count(9301), 3u);
        EXPECT_EQ(f(1), 2);
    }
    EXPECT_EQ(hot::pin_count(9301), 0u);
}

TEST_F(LambdaClosure, DetachedBoxCountsAsStale)
{
    auto a = make_probe(9301, 3);
    auto b = a;
    const uint64_t stale0 = hot::stale_call_count();
    int detached = 0;
    hot::for_each_lambda_box([&](hot::LambdaBox* box, const hot::LambdaRecord* rec)
    {
        if (std::string(rec->key) == "Test::Count#0")
        {
            hot::lambda_box_detach(box);
            ++detached;
        }
    });
    EXPECT_EQ(detached, 1);
    EXPECT_EQ(a(), 0); // R()
    EXPECT_EQ(b(), 0);
    EXPECT_EQ(hot::stale_call_count(), stale0 + 2);
    EXPECT_EQ(g_alive, 0); // detach destroyed the captures
    const int d = g_destroyed;
    a = hot::LambdaClosure<int()>();
    b = hot::LambdaClosure<int()>(); // the last release does not destroy the captures again
    EXPECT_EQ(g_destroyed, d);
    EXPECT_EQ(g_alive, 0);
}

TEST_F(LambdaClosure, StdFunctionCopySharesState)
{
    std::function<int()> f = make_probe(9301, 100);
    std::function<int()> g = f;
    EXPECT_EQ(f(), 100);
    EXPECT_EQ(g(), 101);
    EXPECT_EQ(hot::pin_count(9301), 2u); // each closure copy has its pin, the box is shared
}

TEST_F(LambdaClosure, EmptyClosureCountsAsStale)
{
    hot::LambdaClosure<int(int)> c;
    EXPECT_FALSE(c);
    const uint64_t stale0 = hot::stale_call_count();
    EXPECT_EQ(c(4), 0);
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
}

} // namespace
