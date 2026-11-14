// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::run_compiles: the parallel compile phase.
#include "hot/backend.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace
{

struct Shared
{
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};
    std::mutex mutex;
    std::vector<std::thread::id> ids;
};

class FakePending : public hot::PendingLoad
{
public:
    FakePending(Shared& s, bool throws = false)
        : shared_(s)
        , throws_(throws)
    {
    }
    void compile() override
    {
        const int now = ++shared_.running;
        int old = shared_.peak.load();
        while (old < now && !shared_.peak.compare_exchange_weak(old, now))
        {
        }
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.ids.push_back(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        --shared_.running;
        ++shared_.done;
        if (throws_)
            throw std::runtime_error("boom");
    }
    hot::LoadResult finish() override { return {}; }

private:
    Shared& shared_;
    bool throws_;
};

} // namespace

TEST(RunCompiles, PeakIsMinOfJobsAndCount)
{
    for (const auto [jobs, n, expected] : {std::tuple<unsigned, int, int>{3, 8, 3}, {8, 4, 4}, {2, 2, 2}})
    {
        Shared s;
        std::vector<std::unique_ptr<FakePending>> owned;
        std::vector<hot::PendingLoad*> list;
        for (int i = 0; i < n; ++i)
        {
            owned.push_back(std::make_unique<FakePending>(s));
            list.push_back(owned.back().get());
        }
        const auto errors = hot::run_compiles(list, jobs);
        EXPECT_EQ(errors.size(), static_cast<size_t>(n));
        for (const auto& e : errors)
            EXPECT_TRUE(e.empty());
        EXPECT_EQ(s.done.load(), n);
        EXPECT_EQ(s.peak.load(), expected);
    }
}

TEST(RunCompiles, OneJobStartsNoThread)
{
    Shared s;
    FakePending a(s), b(s), c(s);
    std::vector<hot::PendingLoad*> list{&a, &b, &c};
    const auto errors = hot::run_compiles(list, 1);
    EXPECT_EQ(errors.size(), 3u);
    EXPECT_EQ(s.peak.load(), 1);
    for (const auto& id : s.ids)
        EXPECT_EQ(id, std::this_thread::get_id());
}

TEST(RunCompiles, SingleEntryRunsInline)
{
    Shared s;
    FakePending a(s);
    std::vector<hot::PendingLoad*> list{&a};
    hot::run_compiles(list, 8);
    ASSERT_EQ(s.ids.size(), 1u);
    EXPECT_EQ(s.ids[0], std::this_thread::get_id());
}

TEST(RunCompiles, ExceptionTextIsReturnedAndOthersRun)
{
    Shared s;
    FakePending a(s), b(s, true), c(s), d(s);
    std::vector<hot::PendingLoad*> list{&a, &b, &c, &d};
    const auto errors = hot::run_compiles(list, 2);
    ASSERT_EQ(errors.size(), 4u);
    EXPECT_TRUE(errors[0].empty());
    EXPECT_EQ(errors[1], "boom");
    EXPECT_TRUE(errors[2].empty());
    EXPECT_TRUE(errors[3].empty());
    EXPECT_EQ(s.done.load(), 4);
}

TEST(RunCompiles, EmptyList)
{
    std::vector<hot::PendingLoad*> list;
    EXPECT_TRUE(hot::run_compiles(list, 4).empty());
}

namespace
{
class OrderPending : public hot::PendingLoad
{
public:
    OrderPending(std::vector<int>& log, int id, size_t unit_cost)
        : log_(log)
        , id_(id)
    {
        cost = unit_cost;
    }
    void compile() override { log_.push_back(id_); }
    hot::LoadResult finish() override { return {}; }

private:
    std::vector<int>& log_;
    int id_;
};
} // namespace

TEST(RunCompiles, LargestCostStartsFirstAndTiesKeepOrder)
{
    std::vector<int> log;
    OrderPending a(log, 0, 10), b(log, 1, 500), c(log, 2, 10), d(log, 3, 500), e(log, 4, 70);
    std::vector<hot::PendingLoad*> list{&a, &b, &c, &d, &e};
    const auto errors = hot::run_compiles(list, 1);
    EXPECT_EQ(errors.size(), 5u);
    EXPECT_EQ(log, (std::vector<int>{1, 3, 4, 0, 2}));
}

TEST(BuildJobsFor, UnitCountRule)
{
    EXPECT_EQ(hot::build_jobs_for(1, 8, 16), 8u);   // one unit: the default
    EXPECT_EQ(hot::build_jobs_for(3, 8, 16), 3u);   // units <= threads
    EXPECT_EQ(hot::build_jobs_for(8, 8, 16), 8u);
    EXPECT_EQ(hot::build_jobs_for(9, 8, 16), 8u);   // more units than threads: the default
    EXPECT_EQ(hot::build_jobs_for(20, 32, 16), 16u); // above the memory cap: the default
    EXPECT_EQ(hot::build_jobs_for(4, 0, 16), 1u);   // unknown thread count
}
