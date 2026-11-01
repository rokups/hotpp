// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of hot::Scheduler (include/hot/runtime/scheduler.h). They need no script.
// Some tests break one rule of the scheduler on purpose when the code is wrong: a frame that
// resumes after its death is a use after free (ASan), and a park outside the lock is a race (TSan).
#include "hot/runtime.h"
#include "support/reload_assert.h"

#include <gtest/gtest.h>

#if defined(HOT_HAS_TASK)

    #include <atomic>
    #include <chrono>
    #include <cstdio>
    #include <cstdlib>
    #include <functional>
    #include <memory>
    #include <string>
    #include <thread>
    #include <vector>

namespace
{

using hot::Clock;
using hot::Scheduler;
using hot::Task;
using hot::detail::TaskOriginData;
using namespace std::chrono_literals;

constexpr Clock::time_point kNever = Clock::time_point::max();
constexpr Clock::time_point kNow = Clock::time_point::min();

// A host awaitable that a test resumes by hand. It stores the resume call in "slot".
struct Gate
{
    std::function<void()>* slot;
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(std::coroutine_handle<P> h) const
    {
        *slot = [h] { hot::detail::resume_frame(h); };
    }
    void await_resume() const noexcept {}
};

struct Counter
{
    int* count;
    explicit Counter(int* c)
        : count(c)
    {
    }
    Counter(const Counter&) = delete;
    ~Counter() { ++*count; }
};

struct Watchdog
{
    std::atomic<bool> stop{false};
    std::thread thread;
    explicit Watchdog(int seconds = 20)
    {
        thread = std::thread([this, seconds]
        {
            for (int i = 0; i < seconds * 100 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!stop.load())
            {
                std::fprintf(stderr, "scheduler test hangs: deadlock\n");
                std::abort();
            }
        });
    }
    ~Watchdog()
    {
        stop.store(true);
        thread.join();
    }
};

Task<void> ticker(int* ticks, int n)
{
    for (int i = 0; i < n; ++i)
    {
        ++*ticks;
        co_await hot::yield();
    }
}

Task<void> letters(std::string* out, char c, int n)
{
    for (int i = 0; i < n; ++i)
    {
        *out += c;
        co_await hot::yield();
    }
}

Task<int> sleeper(hot::Duration d, int v, int* destroyed = nullptr)
{
    int dummy = 0;
    Counter c(destroyed ? destroyed : &dummy);
    co_await hot::sleep(d);
    co_return v * 2;
}

Task<int> no_suspension(int v)
{
    co_return v;
}

Task<int> thrower()
{
    throw hot::ScriptError("boom");
    co_return 1;
}

Task<int> thrower_after_yield()
{
    co_await hot::yield();
    throw hot::ScriptError("late boom");
    co_return 1;
}

Task<int> cancelled_body()
{
    co_await hot::yield();
    throw hot::TaskCancelled("stop");
    co_return 1;
}

Task<int> reload_cancelled_body()
{
    throw hot::TaskReloadCancelled("task cancelled by reload: x");
    co_return 1;
}

Task<int> gate_task(std::function<void()>* slot, int v)
{
    co_await Gate{slot};
    co_return v;
}

Task<int> await_ptr(Task<int>* t)
{
    co_return co_await* t;
}

// Waits at the gate, then counts on the thread that resumes it, then yields once.
Task<void> gate_then_yield(std::function<void()>* slot, std::atomic<int>* steps)
{
    co_await Gate{slot};
    steps->fetch_add(1);
    co_await hot::yield();
    steps->fetch_add(1);
}

// Waits at the gate, then counts on the thread that resumes it, then ends.
Task<void> gate_then_end(std::function<void()>* slot, std::atomic<int>* steps)
{
    co_await Gate{slot};
    steps->fetch_add(1);
}

Task<void> forever_yield(int* destroyed)
{
    Counter c(destroyed);
    for (;;)
        co_await hot::yield();
}

struct SchedulerTest : ::testing::Test
{
    std::vector<std::string> reports;
    void SetUp() override
    {
        hot::set_script_error_handler([this](const hot::ScriptErrorReport& r) { reports.push_back(r.message); });
    }
    void TearDown() override
    {
        hot::scheduler().cancel_all();
        // No entry of a dead frame stays in the process scheduler.
        EXPECT_EQ(hot::scheduler().next_wake(), kNever);
        EXPECT_EQ(hot::scheduler().size(), 0u);
        hot::set_script_error_handler(nullptr);
    }
};

// ---------------------------------------------------------------------------- spawn and tick

TEST_F(SchedulerTest, SpawnDoesNotRunTheBody)
{
    Scheduler s;
    int ticks = 0;
    const hot::TaskId id = s.spawn(ticker(&ticks, 3));
    EXPECT_NE(id, 0u);
    EXPECT_EQ(ticks, 0);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_TRUE(s.alive(id));
}

TEST_F(SchedulerTest, TickRunsAndReaps)
{
    Scheduler s;
    int ticks = 0;
    const hot::TaskId id = s.spawn(ticker(&ticks, 3));
    for (int i = 1; i <= 3; ++i)
    {
        EXPECT_EQ(s.tick(), 1u);
        EXPECT_EQ(ticks, i);
        EXPECT_EQ(s.size(), 1u);
    }
    EXPECT_EQ(s.tick(), 1u); // the last resume ends the body
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.alive(id));
    EXPECT_EQ(s.tick(), 0u);
    EXPECT_EQ(s.next_wake(), kNever);
}

TEST_F(SchedulerTest, SpawnEmptiesTheTask)
{
    Scheduler s;
    int ticks = 0;
    Task<void> t = ticker(&ticks, 1);
    s.spawn(t);
    EXPECT_FALSE(t.valid());
}

TEST_F(SchedulerTest, SpawnOfAnEmptyOrAwaitedTaskThrows)
{
    Scheduler s;
    Task<int> empty;
    try
    {
        s.spawn(empty);
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "empty task");
    }
    std::function<void()> slot;
    Task<int> child = gate_task(&slot, 1);
    Task<int> parent = await_ptr(&child);
    parent.resume();
    try
    {
        s.spawn(child);
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task is already awaited");
    }
    EXPECT_TRUE(child.valid()); // the throw did not take the frame
    EXPECT_EQ(s.size(), 0u);
}

TEST_F(SchedulerTest, IdsAreUnique)
{
    Scheduler s;
    int ticks = 0;
    const hot::TaskId a = s.spawn(ticker(&ticks, 1));
    const hot::TaskId b = s.spawn(ticker(&ticks, 1));
    EXPECT_NE(a, b);
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
}

TEST_F(SchedulerTest, BodyErrorReachesTheHandlerOnce)
{
    Scheduler s;
    s.spawn(thrower());
    s.spawn(thrower_after_yield());
    s.tick();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0], "boom");
    s.tick();
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[1], "late boom");
    s.tick();
    EXPECT_EQ(reports.size(), 2u);
    EXPECT_EQ(s.size(), 0u);
}

TEST_F(SchedulerTest, CancellationIsSilent)
{
    Scheduler s;
    s.spawn(cancelled_body());
    s.spawn(reload_cancelled_body());
    s.tick();
    s.tick();
    EXPECT_EQ(s.size(), 0u);
    EXPECT_TRUE(reports.empty());
}

TEST_F(SchedulerTest, YieldIsRoundRobin)
{
    Scheduler s;
    std::string out;
    s.spawn(letters(&out, 'a', 2));
    s.spawn(letters(&out, 'b', 3));
    EXPECT_EQ(s.tick(), 2u); // a frame that yields in a tick waits for the next tick
    EXPECT_EQ(out, "ab");
    EXPECT_EQ(s.tick(), 2u);
    EXPECT_EQ(out, "abab");
    EXPECT_EQ(s.tick(), 2u); // "a" ends, "b" adds its last letter
    EXPECT_EQ(out, "ababb");
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s.tick(), 1u);
    EXPECT_EQ(s.size(), 0u);
}

TEST_F(SchedulerTest, SleepUsesTheInjectedTime)
{
    Scheduler s;
    int destroyed = 0;
    const hot::TaskId id = s.spawn(sleeper(10s, 1, &destroyed));
    const Clock::time_point before = Clock::now();
    EXPECT_EQ(s.next_wake(), kNow); // the start entry is ready
    s.tick(before);
    const Clock::time_point wake = s.next_wake();
    EXPECT_GE(wake, before + 10s);
    EXPECT_LE(wake, Clock::now() + 10s);
    EXPECT_EQ(s.tick(Clock::now()), 0u);
    EXPECT_TRUE(s.alive(id));
    EXPECT_EQ(s.tick(wake), 1u);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(s.next_wake(), kNever);
}

TEST_F(SchedulerTest, ShorterSleepWakesFirst)
{
    Scheduler s;
    std::string out;
    auto mark = [](std::string* o, char c, hot::Duration d) -> Task<void>
    {
        co_await hot::sleep(d);
        *o += c;
    };
    s.spawn(mark(&out, 'l', 2s));
    s.spawn(mark(&out, 's', 1s));
    s.tick();
    s.tick(Clock::now() + 3s);
    EXPECT_EQ(out, "sl");
}

TEST_F(SchedulerTest, NextWake)
{
    Scheduler s;
    EXPECT_EQ(s.next_wake(), kNever);
    int ticks = 0;
    s.spawn(ticker(&ticks, 1));
    EXPECT_EQ(s.next_wake(), kNow);
    s.tick();
    EXPECT_EQ(s.next_wake(), kNow); // the frame yielded
    s.tick();
    EXPECT_EQ(s.next_wake(), kNever);
}

TEST_F(SchedulerTest, RunDrivesUntilIdle)
{
    Watchdog dog;
    Scheduler s;
    int ticks = 0;
    s.spawn(ticker(&ticks, 3));
    s.spawn(sleeper(5ms, 1));
    EXPECT_TRUE(s.run(10s));
    EXPECT_EQ(ticks, 3);
    EXPECT_EQ(s.size(), 0u);
    s.spawn(sleeper(10s, 1));
    EXPECT_FALSE(s.run(20ms));
    EXPECT_EQ(s.size(), 1u);
}

// ---------------------------------------------------------------------------- step

TEST_F(SchedulerTest, StepRunsToTheFirstSuspensionOnce)
{
    Task<int> t = sleeper(10s, 3);
    EXPECT_FALSE(hot::step(t));
    EXPECT_EQ(hot::scheduler().next_wake() == kNever, false); // parked in the process scheduler
    EXPECT_FALSE(hot::step(t)); // a started task does not resume again
    EXPECT_EQ(hot::scheduler().tick(Clock::now() + 20s), 1u);
    EXPECT_TRUE(t.done());
    EXPECT_EQ(t.result(), 6);
    EXPECT_TRUE(hot::step(t)); // done: no resume
}

TEST_F(SchedulerTest, StepOfATaskWithoutSuspensionIsDone)
{
    Task<int> t = no_suspension(4);
    EXPECT_TRUE(hot::step(t));
    EXPECT_EQ(t.result(), 4);
    Task<int> empty;
    EXPECT_THROW(hot::step(empty), hot::ScriptError);
}

// ---------------------------------------------------------------------------- run_until_done

TEST_F(SchedulerTest, RunUntilDoneReturnsTrue)
{
    Watchdog dog;
    Task<int> t = sleeper(5ms, 21);
    EXPECT_TRUE(hot::run_until_done(t, 10s));
    EXPECT_EQ(t.result(), 42);
    Task<int> u = no_suspension(1);
    EXPECT_TRUE(hot::run_until_done(u, 0s));
}

TEST_F(SchedulerTest, RunUntilDoneTimeoutKeepsTheTaskThenSpawnFinishesIt)
{
    Watchdog dog;
    int destroyed = 0;
    Task<int> t = sleeper(10s, 1, &destroyed);
    EXPECT_FALSE(hot::run_until_done(t, 20ms));
    EXPECT_TRUE(t.valid());
    EXPECT_FALSE(t.done());
    EXPECT_EQ(destroyed, 0);
    hot::spawn(t); // the run cleared its link: the spawn works
    EXPECT_FALSE(t.valid());
    EXPECT_EQ(hot::scheduler().size(), 1u);
    hot::scheduler().tick(Clock::now() + 20s);
    EXPECT_EQ(hot::scheduler().size(), 0u);
    EXPECT_EQ(destroyed, 1);
    EXPECT_TRUE(reports.empty());
}

TEST_F(SchedulerTest, RunUntilDoneWithoutTimeStartsAndChecksOnce)
{
    Task<int> t = sleeper(10s, 1);
    EXPECT_FALSE(hot::run_until_done(t, 0s));
    EXPECT_FALSE(t.done());
    EXPECT_NE(hot::scheduler().next_wake(), kNever);
}

TEST_F(SchedulerTest, RunUntilDoneOfAnAwaitedTaskThrows)
{
    std::function<void()> slot;
    Task<int> child = gate_task(&slot, 1);
    Task<int> parent = await_ptr(&child);
    parent.resume();
    try
    {
        hot::run_until_done(child, 10ms);
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task is already awaited");
    }
    Task<int> empty;
    EXPECT_THROW(hot::run_until_done(empty, 10ms), hot::ScriptError);
}

Task<int> nested_runner(int* inner_ticks, bool* ok)
{
    Task<void> inner = ticker(inner_ticks, 2);
    *ok = hot::run_until_done(inner, 10s); // a tick inside a tick
    co_await hot::yield();
    co_return 1;
}

TEST_F(SchedulerTest, RunUntilDoneInsideASpawnedTask)
{
    Watchdog dog;
    int inner = 0;
    bool ok = false;
    int ticks = 0;
    hot::spawn(nested_runner(&inner, &ok));
    hot::spawn(ticker(&ticks, 5));
    hot::scheduler().tick();
    EXPECT_TRUE(ok);
    EXPECT_EQ(inner, 2);
    EXPECT_GE(ticks, 1);
    EXPECT_TRUE(hot::scheduler().run(10s));
    EXPECT_EQ(ticks, 5);
}

// ---------------------------------------------------------------------------- cancel and lifetime

TEST_F(SchedulerTest, CancelDestroysTheFrame)
{
    Scheduler s;
    int destroyed = 0;
    const hot::TaskId id = s.spawn(sleeper(10s, 1, &destroyed));
    s.tick();
    EXPECT_TRUE(s.alive(id));
    EXPECT_TRUE(s.cancel(id));
    EXPECT_EQ(destroyed, 1);
    EXPECT_FALSE(s.alive(id));
    EXPECT_FALSE(s.cancel(id));
    EXPECT_FALSE(s.cancel(0));
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.next_wake(), kNever); // the entry went with the frame
    EXPECT_EQ(s.tick(Clock::now() + 20s), 0u);
}

TEST_F(SchedulerTest, CancelOfANeverStartedTask)
{
    Scheduler s;
    int ticks = 0;
    const hot::TaskId id = s.spawn(ticker(&ticks, 1));
    EXPECT_TRUE(s.cancel(id));
    EXPECT_EQ(s.tick(), 0u);
    EXPECT_EQ(ticks, 0);
}

// Rule: the destroy of a parked frame removes its entry. Else the tick resumes freed memory.
TEST_F(SchedulerTest, TaskDestroyedWhileParkedIsNotResumed)
{
    int destroyed = 0;
    {
        Task<void> t = forever_yield(&destroyed);
        t.resume(); // parks in the process scheduler
        EXPECT_EQ(hot::scheduler().next_wake(), kNow);
    }
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(hot::scheduler().next_wake(), kNever);
    EXPECT_EQ(hot::scheduler().tick(), 0u);
    {
        Task<int> t = sleeper(10s, 1, &destroyed);
        EXPECT_FALSE(hot::step(t)); // parks as a timer
    }
    EXPECT_EQ(destroyed, 2);
    EXPECT_EQ(hot::scheduler().tick(Clock::now() + 20s), 0u);
}

// Rule: a scheduler that dies clears the entries of the frames that it does not own. Else the
// later destroy of the frame removes its entry from a dead scheduler.
TEST_F(SchedulerTest, FrameParkedInADeadSchedulerCanBeDestroyed)
{
    Watchdog dog;
    int destroyed = 0;
    Task<int> t = sleeper(10s, 1, &destroyed);
    {
        Scheduler s;
        EXPECT_FALSE(s.run_until_done(t, 1ms)); // the frame parks in "s"
        EXPECT_EQ(hot::scheduler().next_wake(), kNever);
    }
    EXPECT_FALSE(t.done());
    t = Task<int>();
    EXPECT_EQ(destroyed, 1);
}

Task<void> self_cancel(Scheduler* s, hot::TaskId* id, int* after, int* destroyed)
{
    Counter c(destroyed);
    co_await hot::yield();
    EXPECT_TRUE(s->cancel(*id)); // deferred: the frame ends at its next suspension
    ++*after; // the code after the cancel runs on
    co_await hot::yield();
    ++*after; // never runs
}

TEST_F(SchedulerTest, TaskThatCancelsItselfEndsAtTheNextSuspension)
{
    Scheduler s;
    hot::TaskId id = 0;
    int after = 0;
    int destroyed = 0;
    id = s.spawn(self_cancel(&s, &id, &after, &destroyed));
    s.tick();
    s.tick();
    EXPECT_EQ(after, 1);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.next_wake(), kNever);
    EXPECT_EQ(s.tick(), 0u);
}

Task<void> spawner(Scheduler* s, int* ticks)
{
    co_await hot::yield();
    s->spawn(ticker(ticks, 1)); // a spawn in a tick runs at the next tick
}

TEST_F(SchedulerTest, SpawnInsideATick)
{
    Scheduler s;
    int ticks = 0;
    s.spawn(spawner(&s, &ticks));
    s.tick();
    s.tick();
    EXPECT_EQ(ticks, 0);
    EXPECT_EQ(s.size(), 1u);
    s.tick();
    EXPECT_EQ(ticks, 1);
}

TEST_F(SchedulerTest, CancelAll)
{
    Scheduler s;
    int destroyed = 0;
    s.spawn(sleeper(10s, 1, &destroyed));
    s.spawn(sleeper(10s, 1, &destroyed));
    s.tick();
    s.cancel_all();
    EXPECT_EQ(destroyed, 2);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.next_wake(), kNever);
}

TEST_F(SchedulerTest, SpawnOfAStartedTaskMovesItsEntry)
{
    Scheduler s;
    int ticks = 0;
    Task<void> t = ticker(&ticks, 2);
    EXPECT_FALSE(hot::step(t)); // parks in the process scheduler
    EXPECT_EQ(ticks, 1);
    s.spawn(t);
    EXPECT_EQ(hot::scheduler().next_wake(), kNever);
    EXPECT_EQ(s.next_wake(), kNow);
    s.tick();
    EXPECT_EQ(ticks, 2);
    s.tick();
    EXPECT_EQ(s.size(), 0u);
}

TEST_F(SchedulerTest, ProcessSchedulerIsOneObject)
{
    Scheduler* a = &hot::scheduler();
    Scheduler* b = nullptr;
    std::thread thread([&b] { b = &hot::scheduler(); });
    thread.join();
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, &hot::scheduler());
}

// ---------------------------------------------------------------------------- threads

TEST_F(SchedulerTest, FrameResumedOnAnotherThreadYieldsAndTheMainTickResumesIt)
{
    Watchdog dog;
    std::function<void()> slot;
    std::atomic<int> steps{0};
    Task<void> t = gate_then_yield(&slot, &steps);
    t.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    std::thread thread([&slot] { slot(); }); // the frame yields on this thread
    thread.join();
    EXPECT_EQ(steps.load(), 1);
    EXPECT_EQ(hot::scheduler().tick(), 1u);
    EXPECT_EQ(steps.load(), 2);
    EXPECT_TRUE(t.done());
}

TEST_F(SchedulerTest, RunUntilDoneWakesWhenTheTaskEndsOnAnotherThread)
{
    Watchdog dog;
    std::function<void()> slot;
    Task<int> t = gate_task(&slot, 9);
    t.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    std::thread thread([&slot]
    {
        std::this_thread::sleep_for(20ms);
        slot();
    });
    const Clock::time_point start = Clock::now();
    EXPECT_TRUE(hot::run_until_done(t, 30s));
    EXPECT_LT(Clock::now() - start, 10s);
    thread.join();
    EXPECT_EQ(t.result(), 9);
}

TEST_F(SchedulerTest, RunWakesWhenASpawnedTaskEndsOnAnotherThread)
{
    Watchdog dog;
    Scheduler s;
    std::function<void()> slot;
    Task<int> t = gate_task(&slot, 9);
    t.resume();
    s.spawn(t);
    std::thread thread([&slot]
    {
        std::this_thread::sleep_for(20ms);
        slot();
    });
    const Clock::time_point start = Clock::now();
    EXPECT_TRUE(s.run(30s));
    EXPECT_LT(Clock::now() - start, 10s);
    thread.join();
}

// Rule: a frame parks and stops running in one step under the lock. The owner destroys the task
// on the main thread while the frame parks on another thread: the destroy waits or removes the
// entry. A park outside the lock adds an entry of a dead frame.
TEST_F(SchedulerTest, DestroyWhileTheFrameParksOnAnotherThread)
{
    Watchdog dog;
    for (int i = 0; i < 200; ++i)
    {
        std::function<void()> slot;
        std::atomic<int> steps{0};
        auto t = std::make_unique<Task<void>>(gate_then_yield(&slot, &steps));
        t->resume();
        std::thread thread([&] { slot(); });
        while (steps.load() == 0) // the frame runs on the thread now: the gate does not hold it
        {
        }
        t.reset();
        thread.join();
        EXPECT_EQ(hot::scheduler().tick(), 0u);
        ASSERT_EQ(hot::scheduler().next_wake(), kNever);
    }
}

// Rule: a tick pops an entry and marks the frame running in one step under the lock. The owner
// destroys the task on another thread while the main thread ticks.
TEST_F(SchedulerTest, DestroyOnAnotherThreadWhileTheMainThreadTicks)
{
    Watchdog dog;
    for (int i = 0; i < 100; ++i)
    {
        int destroyed = 0;
        auto t = std::make_unique<Task<void>>(forever_yield(&destroyed));
        t->resume();
        std::atomic<bool> gone{false};
        std::thread thread([&]
        {
            std::this_thread::yield();
            t.reset();
            gone.store(true);
        });
        while (!gone.load())
            hot::scheduler().tick();
        thread.join();
        EXPECT_EQ(destroyed, 1);
        EXPECT_EQ(hot::scheduler().tick(), 0u);
        ASSERT_EQ(hot::scheduler().next_wake(), kNever);
    }
}

// Rule: the task calls on_done (the notify of the scheduler) under task_mutex(). The main thread
// sees the end, returns and destroys a local scheduler. A call of on_done after the lock uses a
// dead scheduler.
TEST_F(SchedulerTest, LocalSchedulerDiesRightAfterRunUntilDoneSeesTheEndOnAWorker)
{
    Watchdog dog(60);
    for (int i = 0; i < 300; ++i)
    {
        auto s = std::make_unique<Scheduler>();
        std::function<void()> slot;
        Task<int> t = gate_task(&slot, i);
        t.resume();
        ASSERT_TRUE(static_cast<bool>(slot));
        std::thread thread([&slot] { slot(); });
        EXPECT_TRUE(s->run_until_done(t, 30s));
        s.reset();
        thread.join();
        EXPECT_EQ(t.result(), i);
    }
}

TEST_F(SchedulerTest, LocalSchedulerDiesRightAfterRunOrATimeoutWhileATaskEndsOnAWorker)
{
    Watchdog dog(60);
    for (int i = 0; i < 300; ++i)
    {
        auto s = std::make_unique<Scheduler>();
        std::function<void()> slot;
        std::atomic<int> steps{0};
        Task<void> t = gate_then_end(&slot, &steps);
        t.resume();
        ASSERT_TRUE(static_cast<bool>(slot));
        s->spawn(t);
        std::thread thread([&slot] { slot(); });
        if (i % 2 == 0)
            EXPECT_TRUE(s->run(30s)); // the reap destroys the frame, then the scheduler dies
        else
        {
            while (steps.load() == 0) // the gate does not hold the frame from here
            {
            }
            s->run(Clock::duration::zero()); // a timeout or not: the scheduler dies at once
        }
        s.reset();
        thread.join();
    }
}

// Rule: run_until_done sets the scheduler of the frame only during the call. The frame waits at
// a gate when the call times out, and the scheduler dies. Then the frame yields: it parks in the
// process scheduler, not in the dead one.
TEST_F(SchedulerTest, FrameThatParksAfterItsRunUntilDoneSchedulerDiedUsesTheProcessScheduler)
{
    Watchdog dog;
    std::function<void()> slot;
    std::atomic<int> steps{0};
    Task<void> t = gate_then_yield(&slot, &steps);
    {
        Scheduler s;
        EXPECT_FALSE(s.run_until_done(t, 1ms));
    }
    ASSERT_TRUE(static_cast<bool>(slot));
    slot();
    EXPECT_EQ(steps.load(), 1);
    EXPECT_EQ(hot::scheduler().next_wake(), kNow);
    EXPECT_EQ(hot::scheduler().tick(), 1u);
    EXPECT_TRUE(t.done());
}

// A frame that was parked in a scheduler that died has no entry: nothing can resume it. A spawn
// or a run_until_done of it throws and leaves the task to its owner.
TEST_F(SchedulerTest, SpawnOfAFrameParkedInADeadSchedulerThrows)
{
    Watchdog dog;
    int destroyed = 0;
    Task<int> t = sleeper(10s, 1, &destroyed);
    {
        Scheduler s;
        EXPECT_FALSE(s.run_until_done(t, 1ms)); // the frame parks in "s"
    }
    try
    {
        hot::spawn(t);
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task was parked in a destroyed scheduler");
    }
    EXPECT_TRUE(t.valid());
    EXPECT_THROW(hot::run_until_done(t, 1ms), hot::ScriptError);
    EXPECT_EQ(hot::scheduler().size(), 0u);
    EXPECT_EQ(hot::scheduler().next_wake(), kNever);
    t = Task<int>();
    EXPECT_EQ(destroyed, 1);
}

// ---------------------------------------------------------------------------- reload

// A class record is needed: the generation number of a module comes from its records.
struct SFake : hot::ShellBase
{
    virtual ~SFake() { hot::destroy_state(this); }
};
struct SState
{
    int value;
};
hot::FieldInfo s_fields[] = {
    {"value", nullptr, hot::type_key<int>(), offsetof(SState, value), sizeof(int), alignof(int),
        hot::field_flags<int>(), [](void*, void* f) { new (f) int(1); }, hot::move_field<int>,
        hot::field_destroyer<int>()},
};
hot::StateLayout s_layout{"SState", sizeof(SState), alignof(SState), 1, s_fields};
hot::SlotFn s_slots[1] = {nullptr};
constexpr uint64_t kSHash = hot::shell_layout_hash("Base", sizeof(SFake), alignof(SFake), 0);
hot::ClassRecord s_rec1{"SFake", 1, s_slots, 1, &s_layout, nullptr, "Base", kSHash};
hot::ClassRecord s_rec2{"SFake", 2, s_slots, 1, &s_layout, nullptr, "Base", kSHash};
hot::Instance create_s1()
{
    auto* s = new SFake;
    hot::init_state(s, &s_rec1);
    return {s, s};
}
hot::Instance create_s2()
{
    auto* s = new SFake;
    hot::init_state(s, &s_rec2);
    return {s, s};
}

uint32_t g_flag1 = 0;
uint32_t g_flag2 = 0;
void register_flag1(hot::Registry& reg)
{
    reg.set_live_flag(&g_flag1);
    reg.add(&s_rec1, &create_s1);
}
void register_flag2(hot::Registry& reg)
{
    reg.set_live_flag(&g_flag2);
    reg.add(&s_rec2, &create_s2);
}
hot::Module module_of(hot::Module::RegisterFn fn, const char* name)
{
    hot::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

// A task of generation 1 code, written like the translator writes it.
Task<int> gen1_co(TaskOriginData, hot::Duration d, bool* ran)
{
    if (d == hot::Duration::zero())
        co_await hot::yield();
    else
        co_await hot::sleep(d);
    *ran = true;
    co_return 1;
}
Task<int> gen1(hot::Duration d, bool* ran)
{
    return gen1_co(TaskOriginData{1, &g_flag1, "Cls::m"}, d, ran);
}

struct SchedulerReload : SchedulerTest
{
    hot::Loader loader;
    void SetUp() override
    {
        SchedulerTest::SetUp();
        g_flag1 = 0;
        g_flag2 = 0;
        s_rec1 = hot::ClassRecord{"SFake", 1, s_slots, 1, &s_layout, nullptr, "Base", kSHash};
        s_rec2 = hot::ClassRecord{"SFake", 2, s_slots, 1, &s_layout, nullptr, "Base", kSHash};
    }
    void reload_to_2()
    {
        ASSERT_RELOAD_OK(loader.request_reload(module_of(register_flag2, "gen2")));
        ASSERT_RELOAD_OK(loader.commit_pending());
        ASSERT_EQ(hot::active_generation, 2u);
        ASSERT_EQ(g_flag1, 0u);
    }
};

TEST_F(SchedulerReload, CancelAllThenLoaderResetWorks)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    bool ran = false;
    hot::spawn(gen1(10s, &ran));
    hot::scheduler().tick();
    EXPECT_EQ(hot::pin_count(1), 1u);
    std::string error;
    EXPECT_FALSE(loader.reset(error)); // the spawned task pins generation 1
    hot::scheduler().cancel_all();
    EXPECT_EQ(hot::pin_count(1), 0u);
    EXPECT_TRUE(loader.reset(error)) << error;
    EXPECT_FALSE(ran);
}

TEST_F(SchedulerReload, StaleSleepingSpawnedTaskIsCancelledAtTheNextTick)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    bool ran = false;
    hot::spawn(gen1(10s, &ran));
    hot::scheduler().tick();
    reload_to_2();
    const uint64_t before = hot::stale_call_count();
    EXPECT_EQ(hot::scheduler().next_wake(), kNow); // a stale timer is due at once
    EXPECT_EQ(hot::scheduler().tick(), 1u); // not due by the time
    EXPECT_FALSE(ran);
    EXPECT_EQ(hot::stale_call_count(), before + 1);
    EXPECT_EQ(hot::scheduler().size(), 0u);
    EXPECT_EQ(hot::pin_count(1), 0u);
    EXPECT_TRUE(reports.empty());
}

TEST_F(SchedulerReload, StaleReadyAndNotStartedTasksAreCancelled)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    bool ran_yield = false;
    bool ran_new = false;
    hot::spawn(gen1(hot::Duration::zero(), &ran_yield));
    hot::scheduler().tick(); // parks at the yield
    hot::spawn(gen1(hot::Duration::zero(), &ran_new)); // never started
    reload_to_2();
    const uint64_t before = hot::stale_call_count();
    EXPECT_EQ(hot::scheduler().tick(), 2u);
    EXPECT_FALSE(ran_yield);
    EXPECT_FALSE(ran_new);
    EXPECT_EQ(hot::stale_call_count(), before + 2);
    EXPECT_EQ(hot::scheduler().size(), 0u);
    EXPECT_EQ(hot::pin_count(1), 0u);
    EXPECT_TRUE(reports.empty());
}

} // namespace

#endif // HOT_HAS_TASK
