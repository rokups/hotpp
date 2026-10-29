// The entry counter mode of design 6.3 (S21), without a script.
//
// A ScriptEntry stands for a call into script code. A CommitWindow stands for a commit.
// A plain struct stands for the script state: the window changes it, the entries read it.
// TSan sees a data race when the protocol does not order the accesses.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "spp/runtime.h"

namespace {

using namespace std::chrono_literals;

// The mode is global. Each test sets it with a Loader. The Loader destructor resets it.
struct EntryCounters : ::testing::Test {
    spp::Loader loader;
    void SetUp() override { loader.set_sync_mode(spp::SyncMode::EntryCounters); }
};

TEST(EntryCountersDefault, StopTheWorldCountsNothing)
{
    spp::Loader loader;
    EXPECT_EQ(loader.sync_mode(), spp::SyncMode::StopTheWorld);
    spp::ScriptEntry entry;
    EXPECT_EQ(spp::active_entries(), 0u);
    EXPECT_EQ(spp::entry_depth(), 0u);
}

TEST(EntryCountersDefault, LoaderDestructorResetsTheMode)
{
    {
        spp::Loader loader;
        loader.set_sync_mode(spp::SyncMode::EntryCounters);
        EXPECT_EQ(loader.sync_mode(), spp::SyncMode::EntryCounters);
        EXPECT_TRUE(spp::entry_counters_enabled());
    }
    EXPECT_FALSE(spp::entry_counters_enabled());
}

TEST_F(EntryCounters, NestedEntryCountsOnce)
{
    {
        spp::ScriptEntry outer;
        EXPECT_EQ(spp::active_entries(), 1u);
        EXPECT_EQ(spp::entry_depth(), 1u);
        {
            spp::ScriptEntry inner;  // script -> native -> script on the same thread
            EXPECT_EQ(spp::active_entries(), 1u);
            EXPECT_EQ(spp::entry_depth(), 2u);
        }
        EXPECT_EQ(spp::entry_depth(), 1u);
    }
    EXPECT_EQ(spp::active_entries(), 0u);
    EXPECT_EQ(spp::entry_depth(), 0u);
}

TEST_F(EntryCounters, ExceptionLeavesTheEntry)
{
    try {
        spp::ScriptEntry entry;
        throw 1;
    } catch (int) {
    }
    EXPECT_EQ(spp::active_entries(), 0u);
    EXPECT_EQ(spp::entry_depth(), 0u);
}

TEST_F(EntryCounters, WindowThreadCanRunScriptCode)
{
    spp::CommitWindow window;
    EXPECT_TRUE(spp::commit_window_closed());
    EXPECT_EQ(spp::entry_depth(), 1u);
    {
        spp::ScriptEntry hook;  // for example an onReload hook; it must not block
        EXPECT_EQ(spp::entry_depth(), 2u);
    }
    EXPECT_EQ(spp::active_entries(), 0u);
}

// The window waits for an entry that is in. A new entry waits for the window.
TEST_F(EntryCounters, CommitWaitsForEntryAndBlocksNewEntries)
{
    std::atomic<bool> release_a{false}, a_in{false}, committed{false}, c_in{false};
    std::atomic<bool> c_saw_commit{false};
    int state = 0;  // the "script state"; not atomic on purpose

    std::thread a([&] {
        spp::ScriptEntry entry;
        state = 1;
        a_in.store(true);
        while (!release_a.load()) std::this_thread::yield();
        state = 2;  // still inside: the commit must not run yet
    });
    while (!a_in.load()) std::this_thread::yield();

    std::thread b([&] {
        spp::CommitWindow window;
        EXPECT_EQ(state, 2);  // "a" left before the window started
        std::this_thread::sleep_for(20ms);
        state = 3;
        committed.store(true);
    });
    while (!spp::commit_window_closed()) std::this_thread::yield();

    std::thread c([&] {
        spp::ScriptEntry entry;  // blocks until the window ends
        c_saw_commit.store(committed.load());
        EXPECT_EQ(state, 3);
        c_in.store(true);
    });

    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(committed.load());  // "b" waits for "a"
    EXPECT_FALSE(c_in.load());       // "c" waits at the gate
    release_a.store(true);
    a.join();
    b.join();
    c.join();
    EXPECT_TRUE(committed.load());
    EXPECT_TRUE(c_saw_commit.load());
    EXPECT_EQ(spp::active_entries(), 0u);
    EXPECT_FALSE(spp::commit_window_closed());
}

// Many threads enter in a loop while another thread opens many windows.
// The pair (x, y) changes only in a window, so an entry always sees x == y.
TEST_F(EntryCounters, ManyWindowsWhileThreadsEnter)
{
    constexpr int kThreads = 4;
    constexpr int kWindows = 2000;
    struct {
        long x = 0;
        long y = 0;
    } shared;
    std::atomic<bool> stop{false};
    std::atomic<long> torn{0};
    std::atomic<long> entries{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                spp::ScriptEntry outer;
                const long x = shared.x;
                {
                    spp::ScriptEntry inner;
                    if (shared.y != x) torn.fetch_add(1);
                }
                entries.fetch_add(1, std::memory_order_relaxed);
            }
        });
    const uint64_t windows0 = spp::commit_window_count();
    for (int i = 0; i < kWindows; ++i) {
        spp::CommitWindow window;
        ++shared.x;
        ++shared.y;
    }
    stop.store(true);
    for (std::thread& w : workers) w.join();
    EXPECT_EQ(torn.load(), 0);
    EXPECT_GT(entries.load(), 0);
    EXPECT_EQ(shared.x, kWindows);
    EXPECT_EQ(spp::commit_window_count() - windows0, static_cast<uint64_t>(kWindows));
    EXPECT_EQ(spp::active_entries(), 0u);
}

}  // namespace
