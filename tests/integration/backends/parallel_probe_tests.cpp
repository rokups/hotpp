// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the parallel probe helpers (src/runtime/parallel_probe.h).
#include "parallel_probe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <vector>

namespace
{
// Sets the thread start budget for one test and restores it at the end.
struct SpawnBudget
{
    explicit SpawnBudget(int n) { hot::detail::probe_spawn_budget().store(n); }
    ~SpawnBudget() { hot::detail::probe_spawn_budget().store(-1); }
};

std::vector<int> make_items(size_t n)
{
    std::vector<int> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<int>(i);
    return v;
}
} // namespace

TEST(ParallelProbe, FindsAMatchAndReportsNoMatch)
{
    const std::vector<int> items = make_items(20000);
    EXPECT_EQ(hot::detail::find_any_parallel(items, [](int v) { return v == 12345; }), 12345u);
    EXPECT_EQ(hot::detail::find_any_parallel(items, [](int) { return false; }), items.size());
}

TEST(ParallelProbe, ThrowingPredicateReachesTheCaller)
{
    const std::vector<int> items = make_items(20000);
    EXPECT_THROW(hot::detail::find_any_parallel(items, [](int v) -> bool {
        if (v == 7777)
            throw std::runtime_error("probe");
        return false;
    }), std::runtime_error);
    EXPECT_THROW(hot::detail::parallel_for(items.size(), [](size_t i) {
        if (i == 9999)
            throw std::runtime_error("probe");
    }), std::runtime_error);
}

TEST(ParallelProbe, ThreadThatCannotStartDoesNotStopTheWork)
{
    const std::vector<int> items = make_items(20000);
    for (const int budget : {0, 1, 2})
    {
        SpawnBudget guard(budget);
        EXPECT_EQ(hot::detail::find_any_parallel(items, [](int v) { return v == 19999; }), 19999u);
        std::atomic<size_t> sum{0};
        hot::detail::parallel_for(items.size(), [&](size_t i) { sum += i; });
        EXPECT_EQ(sum.load(), 20000u * 19999u / 2);
    }
}
