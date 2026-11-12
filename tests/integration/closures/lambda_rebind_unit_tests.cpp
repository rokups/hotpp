// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of lambda_rebind_module (hot/runtime/lambda.h) without a backend. Two "modules" are two
// live flags. The tags of the old module and of the new module have the same keys. A rebind
// moves the boxes of the old flag to the records of the new flag.
#include "hot/runtime.h"
#include "hot/runtime/lambda.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

uint32_t g_old = 1; // the live flag of the old module
uint32_t g_new = 1; // the live flag of the new module

int g_alive = 0; // the count of Probe objects that exist

struct Probe
{
    int value;
    explicit Probe(int v) : value(v) { ++g_alive; }
    Probe(const Probe& o) : value(o.value) { ++g_alive; }
    Probe(Probe&& o) noexcept : value(o.value) { ++g_alive; }
    ~Probe() { --g_alive; }
};

// A native struct: a capture of this type depends on the layout of the script structs.
struct Pt
{
    int x;
};

#define REBIND_TAG(Name, Key, Live, ...)                                                                               \
    struct Name                                                                                                        \
    {                                                                                                                  \
        static constexpr const char* key = Key;                                                                       \
        static constexpr const char* what = Key " lambda";                                                            \
        static constexpr const char* names[] = {__VA_ARGS__, nullptr};                                                 \
        static const uint32_t* live() { return &Live; }                                                                \
    }

// The old version: captures "a" (int) and "p" (Probe) and "gone" (Probe).
REBIND_TAG(OldMove, "T:move", g_old, "a", "p", "gone");
// The new version: the order changes, "gone" is removed.
REBIND_TAG(NewMove, "T:move", g_new, "p", "a");

REBIND_TAG(OldAdd, "T:add", g_old, "a");
REBIND_TAG(NewAdd, "T:add", g_new, "a", "b");
REBIND_TAG(OldType, "T:type", g_old, "a");
REBIND_TAG(NewType, "T:type", g_new, "a");
REBIND_TAG(OldSig, "T:sig", g_old, "a");
REBIND_TAG(NewSig, "T:sig", g_new, "a");
REBIND_TAG(OldGone, "T:gone", g_old, "a");
REBIND_TAG(OldCell, "T:cell", g_old, "n");
REBIND_TAG(NewCell, "T:cell", g_new, "n");

// The records of the new module. The lookup of the rebind finds them by key.
struct NewModule
{
    std::vector<const hot::LambdaRecord*> recs;
    static const hot::LambdaRecord* find(const char* key, void* ctx)
    {
        for (const hot::LambdaRecord* r : static_cast<NewModule*>(ctx)->recs)
            if (std::strcmp(r->key, key) == 0)
                return r;
        return nullptr;
    }
};

// The record of a lambda: the same instantiation as make_lambda.
template <class Tag, class Sig, class Caps, class F> const hot::LambdaRecord* record_of(F)
{
    using Reg = hot::detail::LambdaReg<Tag, Sig, Caps, F>;
    hot::detail::touch(&Reg::registered);
    return &Reg::record();
}

class LambdaRebind : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_alive = 0;
        hot::active_generation = 9401;
    }
    void TearDown() override
    {
        hot::lambda_detach_module(&g_old);
        hot::lambda_detach_module(&g_new);
        hot::active_generation = 0;
    }
    hot::LambdaRebind rebind(NewModule& m)
    {
        hot::CommitWindow window; // the rebind runs inside the commit window
        return hot::lambda_rebind_module(&g_old, &NewModule::find, &m);
    }
};

auto move_old = [](auto& c, int x) { return x + std::get<0>(c) + std::get<1>(c).value; };
auto move_new = [](auto& c, int x) { return 1000 + x + std::get<1>(c) + std::get<0>(c).value; };
using MoveOldCaps = std::tuple<int, Probe, Probe>;
using MoveNewCaps = std::tuple<Probe, int>;

TEST_F(LambdaRebind, MovesCapturesByNameAndRunsTheNewBody)
{
    auto c = hot::make_lambda<OldMove, int(int)>(9401, hot::caps(2, Probe(30), Probe(7)), move_old);
    auto copy = c;
    EXPECT_EQ(c(1), 33);
    EXPECT_EQ(g_alive, 2);
    NewModule m{{record_of<NewMove, int(int), MoveNewCaps>(move_new)}};
    const hot::LambdaRebind r = rebind(m);
    EXPECT_EQ(r.rebound, 1u);
    EXPECT_EQ(r.detached, 0u);
    EXPECT_EQ(g_alive, 1); // "gone" is destroyed, "p" moved to the new block
    EXPECT_EQ(c(1), 1033);
    EXPECT_EQ(copy(2), 1034); // the copy shares the box
    // The box has the new record now: a second rebind of the old flag finds nothing.
    EXPECT_EQ(rebind(m).rebound, 0u);
    bool found = false;
    hot::for_each_lambda_box([&](hot::LambdaBox*, const hot::LambdaRecord* rec) { found |= rec == m.recs[0]; });
    EXPECT_TRUE(found);
}

TEST_F(LambdaRebind, SharedCellKeepsItsValue)
{
    auto old_body = [](auto& c, int d) { return *std::get<0>(c) += d; };
    auto new_body = [](auto& c, int d) { return *std::get<0>(c) += d * 10; };
    hot::Cell<int> n = hot::make_cell<int>(0);
    auto c = hot::make_lambda<OldCell, int(int)>(9401, hot::caps(n), old_body);
    EXPECT_EQ(c(1), 1);
    NewModule m{{record_of<NewCell, int(int), std::tuple<hot::Cell<int>>>(new_body)}};
    EXPECT_EQ(rebind(m).rebound, 1u);
    EXPECT_EQ(c(1), 11);
    EXPECT_EQ(*n, 11); // the cell is the same object
}

TEST_F(LambdaRebind, DetachCasesLogOneWarningForEachKey)
{
    auto body = [](auto& c, int x) { return x + std::get<0>(c); };
    auto add_new = [](auto& c, int x) { return x + std::get<0>(c) + std::get<1>(c); };
    auto type_new = [](auto& c, int x) { return x + static_cast<int>(std::get<0>(c)); };
    auto sig_new = [](auto& c, long x) { return x + std::get<0>(c); };
    auto a1 = hot::make_lambda<OldAdd, int(int)>(9401, hot::caps(1), body);
    auto a2 = hot::make_lambda<OldAdd, int(int)>(9401, hot::caps(2), body); // a second box of the key
    auto t = hot::make_lambda<OldType, int(int)>(9401, hot::caps(1), body);
    auto s = hot::make_lambda<OldSig, int(int)>(9401, hot::caps(1), body);
    auto g = hot::make_lambda<OldGone, int(int)>(9401, hot::caps(1), body);
    NewModule m{{record_of<NewAdd, int(int), std::tuple<int, int>>(add_new),
        record_of<NewType, int(int), std::tuple<double>>(type_new), record_of<NewSig, long(long), std::tuple<int>>(sig_new)}};
    testing::internal::CaptureStderr();
    const hot::LambdaRebind r = rebind(m);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r.rebound, 0u);
    EXPECT_EQ(r.detached, 5u);
    EXPECT_NE(log.find("lambda T:add: capture b is added; 2 stored closures are detached"), std::string::npos) << log;
    EXPECT_NE(log.find("lambda T:type: capture a changed its type"), std::string::npos) << log;
    EXPECT_NE(log.find("lambda T:sig: the signature changed; 1 stored closures are detached"), std::string::npos)
        << log;
    EXPECT_NE(log.find("lambda T:gone: the lambda is removed; 1 stored closures are detached"), std::string::npos)
        << log;
    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    EXPECT_EQ(a1(1), 0);
    EXPECT_EQ(a2(1), 0);
    EXPECT_EQ(t(1), 0);
    EXPECT_EQ(s(1), 0);
    EXPECT_EQ(g(1), 0);
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 5);
}

// A capture of a class type depends on the layout of the script structs. A change of the layout
// hash detaches the box. A scalar capture moves.
REBIND_TAG(OldPt, "T:pt", g_old, "p");
struct NewPt
{
    static constexpr const char* key = "T:pt";
    static constexpr const char* names[] = {"p", nullptr};
    static const uint32_t* live() { return &g_new; }
    static const char* layout() { return "changed"; }
};
REBIND_TAG(OldInt, "T:int", g_old, "i");
struct NewInt
{
    static constexpr const char* key = "T:int";
    static constexpr const char* names[] = {"i", nullptr};
    static const uint32_t* live() { return &g_new; }
    static const char* layout() { return "changed"; }
};

TEST_F(LambdaRebind, ChangedStructLayoutDetachesAClassCapture)
{
    auto pt_body = [](auto& c) { return std::get<0>(c).x; };
    auto int_body = [](auto& c) { return std::get<0>(c); };
    auto p = hot::make_lambda<OldPt, int()>(9401, hot::caps(Pt{4}), pt_body);
    auto i = hot::make_lambda<OldInt, int()>(9401, hot::caps(5), int_body);
    NewModule m{{record_of<NewPt, int(), std::tuple<Pt>>(pt_body), record_of<NewInt, int(), std::tuple<int>>(int_body)}};
    testing::internal::CaptureStderr();
    const hot::LambdaRebind r = rebind(m);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r.rebound, 1u);
    EXPECT_EQ(r.detached, 1u);
    EXPECT_NE(log.find("lambda T:pt: capture p depends on a script struct layout that changed"), std::string::npos)
        << log;
    EXPECT_EQ(i(), 5);
}

// The last release of a closure on a worker thread waits for the end of the commit window. So it
// never destroys a capture block while the commit changes the box.
TEST_F(LambdaRebind, ReleaseOnAWorkerWaitsForTheCommitWindow)
{
    hot::Loader loader;
    loader.set_sync_mode(hot::SyncMode::EntryCounters);
    auto c = hot::make_lambda<OldMove, int(int)>(9401, hot::caps(2, Probe(30), Probe(7)), move_old);
    std::atomic<bool> done{false};
    std::thread worker;
    {
        hot::CommitWindow window;
        worker = std::thread(
            [&, c = std::move(c)]() mutable
            {
                c = hot::LambdaClosure<int(int)>(); // the last release
                done = true;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(done.load());
        EXPECT_EQ(g_alive, 2); // the captures are alive while the window is closed
    }
    worker.join();
    EXPECT_TRUE(done.load());
    EXPECT_EQ(g_alive, 0);
    loader.set_sync_mode(hot::SyncMode::StopTheWorld);
}

// N threads copy, call and release closures while the main thread rebinds them back and forth
// between two modules. A run under TSan checks the commit-window rule.
REBIND_TAG(PingA, "T:ping", g_old, "n", "k");
REBIND_TAG(PingB, "T:ping", g_new, "n", "k");

TEST_F(LambdaRebind, StressCopiesAndReleasesWhileCommitsRebind)
{
    hot::Loader loader;
    loader.set_sync_mode(hot::SyncMode::EntryCounters);
    auto body_a = [](auto& c) { return *std::get<0>(c) += std::get<1>(c); };
    auto body_b = [](auto& c) { return *std::get<0>(c) += 2 * std::get<1>(c); };
    using Caps = std::tuple<hot::Cell<int>, int>;
    NewModule to_b{{record_of<PingB, int(), Caps>(body_b)}};
    NewModule to_a{{record_of<PingA, int(), Caps>(body_a)}};

    std::mutex lock;
    std::deque<hot::LambdaClosure<int()>> queue; // closures for the workers; a worker drops the last copy
    std::atomic<bool> stop{false};
    std::atomic<int> calls{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w)
        workers.emplace_back(
            [&]
            {
                while (!stop.load())
                {
                    hot::LambdaClosure<int()> c;
                    {
                        std::lock_guard<std::mutex> g(lock);
                        if (queue.empty())
                            continue;
                        c = std::move(queue.front());
                        queue.pop_front();
                    }
                    for (int i = 0; i < 8; ++i)
                    {
                        hot::LambdaClosure<int()> copy = c;
                        std::function<int()> f = copy;
                        f();
                        calls.fetch_add(1);
                    }
                }
            });
    bool on_a = true;
    for (int round = 0; round < 200; ++round)
    {
        {
            std::lock_guard<std::mutex> g(lock);
            for (int i = 0; i < 8; ++i)
            {
                hot::Cell<int> n = hot::make_cell<int>(0);
                // A new box gets the record of the active module.
                queue.push_back(on_a ? hot::make_lambda<PingA, int()>(9401, hot::caps(n, 1), body_a)
                                     : hot::make_lambda<PingB, int()>(9401, hot::caps(n, 1), body_b));
            }
        }
        hot::CommitWindow window;
        if (on_a)
            hot::lambda_rebind_module(&g_old, &NewModule::find, &to_b);
        else
            hot::lambda_rebind_module(&g_new, &NewModule::find, &to_a);
        on_a = !on_a;
    }
    stop = true;
    for (std::thread& t : workers)
        t.join();
    queue.clear();
    EXPECT_GT(calls.load(), 0);
    loader.set_sync_mode(hot::SyncMode::StopTheWorld);
}

} // namespace
