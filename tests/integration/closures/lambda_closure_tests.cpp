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

uint32_t g_live_a = 1; // the live flags of two modules for lambda_detach_module
uint32_t g_live_b = 1;

struct TagOuterA
{
    static constexpr const char* key = "Test::OuterA#0";
    static constexpr const char* what = "Test::OuterA lambda";
    static constexpr const char* names[] = {"inner", nullptr};
    static const uint32_t* live() { return &g_live_a; }
};
struct TagInnerA
{
    static constexpr const char* key = "Test::InnerA#0";
    static const uint32_t* live() { return &g_live_a; }
};
struct TagB
{
    static constexpr const char* key = "Test::B#0";
    static const uint32_t* live() { return &g_live_b; }
};

// A box whose capture is a closure of another box. Both boxes have the live flag of module A.
hot::LambdaClosure<int()> make_outer(uint32_t gen, int v)
{
    hot::LambdaClosure<int()> inner = hot::make_lambda<TagInnerA, int()>(
        gen, hot::caps(Probe(v)), [](auto& c) { return std::get<0>(c).value; });
    return hot::make_lambda_deduced<TagOuterA>(gen, hot::caps(std::move(inner)), [](auto& c) { return std::get<0>(c)() + 1; });
}

// The detach of the outer box destroys the inner closure, and so releases the inner box. The
// release takes the list lock. The callback runs without the lock, so this does not deadlock.
TEST_F(LambdaClosure, ForEachCanDetachABoxThatHoldsAnotherBox)
{
    auto outer = make_outer(9301, 7);
    EXPECT_EQ(outer(), 8);
    const size_t boxes = hot::lambda_box_count();
    hot::for_each_lambda_box([&](hot::LambdaBox* box, const hot::LambdaRecord* rec)
    {
        if (rec && std::string(rec->key) == "Test::OuterA#0")
            hot::lambda_box_detach(box);
    });
    EXPECT_EQ(g_alive, 0);
    EXPECT_EQ(hot::lambda_box_count(), boxes - 1); // the inner box is free
    const uint64_t stale0 = hot::stale_call_count();
    EXPECT_EQ(outer(), 0);
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
}

TEST_F(LambdaClosure, DetachModuleDetachesOnlyTheBoxesOfTheModule)
{
    auto outer = make_outer(9301, 7);
    auto b = hot::make_lambda_deduced<TagB>(9301, hot::caps(Probe(5)), [](auto& c) { return std::get<0>(c).value; });
    const size_t boxes = hot::lambda_box_count();
    EXPECT_EQ(hot::lambda_detach_module(&g_live_a), 2u); // the outer box and the inner box
    EXPECT_EQ(hot::lambda_box_count(), boxes - 1);
    EXPECT_EQ(g_alive, 1); // the capture of "b" stays
    EXPECT_EQ(b(), 5);
    testing::internal::CaptureStderr();
    EXPECT_EQ(outer(), 0);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("Test::OuterA lambda"), std::string::npos) << log; // the name stays after the detach
    EXPECT_EQ(hot::lambda_detach_module(&g_live_a), 0u); // a detached box has no record
    EXPECT_EQ(hot::lambda_detach_module(nullptr), 0u);
}

struct TagDeduced
{
    static constexpr const char* key = "Test::Deduced#0";
};

// The delegate of an engine: it binds a std::function.
struct Delegate
{
    std::function<void(float)> fn;
    void Bind(std::function<void(float)> f) { fn = std::move(f); }
};

TEST_F(LambdaClosure, DeducedSignatureComesFromTheBodyLambda)
{
    auto mul = hot::make_lambda_deduced<TagDeduced>(9301, hot::caps(3), [](auto& c, int x) { return x * std::get<0>(c); });
    static_assert(std::is_same_v<decltype(mul), hot::LambdaClosure<int(int)>>);
    EXPECT_EQ(mul(4), 12);
    float seen = 0;
    auto set = hot::make_lambda_deduced<TagDeduced>(9301, hot::caps(&seen), [](auto& c, float v) { *std::get<0>(c) = v; });
    static_assert(std::is_same_v<decltype(set), hot::LambdaClosure<void(float)>>);
    Delegate d = set;
    d.fn(2.5f);
    EXPECT_EQ(seen, 2.5f);
    std::function<void(int)> drop = mul; // a std::function with a void result drops the value
    drop(1);
}

TEST_F(LambdaClosure, DeferredLambdaTakesTheTargetSignature)
{
    auto make = [] { return hot::make_lambda_deferred<TagDeduced>(9301, hot::caps(10), [](auto& c, auto x) { return x + std::get<0>(c); }); };
    std::function<int(int)> f = make();
    EXPECT_EQ(f(1), 11);
    hot::LambdaClosure<double(double)> g = make();
    EXPECT_EQ(g(0.5), 10.5);
    std::function<void(int)> v = make(); // the void signature drops the value of the body
    v(3);
    EXPECT_EQ(hot::pin_count(9301), 3u); // one pin for each closure
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
