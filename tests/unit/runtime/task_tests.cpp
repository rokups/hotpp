// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of hot::Task (include/hot/runtime/task.h). They need no script.
// The coroutines here are written like the translator writes them: an outer function passes
// a TaskOriginData as the first parameter of the coroutine. The reload tests use the real Loader with modules
// that have no classes and only a live flag.
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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using hot::Task;
using hot::detail::TaskOriginData;

uint32_t g_live = 1; // a live flag that never changes

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

Task<int> value_task_co(TaskOriginData, int v, bool* started)
{
    if (started)
        *started = true;
    co_return v;
}
Task<int> value_task(int v, bool* started = nullptr)
{
    return value_task_co(TaskOriginData{0, &g_live, "t"}, v, started);
}

Task<void> void_task_co(TaskOriginData, int* out)
{
    *out = 7;
    co_return;
}
Task<void> void_task(int* out)
{
    return void_task_co(TaskOriginData{0, &g_live, "t"}, out);
}

Task<int> child_sum(int a, int b)
{
    co_return a + b;
}

Task<int> parent_task()
{
    int x = co_await child_sum(1, 2);
    Task<int> second = child_sum(x, 10);
    int y = co_await second;
    co_return y * 2;
}

Task<int> thrower()
{
    throw hot::ScriptError("boom");
    co_return 1;
}

// A host awaitable. Another thread resumes the frame through resume_frame.
struct ThreadAwaitable
{
    std::thread* thread;
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(std::coroutine_handle<P> h) const
    {
        *thread = std::thread(
            [h]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                hot::detail::resume_frame(h);
            });
    }
    int await_resume() const noexcept { return 5; }
};

Task<int> host_task(std::thread* thread)
{
    int v = co_await ThreadAwaitable{thread};
    co_return v + 1;
}

struct Counter
{
    int* count;
    explicit Counter(int* c) : count(c) {}
    Counter(const Counter&) = delete;
    ~Counter() { ++*count; }
};

Task<void> suspended_task(int* destroyed, std::function<void()>* slot)
{
    Counter c(destroyed);
    co_await Gate{slot};
}

// ---------------------------------------------------------------------------- basic

TEST(TaskTests, EmptyTask)
{
    Task<int> t;
    EXPECT_FALSE(t.valid());
    EXPECT_FALSE(t.done());
    t.resume(); // no effect
    try
    {
        t.result();
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "empty task");
    }
    EXPECT_THROW(hot::sync_wait(t), hot::ScriptError);
}

TEST(TaskTests, EmptyTaskAwaitThrows)
{
    auto co = [](Task<int>* t) -> Task<int> { co_return co_await *t; };
    Task<int> empty;
    Task<int> outer = co(&empty);
    outer.resume();
    EXPECT_TRUE(outer.done());
    EXPECT_THROW(outer.result(), hot::ScriptError);
}

TEST(TaskTests, LazyStart)
{
    bool started = false;
    Task<int> t = value_task(3, &started);
    EXPECT_TRUE(t.valid());
    EXPECT_FALSE(started);
    EXPECT_FALSE(t.done());
    try
    {
        t.result();
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task is not done");
    }
    t.resume();
    EXPECT_TRUE(started);
    EXPECT_TRUE(t.done());
    EXPECT_EQ(t.result(), 3);
}

TEST(TaskTests, MoveTransfersOwnership)
{
    Task<int> a = value_task(4);
    Task<int> b = std::move(a);
    EXPECT_FALSE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(hot::sync_wait(b), 4);
}

TEST(TaskTests, SyncWaitReturnsTheValue)
{
    EXPECT_EQ(hot::sync_wait(value_task(42)), 42);
}

TEST(TaskTests, VoidTask)
{
    int out = 0;
    Task<void> t = void_task(&out);
    EXPECT_EQ(out, 0);
    hot::sync_wait(t);
    EXPECT_EQ(out, 7);
    EXPECT_TRUE(t.done());
}

Task<std::string> string_task(std::string s)
{
    co_return s + "!";
}

Task<std::unique_ptr<int>> move_only_task(int v)
{
    co_return std::make_unique<int>(v);
}

Task<int> sum_move_only()
{
    std::unique_ptr<int> p = co_await move_only_task(9);
    co_return *p;
}

TEST(TaskTests, StringAndMoveOnlyResults)
{
    EXPECT_EQ(hot::sync_wait(string_task(std::string(100, 'a'))), std::string(100, 'a') + "!");
    std::unique_ptr<int> p = hot::sync_wait(move_only_task(5));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 5);
    EXPECT_EQ(hot::sync_wait(sum_move_only()), 9);
}

TEST(TaskTests, NestedAwaitOfAChildTask)
{
    EXPECT_EQ(hot::sync_wait(parent_task()), (3 + 10) * 2);
}

TEST(TaskTests, ExceptionReachesResult)
{
    Task<int> t = thrower();
    t.resume(); // does not throw
    EXPECT_TRUE(t.done());
    EXPECT_THROW(t.result(), hot::ScriptError);
}

TEST(TaskTests, ExceptionReachesSyncWait)
{
    Task<int> t = thrower();
    try
    {
        hot::sync_wait(t);
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "boom");
    }
}

Task<int> parent_of_thrower()
{
    int v = 0;
    try
    {
        v = co_await thrower();
    }
    catch (const hot::ScriptError&)
    {
        v = -1;
    }
    co_return v;
}

TEST(TaskTests, ExceptionOfAChildReachesTheAwaiter)
{
    EXPECT_EQ(hot::sync_wait(parent_of_thrower()), -1);
}

TEST(TaskTests, DestroyOfASuspendedFrameRunsLocalDestructors)
{
    int destroyed = 0;
    std::function<void()> slot;
    {
        Task<void> t = suspended_task(&destroyed, &slot);
        t.resume();
        EXPECT_FALSE(t.done());
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(TaskTests, MoveAssignOverASuspendedTaskDestroysIt)
{
    int destroyed = 0;
    std::function<void()> slot;
    Task<void> t = suspended_task(&destroyed, &slot);
    t.resume();
    EXPECT_EQ(destroyed, 0);
    t = Task<void>();
    EXPECT_EQ(destroyed, 1);
    EXPECT_FALSE(t.valid());
    int out = 0;
    t = void_task(&out); // assign over an empty task
    hot::sync_wait(t);
    EXPECT_EQ(out, 7);
}

// ---------------------------------------------------------------------------- threads

TEST(TaskTests, HostAwaitableResumedFromAnotherThread)
{
    std::thread thread;
    Task<int> t = host_task(&thread);
    const int v = hot::sync_wait(t);
    thread.join();
    EXPECT_EQ(v, 6);
}

Task<int> gate_task(std::function<void()>* slot, int v)
{
    co_await Gate{slot};
    co_return v;
}
Task<int> gate_task(TaskOriginData, std::function<void()>* slot, int v)
{
    co_await Gate{slot};
    co_return v;
}

TEST(TaskTests, SyncWaitAgainstEarlyCompletionFromAnotherThread)
{
    for (int i = 0; i < 300; ++i)
    {
        std::function<void()> slot;
        Task<int> t = gate_task(&slot, i);
        t.resume(); // runs to the gate
        ASSERT_TRUE(static_cast<bool>(slot));
        std::thread thread([&slot] { slot(); }); // can finish before or after sync_wait
        EXPECT_EQ(hot::sync_wait(t), i);
        thread.join();
    }
}

TEST(TaskTests, StaleResumeFromAHostThreadCancelsTheTask)
{
    uint32_t flag = 1;
    std::function<void()> slot;
    Task<int> t = gate_task(TaskOriginData{0, &flag, "host_t"}, &slot, 1);
    t.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    flag = 0;
    const uint64_t before = hot::stale_call_count();
    std::thread thread([&slot] { slot(); });
    try
    {
        hot::sync_wait(t);
        FAIL();
    }
    catch (const hot::TaskReloadCancelled& e)
    {
        EXPECT_STREQ(e.what(), "task cancelled by reload: host_t");
    }
    thread.join();
    EXPECT_EQ(hot::stale_call_count(), before + 1);
}

// ---------------------------------------------------------------------------- await rules

struct Steps
{
    int count = 0;
};

Task<int> stepping_child(Steps* s, std::function<void()>* slot)
{
    ++s->count;
    co_await Gate{slot};
    ++s->count;
    co_return 11;
}

Task<int> await_ptr(Task<int>* t)
{
    co_return co_await *t;
}

TEST(TaskTests, AwaitOfAStartedTaskDoesNotResumeIt)
{
    Steps s;
    std::function<void()> slot;
    Task<int> child = stepping_child(&s, &slot);
    child.resume();
    EXPECT_EQ(s.count, 1);
    child.resume(); // a started task: no effect
    EXPECT_EQ(s.count, 1);
    Task<int> parent = await_ptr(&child);
    parent.resume();
    EXPECT_EQ(s.count, 1); // the await did not resume the child
    EXPECT_FALSE(parent.done());
    slot(); // the host awaitable resumes the child; the child resumes the parent
    EXPECT_EQ(s.count, 2);
    EXPECT_TRUE(child.done());
    ASSERT_TRUE(parent.done());
    EXPECT_EQ(parent.result(), 11);
}

TEST(TaskTests, SecondAwaiterGetsAnError)
{
    Steps s;
    std::function<void()> slot;
    Task<int> child = stepping_child(&s, &slot);
    Task<int> first = await_ptr(&child);
    Task<int> second = await_ptr(&child);
    first.resume(); // starts the child
    EXPECT_EQ(s.count, 1);
    second.resume();
    ASSERT_TRUE(second.done());
    try
    {
        second.result();
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task is already awaited");
    }
    EXPECT_FALSE(first.done());
    slot();
    ASSERT_TRUE(first.done());
    EXPECT_EQ(first.result(), 11);
}

TEST(TaskTests, DestroyOfAnAwaitedChildGivesTheParentAnError)
{
    Steps s;
    std::function<void()> slot;
    Task<int> child = stepping_child(&s, &slot);
    Task<int> parent = await_ptr(&child);
    parent.resume();
    EXPECT_FALSE(parent.done());
    child = Task<int>(); // destroys the child while the parent awaits it
    ASSERT_TRUE(parent.done());
    try
    {
        parent.result();
        FAIL();
    }
    catch (const hot::TaskCancelled& e)
    {
        EXPECT_EQ(dynamic_cast<const hot::TaskReloadCancelled*>(&e), nullptr);
        EXPECT_STREQ(e.what(), "task destroyed while awaited");
    }
}

TEST(TaskTests, DestroyOfAnAwaitingParentLeavesTheChildValid)
{
    Steps s;
    std::function<void()> slot;
    Task<int> child = stepping_child(&s, &slot);
    {
        Task<int> parent = await_ptr(&child);
        parent.resume();
        EXPECT_FALSE(parent.done());
    } // the parent frame dies: the child has no continuation now
    slot();
    EXPECT_EQ(s.count, 2);
    ASSERT_TRUE(child.done());
    EXPECT_EQ(child.result(), 11);
}

TEST(TaskTests, MoveOfAnAwaitedTaskKeepsTheAwaiterValid)
{
    Steps s;
    std::function<void()> slot;
    Task<int> child = stepping_child(&s, &slot);
    Task<int> parent = await_ptr(&child);
    parent.resume();
    Task<int> moved = std::move(child);
    slot();
    ASSERT_TRUE(parent.done());
    EXPECT_EQ(parent.result(), 11);
    EXPECT_TRUE(moved.done());
}

Task<int> script_with_arg_co(TaskOriginData, Task<int> arg)
{
    co_return co_await arg;
}
Task<int> host_delay(int v)
{
    co_return v;
}

TEST(TaskTests, OriginIsTheFirstParameterNotAThreadLocal)
{
    // The host coroutine is made in the argument list. It does not take the origin.
    Task<int> host = host_delay(5);
    const hot::detail::TaskPromiseBase& hb = host.base();
    EXPECT_EQ(hb.pin.gen, 0u);
    EXPECT_EQ(hb.what, nullptr);
    EXPECT_EQ(hb.live, nullptr);
    Task<int> script = script_with_arg_co(TaskOriginData{7, &g_live, "S::m"}, std::move(host));
    EXPECT_EQ(script.base().pin.gen, 7u);
    EXPECT_STREQ(script.base().what, "S::m");
    EXPECT_EQ(hot::pin_count(7), 1u);
    EXPECT_EQ(hot::sync_wait(script), 5);
    script = Task<int>();
    EXPECT_EQ(hot::pin_count(7), 0u);
    // Both in one argument list.
    Task<int> s2 = script_with_arg_co(TaskOriginData{7, &g_live, "S::m"}, host_delay(6));
    EXPECT_EQ(s2.base().pin.gen, 7u);
    EXPECT_EQ(hot::sync_wait(s2), 6);
}

TEST(TaskTests, SecondResultOfAFinishedTaskThrows)
{
    Task<int> t = value_task(3);
    EXPECT_EQ(hot::sync_wait(t), 3);
    try
    {
        t.result();
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task result was already taken");
    }
}

TEST(TaskTests, SecondAwaitOfAFinishedTaskThrows)
{
    Task<std::string> t = string_task("x");
    t.resume();
    auto co = [](Task<std::string>* c) -> Task<std::string> { co_return co_await *c; };
    Task<std::string> first = co(&t);
    EXPECT_EQ(hot::sync_wait(first), "x!");
    Task<std::string> second = co(&t);
    try
    {
        hot::sync_wait(second);
        FAIL();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_STREQ(e.what(), "task result was already taken");
    }
}

TEST(TaskTests, SyncWaitResetsOnDone)
{
    std::function<void()> slot;
    Task<int> t = gate_task(&slot, 2);
    t.resume();
    std::thread thread([&slot] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); slot(); });
    EXPECT_EQ(hot::sync_wait(t), 2);
    thread.join();
    EXPECT_EQ(t.base().on_done, nullptr);
    EXPECT_EQ(t.base().on_done_ctx, nullptr);
}

TEST(TaskTests, ChildCompletesOnAThreadWhileTheParentIsDestroyed)
{
    for (int i = 0; i < 300; ++i)
    {
        std::function<void()> slot;
        Task<int> child = gate_task(&slot, i);
        child.resume(); // runs to the gate
        Task<int> parent = await_ptr(&child);
        parent.resume();
        ASSERT_TRUE(static_cast<bool>(slot));
        std::thread thread([&slot] { slot(); });
        parent = Task<int>(); // destroy the awaiting parent while the child completes
        thread.join();
        EXPECT_TRUE(child.done()); // the parent may have taken the value, or it was destroyed first
    }
}

// The thread completes the child, the parent completes inline, and then the thread destroys
// the child. The parent took the value before.
TEST(TaskTests, ChildDestroyedOnTheThreadAfterTheParentCompletes)
{
    for (int i = 0; i < 300; ++i)
    {
        std::function<void()> slot;
        auto child = std::make_unique<Task<int>>(gate_task(&slot, i));
        child->resume();
        Task<int> parent = await_ptr(child.get());
        parent.resume();
        std::thread thread([&slot, &child]
        {
            slot();
            child.reset();
        });
        thread.join();
        EXPECT_TRUE(parent.done());
        EXPECT_EQ(hot::sync_wait(parent), i);
    }
}

// Between the two awaits the frame tells the test that it runs, and it stays running for a
// short time. So the destroy of the test comes while the frame runs.
Task<int> await_two(Task<int>* first, Task<int>* second, std::atomic<bool>* between)
{
    const int a = co_await *first;
    between->store(true);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    const int b = co_await *second;
    co_return a + b;
}

// The parent runs on a host thread and links a second child. The main thread destroys the
// parent at the same time. The destroy waits for the run, and then it unlinks the second
// child. So the second child does not resume the dead parent.
TEST(TaskTests, ParentDestroyedWhileItLinksASecondChildOnAThread)
{
    for (int i = 0; i < 50; ++i)
    {
        std::function<void()> slot1, slot2;
        std::atomic<bool> between{false};
        Task<int> c1 = gate_task(&slot1, 1);
        c1.resume();
        ASSERT_TRUE(static_cast<bool>(slot1));
        Task<int> c2 = gate_task(&slot2, 2);
        auto parent = std::make_unique<Task<int>>(await_two(&c1, &c2, &between));
        parent->resume(); // the parent awaits c1
        std::thread thread([&slot1] { slot1(); }); // c1 ends, and the parent goes on to c2
        while (!between.load()) // the parent runs on the thread now
            std::this_thread::yield();
        parent.reset();
        thread.join();
        EXPECT_TRUE(c1.done());
        if (slot2) // c2 started and waits at its gate: its end must not resume the parent
        {
            slot2();
            EXPECT_TRUE(c2.done());
        }
    }
}

// ---------------------------------------------------------------------------- the frame after the suspend

// The probe stands for the MSVC ABI: after await_suspend returns, the coroutine writes the
// returned handle into its frame. The probe runs once for the target frame. It waits until the
// test destroyed the frame, or until the destroy waits for the frame. Then it reads the frame:
// ASan reports a read of a dead frame. The wait ends after a short time in all cases.
struct SuspendProbe
{
    static inline std::atomic<void*> target{nullptr};
    static inline std::atomic<bool> hit{false};
    static inline std::atomic<bool> destroyed{false};

    static void probe(void* frame)
    {
        void* expected = frame;
        if (!target.compare_exchange_strong(expected, nullptr))
            return;
        hit.store(true);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!destroyed.load() && hot::detail::task_waiters().load() == 0 && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        (void)*static_cast<volatile unsigned char*>(frame);
    }
    explicit SuspendProbe(void* frame)
    {
        hit.store(false);
        destroyed.store(false);
        target.store(frame);
        hot::detail::suspend_probe().store(&probe);
    }
    SuspendProbe(const SuspendProbe&) = delete;
    SuspendProbe& operator=(const SuspendProbe&) = delete;
    ~SuspendProbe()
    {
        hot::detail::suspend_probe().store(nullptr);
        target.store(nullptr);
    }
    static void wait_hit()
    {
        while (!hit.load())
            std::this_thread::yield();
    }
};

template <class T> void* frame_of(Task<T>& t)
{
    auto& p = static_cast<hot::detail::TaskPromise<T>&>(t.base());
    return std::coroutine_handle<hot::detail::TaskPromise<T>>::from_promise(p).address();
}

// A gate that also sets a flag when it holds the frame.
struct FlagGate
{
    std::function<void()>* slot;
    std::atomic<bool>* held;
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(std::coroutine_handle<P> h) const
    {
        *slot = [h] { hot::detail::resume_frame(h); };
        held->store(true);
    }
    void await_resume() const noexcept {}
};

Task<int> two_gates(std::function<void()>* slot1, std::function<void()>* slot2, std::atomic<bool>* held2)
{
    co_await Gate{slot1};
    co_await FlagGate{slot2, held2};
    co_return 3;
}

Task<int> gate_then_yield(std::function<void()>* slot)
{
    co_await Gate{slot};
    co_await hot::yield();
    co_return 4;
}

// Rule: after the link of a child, the thread that suspends the parent does not touch the
// parent frame. The test destroys the parent while the thread is at the end of its suspend.
TEST(TaskTests, ThreadDoesNotTouchTheParentAfterItLinksAChild)
{
    std::function<void()> slot1, slot2;
    std::atomic<bool> between{false};
    Task<int> c1 = gate_task(&slot1, 1);
    c1.resume();
    Task<int> c2 = gate_task(&slot2, 2);
    auto parent = std::make_unique<Task<int>>(await_two(&c1, &c2, &between));
    parent->resume(); // the parent awaits c1
    SuspendProbe probe(frame_of(*parent));
    std::thread thread([&slot1] { slot1(); }); // the parent links c2 on the thread
    SuspendProbe::wait_hit();
    parent.reset();
    SuspendProbe::destroyed.store(true);
    thread.join();
    ASSERT_TRUE(static_cast<bool>(slot2)); // c2 started on the thread
    slot2();
    EXPECT_TRUE(c2.done());
}

// Rule: after the completion, the thread that ends the frame does not touch it.
TEST(TaskTests, ThreadDoesNotTouchTheFrameAfterItEnds)
{
    std::function<void()> slot;
    auto t = std::make_unique<Task<int>>(gate_task(&slot, 7));
    t->resume();
    SuspendProbe probe(frame_of(*t));
    std::thread thread([&slot] { slot(); }); // the frame ends on the thread
    SuspendProbe::wait_hit();
    t.reset();
    SuspendProbe::destroyed.store(true);
    thread.join();
}

// Rule: after the park, the thread that suspends the frame does not touch it.
TEST(TaskTests, ThreadDoesNotTouchTheFrameAfterItParks)
{
    std::function<void()> slot;
    auto t = std::make_unique<Task<int>>(gate_then_yield(&slot));
    t->resume();
    SuspendProbe probe(frame_of(*t));
    std::thread thread([&slot] { slot(); }); // the frame parks on the thread
    SuspendProbe::wait_hit();
    t.reset();
    SuspendProbe::destroyed.store(true);
    thread.join();
    EXPECT_EQ(hot::scheduler().size(), 0u);
}

// Rule: after a host awaitable holds the frame, the thread that suspends the frame does not
// touch it. The test resumes the frame from the second gate, and it destroys the task.
TEST(TaskTests, ThreadDoesNotTouchTheFrameAfterAHostAwaitableHoldsIt)
{
    std::function<void()> slot1, slot2;
    std::atomic<bool> held2{false};
    auto t = std::make_unique<Task<int>>(two_gates(&slot1, &slot2, &held2));
    t->resume();
    SuspendProbe probe(frame_of(*t));
    std::thread thread([&slot1] { slot1(); }); // the second gate holds the frame on the thread
    SuspendProbe::wait_hit();
    while (!held2.load())
        std::this_thread::yield();
    slot2(); // the frame ends here
    EXPECT_EQ(hot::sync_wait(*t), 3);
    t.reset();
    SuspendProbe::destroyed.store(true);
    thread.join();
}

// ---------------------------------------------------------------------------- cancellation

Task<int> cancelled_body()
{
    throw hot::TaskCancelled("user cancel");
    co_return 1;
}

Task<int> rethrowing_parent(Task<int>* child)
{
    try
    {
        co_return co_await *child;
    }
    catch (const hot::TaskCancelled&)
    {
        throw; // the dynamic type stays
    }
}

TEST(TaskTests, CancelTypesAreCatchableAsTheirBases)
{
    uint32_t flag = 1;
    std::function<void()> slot;
    Task<int> child = gate_task(TaskOriginData{0, &flag, "c"}, &slot, 1);
    child.resume();
    Task<int> parent = rethrowing_parent(&child);
    Task<int> grand = await_ptr(&parent);
    grand.resume();
    flag = 0;
    slot();
    ASSERT_TRUE(grand.done());
    // The host sees the type through two parents.
    try
    {
        grand.result();
        FAIL();
    }
    catch (const hot::TaskReloadCancelled& e)
    {
        EXPECT_STREQ(e.what(), "task cancelled by reload: c");
    }
    EXPECT_THROW(parent.result(), hot::TaskCancelled);
    EXPECT_THROW(child.result(), hot::ScriptError);
}

// ---------------------------------------------------------------------------- destroy from the own run

// Ends the process when a test hangs for too long. A deadlock cannot fail a test in another way.
struct Watchdog
{
    std::atomic<bool> stop{false};
    std::thread thread;
    explicit Watchdog(int seconds = 20)
    {
        thread = std::thread(
            [this, seconds]
            {
                for (int i = 0; i < seconds * 100 && !stop.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (!stop.load())
                {
                    std::fprintf(stderr, "task test hangs: deadlock\n");
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

struct Owner
{
    Task<int> task;
};

Task<int> destroys_itself_then_suspends(Owner* o, int* frames, int* after, std::function<void()>* slot)
{
    Counter c(frames);
    o->task = Task<int>{}; // the body destroys its own task
    *after = 1;            // the body runs on
    co_await Gate{slot};
    *after = 2; // not reached: the frame ends at the co_await
    co_return 1;
}

TEST(TaskTests, TheBodyDestroysItsOwnTaskAndEndsAtTheNextSuspension)
{
    Watchdog dog;
    Owner o;
    int frames = 0, after = 0;
    std::function<void()> slot;
    o.task = destroys_itself_then_suspends(&o, &frames, &after, &slot);
    o.task.resume();
    EXPECT_EQ(after, 1);
    EXPECT_EQ(frames, 1);
    EXPECT_FALSE(o.task.valid());
    EXPECT_FALSE(static_cast<bool>(slot));
}

Task<int> destroys_itself_then_ends(Owner* o, int* frames)
{
    Counter c(frames);
    o->task = Task<int>{};
    co_return 5;
}

TEST(TaskTests, TheBodyDestroysItsOwnTaskAndEndsWithoutSuspension)
{
    Watchdog dog;
    Owner o;
    int frames = 0;
    o.task = destroys_itself_then_ends(&o, &frames);
    o.task.resume();
    EXPECT_EQ(frames, 1);
    EXPECT_FALSE(o.task.valid());
}

Task<int> counted_gate(int* frames, std::function<void()>* slot)
{
    Counter c(frames);
    co_await Gate{slot};
    co_return 1;
}

// Gives a frame that is suspended at a Gate and marked as running, with a deferred destroy.
// This is the state of a body that destroyed its own task and now suspends.
Task<int>::Handle self_destroyed_frame(int* frames, std::function<void()>* slot)
{
    Task<int> t = counted_gate(frames, slot);
    t.resume();
    Task<int>::Handle h = t.release();
    std::lock_guard<std::mutex> lock(hot::detail::task_mutex());
    h.promise().begin_run();
    h.promise().destroy_requested = true;
    return h;
}

// The MSVC ABI returns the handle of await_suspend through a pointer into the frame. So the frame
// must live when await_suspend returns. The handle that it returns ends the frame.
TEST(TaskTests, ADeferredDestroyKeepsTheFrameUntilTheAwaiterReturns)
{
    Watchdog dog;
    int frames = 0;
    std::function<void()> slot;
    Task<int>::Handle h = self_destroyed_frame(&frames, &slot);
    auto w = h.promise().await_transform(std::suspend_always{});
    std::coroutine_handle<> next = w.await_suspend(h);
    EXPECT_EQ(frames, 0);
    next.resume();
    EXPECT_EQ(frames, 1);
}

TEST(TaskTests, ADeferredDestroyAtTheEndKeepsTheFrameUntilTheAwaiterReturns)
{
    Watchdog dog;
    int frames = 0;
    std::function<void()> slot;
    Task<int>::Handle h = self_destroyed_frame(&frames, &slot);
    std::coroutine_handle<> next = hot::detail::TaskPromiseBase::FinalAwaiter{}.await_suspend(h);
    EXPECT_EQ(frames, 0);
    next.resume();
    EXPECT_EQ(frames, 1);
}

Task<void> catch_cancel(Task<int>* t, int* out)
{
    try
    {
        *out = co_await *t;
    }
    catch (const hot::TaskCancelled&)
    {
        *out = -1;
    }
}

TEST(TaskTests, TheAwaitedBodyDestroysItsOwnTaskTheAwaiterGetsTheCancel)
{
    Watchdog dog;
    Owner o;
    int frames = 0, out = 0;
    o.task = destroys_itself_then_ends(&o, &frames);
    Task<void> parent = catch_cancel(&o.task, &out);
    parent.resume(); // the body destroys its task at the first run
    EXPECT_TRUE(parent.done());
    EXPECT_EQ(out, -1);
    EXPECT_EQ(frames, 1);
}

Task<int> destroys_itself_after_child(Owner* o, Task<int>* child, int* frames, int* after)
{
    Counter c(frames);
    const int v = co_await *child;
    o->task = Task<int>{}; // the continuation runs inline in the thread that completed the child
    *after = v;
    co_return v;
}

TEST(TaskTests, InlineContinuationDestroysItsOwnTask)
{
    Watchdog dog;
    std::function<void()> slot;
    Task<int> child = gate_task(&slot, 9);
    Owner o;
    int frames = 0, after = 0;
    o.task = destroys_itself_after_child(&o, &child, &frames, &after);
    o.task.resume(); // the parent awaits the child, the child waits at the gate
    ASSERT_TRUE(static_cast<bool>(slot));
    EXPECT_EQ(frames, 0);
    slot(); // the child completes; complete() resumes the parent inline
    EXPECT_EQ(after, 9);
    EXPECT_EQ(frames, 1);
    EXPECT_FALSE(o.task.valid());
}

TEST(TaskTests, InlineContinuationOnAThreadDestroysItsOwnTask)
{
    Watchdog dog;
    for (int i = 0; i < 100; ++i)
    {
        std::function<void()> slot;
        Task<int> child = gate_task(&slot, i);
        Owner o;
        int frames = 0, after = -1;
        o.task = destroys_itself_after_child(&o, &child, &frames, &after);
        o.task.resume();
        std::thread thread([&slot] { slot(); });
        thread.join();
        EXPECT_EQ(after, i);
        EXPECT_EQ(frames, 1);
    }
}

struct HolderOfATask
{
    Task<void> task;
    int* deleted;
    ~HolderOfATask() { ++*deleted; }
};

Task<void> deletes_its_owner(HolderOfATask* h, int* frames, std::function<void()>* slot)
{
    Counter c(frames);
    co_await Gate{slot};
    delete h; // the owner of the task dies in the task
    co_return;
}

TEST(TaskTests, TheBodyDeletesTheObjectThatOwnsItsTask)
{
    Watchdog dog;
    int deleted = 0, frames = 0;
    std::function<void()> slot;
    auto* h = new HolderOfATask{Task<void>{}, &deleted};
    h->task = deletes_its_owner(h, &frames, &slot);
    h->task.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    slot();
    EXPECT_EQ(deleted, 1);
    EXPECT_EQ(frames, 1);
}

// The host awaitable throws from await_suspend: the exception comes out of the co_await and the
// frame runs again.
struct ThrowingAwaitable
{
    hot::detail::TaskPromiseBase** seen;
    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(std::coroutine_handle<P> h) const
    {
        *seen = &static_cast<hot::detail::TaskPromiseBase&>(h.promise());
        throw std::runtime_error("no suspend");
    }
    void await_resume() const noexcept {}
};

Task<int> catches_the_throw(hot::detail::TaskPromiseBase** seen, bool* running_in_catch)
{
    try
    {
        co_await ThrowingAwaitable{seen};
    }
    catch (const std::runtime_error&)
    {
        *running_in_catch = (*seen)->running.load();
        co_return 3;
    }
    co_return 0;
}

TEST(TaskTests, ThrowFromAwaitSuspendOfAHostAwaitableKeepsTheFrameRunning)
{
    Watchdog dog;
    hot::detail::TaskPromiseBase* seen = nullptr;
    bool running_in_catch = false;
    Task<int> t = catches_the_throw(&seen, &running_in_catch);
    EXPECT_EQ(hot::sync_wait(t), 3);
    EXPECT_TRUE(running_in_catch);
    EXPECT_FALSE(t.base().running.load());
}

// ---------------------------------------------------------------------------- await cycles

Task<int> awaits_task(Task<int>* other)
{
    co_return co_await *other;
}

static std::string error_of(Task<int>& t)
{
    try
    {
        hot::sync_wait(t);
    }
    catch (const hot::ScriptError& e)
    {
        return e.what();
    }
    return "";
}

TEST(TaskTests, TaskThatAwaitsItselfGetsAnError)
{
    Watchdog dog;
    Task<int> t;
    t = awaits_task(&t);
    EXPECT_EQ(error_of(t), "task awaits itself");
}

TEST(TaskTests, TwoTasksThatAwaitEachOtherGetAnError)
{
    Watchdog dog;
    Task<int> a, b;
    a = awaits_task(&b);
    b = awaits_task(&a);
    EXPECT_EQ(error_of(a), "task await cycle");
    EXPECT_TRUE(b.done());
}

// A coroutine of the host (not a task) is not in the chain of awaiting tasks.
struct Fire
{
    struct promise_type
    {
        Fire get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

Fire foreign_wait(Task<int>* t, int* cancelled, std::function<void()>* wake)
{
    try
    {
        co_await *t;
    }
    catch (const hot::TaskCancelled&)
    {
        *cancelled = 1;
    }
    (*wake)();
}

Task<int> cycle_b(Task<int>* a, int* cancelled, std::function<void()>* slot)
{
    foreign_wait(a, cancelled, slot); // waits for "a"
    co_await Gate{slot};              // and this task waits for the foreign coroutine
    co_return 7;
}

TEST(TaskTests, DestroyEndsACycleThatTheCheckDoesNotSee)
{
    Watchdog dog;
    Task<int> a, b;
    int cancelled = 0;
    std::function<void()> slot;
    a = awaits_task(&b);
    b = cycle_b(&a, &cancelled, &slot);
    a.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    EXPECT_FALSE(a.done());
    EXPECT_FALSE(b.done());
    a = Task<int>{}; // the foreign coroutine gets the cancel, then it wakes "b"
    EXPECT_EQ(cancelled, 1);
    EXPECT_TRUE(b.done());
    EXPECT_EQ(b.result(), 7);
}

// ---------------------------------------------------------------------------- unhandled

struct ErrorCapture : ::testing::Test
{
    std::vector<std::string> reports;
    void SetUp() override
    {
        hot::set_script_error_handler([this](const hot::ScriptErrorReport& r) { reports.push_back(r.message); });
    }
    void TearDown() override { hot::set_script_error_handler(nullptr); }
};

TEST_F(ErrorCapture, UnobservedErrorIsReported)
{
    {
        Task<int> t = thrower();
        t.resume();
    }
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0], "boom");
}

TEST_F(ErrorCapture, CancellationIsNotReported)
{
    {
        Task<int> t = cancelled_body();
        t.resume();
        ASSERT_TRUE(t.done());
    }
    {
        Task<int> t = cancelled_body();
        Task<int> parent = await_ptr(&t);
        parent.resume();
        ASSERT_TRUE(parent.done());
    }
    Task<int> t = cancelled_body();
    EXPECT_THROW(hot::sync_wait(t), hot::TaskCancelled);
    EXPECT_TRUE(reports.empty());
}

TEST_F(ErrorCapture, ObservedErrorIsNotReported)
{
    {
        Task<int> t = thrower();
        t.resume();
        EXPECT_THROW(t.result(), hot::ScriptError);
    }
    EXPECT_EQ(hot::sync_wait(parent_of_thrower()), -1);
    EXPECT_TRUE(reports.empty());
}

// ---------------------------------------------------------------------------- reload

// A class record is needed: the generation number of a module comes from its records.
struct TFake : hot::ShellBase
{
    virtual ~TFake() { hot::destroy_state(this); }
};
struct TState
{
    int value;
};
hot::FieldInfo t_fields[] = {
    {"value", nullptr, hot::type_key<int>(), offsetof(TState, value), sizeof(int), alignof(int),
        hot::field_flags<int>(), [](void*, void* f) { new (f) int(1); }, hot::move_field<int>,
        hot::field_destroyer<int>()},
};
hot::StateLayout t_layout{"TState", sizeof(TState), alignof(TState), 1, t_fields};
hot::SlotFn t_slots[1] = {nullptr};
constexpr uint64_t kTHash = hot::shell_layout_hash("Base", sizeof(TFake), alignof(TFake), 0);
hot::ClassRecord t_rec1{"TFake", 1, t_slots, 1, &t_layout, nullptr, "Base", kTHash};
hot::ClassRecord t_rec2{"TFake", 2, t_slots, 1, &t_layout, nullptr, "Base", kTHash};
hot::Instance create_t1()
{
    auto* s = new TFake;
    hot::init_state(s, &t_rec1);
    return {s, s};
}
hot::Instance create_t2()
{
    auto* s = new TFake;
    hot::init_state(s, &t_rec2);
    return {s, s};
}

uint32_t g_flag1 = 0;
uint32_t g_flag2 = 0;
void register_flag1(hot::Registry& reg)
{
    reg.set_live_flag(&g_flag1);
    reg.add(&t_rec1, &create_t1);
}
void register_flag2(hot::Registry& reg)
{
    reg.set_live_flag(&g_flag2);
    reg.add(&t_rec2, &create_t2);
}
hot::Module module_of(hot::Module::RegisterFn fn, const char* name)
{
    hot::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

Task<int> traced_co(TaskOriginData, bool* ran)
{
    *ran = true;
    co_return 1;
}
Task<int> traced(uint32_t gen, const uint32_t* live, const char* what, bool* ran)
{
    return traced_co(TaskOriginData{gen, live, what}, ran);
}

Task<int> catch_parent(Task<int>* child, std::string* message)
{
    try
    {
        co_return co_await *child;
    }
    catch (const hot::ScriptError& e)
    {
        *message = e.what();
    }
    co_return -1;
}

struct TaskReload : ::testing::Test
{
    hot::Loader loader;
    void SetUp() override
    {
        g_flag1 = 0;
        g_flag2 = 0;
        t_rec1 = hot::ClassRecord{"TFake", 1, t_slots, 1, &t_layout, nullptr, "Base", kTHash};
        t_rec2 = hot::ClassRecord{"TFake", 2, t_slots, 1, &t_layout, nullptr, "Base", kTHash};
    }
    void reload_to_2()
    {
        ASSERT_RELOAD_OK(loader.request_reload(module_of(register_flag2, "gen2")));
        ASSERT_RELOAD_OK(loader.commit_pending());
        ASSERT_EQ(hot::active_generation, 2u);
        ASSERT_EQ(g_flag1, 0u);
        ASSERT_EQ(g_flag2, 1u);
    }
};

TEST_F(TaskReload, TaskCreatedBeforeAReloadIsCancelledAtTheFirstResume)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    ASSERT_EQ(g_flag1, 1u);
    bool ran = false;
    Task<int> t = traced(1, &g_flag1, "Cls::m", &ran);
    EXPECT_EQ(hot::pin_count(1), 1u); // the promise took the pin before the first suspension
    reload_to_2();
    const uint64_t before = hot::stale_call_count();
    t.resume();
    EXPECT_FALSE(ran); // the old code did not run
    ASSERT_TRUE(t.done());
    EXPECT_EQ(hot::stale_call_count(), before + 1);
    try
    {
        t.result();
        FAIL();
    }
    catch (const hot::TaskReloadCancelled& e)
    {
        EXPECT_STREQ(e.what(), "task cancelled by reload: Cls::m");
    }
}

TEST_F(TaskReload, AwaitOfATaskCreatedBeforeAReloadGetsTheError)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    bool ran = false;
    Task<int> child = traced(1, &g_flag1, "Cls::child", &ran);
    reload_to_2();
    std::string message;
    Task<int> parent = catch_parent(&child, &message);
    hot::sync_wait(parent);
    EXPECT_FALSE(ran);
    EXPECT_EQ(message, "task cancelled by reload: Cls::child");
}

TEST_F(TaskReload, DestroyOfANeverStartedStaleTaskWorks)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    {
        bool ran = false;
        Task<int> t = traced(1, &g_flag1, "Cls::m", &ran);
        reload_to_2();
        EXPECT_EQ(hot::pin_count(1), 1u);
    }
    EXPECT_EQ(hot::pin_count(1), 0u);
}

TEST_F(TaskReload, PinBlocksResetUntilTheTaskIsDestroyed)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    std::string error;
    {
        bool ran = false;
        Task<int> t = traced(1, &g_flag1, "Cls::m", &ran);
        EXPECT_FALSE(loader.reset(error));
        EXPECT_EQ(error, "generation 1 is pinned (a closure or a task is alive)");
    }
    EXPECT_EQ(hot::pin_count(1), 0u);
    EXPECT_TRUE(loader.reset(error)) << error;
}

Task<int> pass_through(TaskOriginData, Task<int>* child)
{
    co_return co_await *child;
}

TEST_F(TaskReload, StaleParentInAChainGetsItsOwnMessage)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    // grandparent (gen 0, live) awaits parent (gen 1) awaits child (gen 0, waits at a gate).
    std::function<void()> slot;
    Task<int> child = gate_task(&slot, 5);
    Task<int> parent = pass_through(TaskOriginData{1, &g_flag1, "Cls::parent"}, &child);
    std::string message;
    Task<int> grand = catch_parent(&parent, &message);
    grand.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    EXPECT_FALSE(grand.done());
    reload_to_2(); // the parent code is not live now
    const uint64_t before = hot::stale_call_count();
    slot(); // the child finishes; the parent is stale and does not resume
    EXPECT_TRUE(child.done());
    ASSERT_TRUE(parent.done());
    ASSERT_TRUE(grand.done());
    EXPECT_EQ(grand.result(), -1);
    EXPECT_EQ(message, "task cancelled by reload: Cls::parent");
    EXPECT_EQ(hot::stale_call_count(), before + 1);
}

// The stale handler runs without the link lock: it can read the tasks and other threads can use them.
static hot::Task<int>* g_hook_parent = nullptr;
static hot::Task<int>* g_hook_child = nullptr;
static std::atomic<int> g_hook_calls{0};
static std::atomic<int> g_hook_lock_free{0};
static std::atomic<int> g_hook_child_done{0};

static void stale_hook(const char*)
{
    ++g_hook_calls;
    if (g_hook_child && g_hook_child->done()) // lock-free read of the flag
        ++g_hook_child_done;
    bool got = false;
    std::thread other(
        [&got]
        {
            got = hot::detail::task_mutex().try_lock();
            if (got)
                hot::detail::task_mutex().unlock();
        });
    other.join();
    if (got)
        ++g_hook_lock_free;
}

TEST_F(TaskReload, StaleCallRunsWithoutTheLinkLock)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_flag1, "gen1")));
    std::function<void()> slot;
    Task<int> child = gate_task(&slot, 5);
    Task<int> parent = pass_through(TaskOriginData{1, &g_flag1, "Cls::parent"}, &child);
    std::string message;
    Task<int> grand = catch_parent(&parent, &message);
    grand.resume();
    ASSERT_TRUE(static_cast<bool>(slot));
    reload_to_2();
    g_hook_parent = &parent;
    g_hook_child = &child;
    g_hook_calls = 0;
    g_hook_lock_free = 0;
    g_hook_child_done = 0;
    auto* old = hot::set_stale_hook(stale_hook);
    {
        Watchdog dog;
        slot();
    }
    hot::set_stale_hook(old);
    EXPECT_EQ(g_hook_calls.load(), 1);
    EXPECT_EQ(g_hook_lock_free.load(), 1);
    EXPECT_EQ(g_hook_child_done.load(), 1);
    EXPECT_TRUE(grand.done());
    EXPECT_EQ(message, "task cancelled by reload: Cls::parent");
}

} // namespace

#endif // HOT_HAS_TASK
