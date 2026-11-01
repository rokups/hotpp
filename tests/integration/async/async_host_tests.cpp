// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Async script methods at run time. A real host calls the methods through the thunks, gets a
// hot::Task and drives it. The scripts are async_host_v1.hot (and v2), async_override.hot and
// async_cpp.cpp. Each test runs with the clang files backend, with the JIT backend and with the
// AOT code of the scripts (Loader::load_aot). A mode that is not in the build is skipped.
//
// The host awaitable is the gate of async_gate.h. A script frame waits at the gate, and the
// test opens it.
//
// CMake defines HOT_SOURCE_DIR and ASYNC_INCLUDE_DIR.
#include "Actor.h"
#include "async_gate.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "hot/runtime/scheduler.h"
#include "hot/runtime/task.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

asynctest::Gate& asynctest::gate()
{
    static Gate g;
    return g;
}

namespace
{

using asynctest::gate;
using test_support::source;

enum class Mode
{
    Files,
    Jit,
    Aot,
};

const hot::GenerationInfo* find_gen(const hot::LoaderStats& st, uint32_t generation)
{
    for (const hot::GenerationInfo& g : st.generations)
        if (g.generation == generation)
            return &g;
    return nullptr;
}

// The loader, the backend and one script object. The loader is the first member, so it is
// the last to go: the code stays loaded while the object dies. A test declares its tasks after
// the host, so the tasks die first.
struct Host
{
    hot::Loader loader;
    std::unique_ptr<hot::Backend> backend;
    hot::Instance inst{};
    Actor* actor = nullptr;

    ~Host() { delete actor; }
};

using TaskFn1 = hot::Task<int32_t> (*)(Actor*, int32_t);
using TaskFn0 = hot::Task<int32_t> (*)(Actor*);

class AsyncHost : public ::testing::TestWithParam<Mode>
{
public:
    void SetUp() override
    {
        const Mode m = GetParam();
        if (m == Mode::Files && !hot::backend_available(hot::BackendKind::ClangFiles))
            GTEST_SKIP() << "the clang files backend is not in this build";
        if (m == Mode::Jit && !hot::backend_available(hot::BackendKind::Jit))
            GTEST_SKIP() << "the JIT backend is not in this build";
        gate().drop();
        gate().waits = 0;
    }
    void TearDown() override
    {
        hot::scheduler().cancel_all();
        gate().drop();
    }

    bool aot() const { return GetParam() == Mode::Aot; }

    // The backend of the scripts: C++20, the engine headers and the gate header.
    std::unique_ptr<hot::Backend> make_backend() const
    {
        hot::DefaultBackendOptions o;
        if (GetParam() == Mode::Files)
            o.kind = hot::BackendKind::ClangFiles;
        else if (GetParam() == Mode::Jit)
            o.kind = hot::BackendKind::Jit;
        o.std_version = "c++20";
        o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", ASYNC_INCLUDE_DIR};
        return hot::make_default_backend(o);
    }

    // Loads generation 1 of the script (or the AOT table) and makes an object of the class.
    void start(Host& h, const char* file, const char* cls)
    {
        h.backend = make_backend();
        if (aot())
        {
            h.backend->seed_slots(hot::aot_table.slots);
            const hot::ReloadResult r = h.loader.load_aot(hot::aot_table);
            ASSERT_TRUE(r) << r.error;
        }
        else
        {
            hot::LoadResult built = h.backend->load(source(file), 1);
            ASSERT_TRUE(built) << built.error;
            const hot::ReloadResult r = h.loader.load(std::move(*built.module));
            ASSERT_TRUE(r) << r.error;
        }
        const hot::ClassEntry* e = h.loader.find(cls);
        ASSERT_NE(e, nullptr) << "class " << cls << " is not registered";
        h.inst = e->create();
        h.actor = static_cast<Actor*>(h.inst.native);
    }

    // Builds the script as the generation and commits the reload. "unit" is the unit of the
    // AOT script that the new generation replaces.
    void reload(Host& h, const char* file, uint32_t generation, const char* unit)
    {
        hot::LoadResult built = h.backend->load(source(file), generation);
        ASSERT_TRUE(built) << built.error;
        if (aot())
            built.module->unit = unit;
        const hot::ReloadResult q = h.loader.request_reload(std::move(*built.module));
        ASSERT_TRUE(q) << q.error;
        const hot::ReloadResult d = h.loader.commit_pending();
        ASSERT_TRUE(d) << d.error;
        h.loader.frame_boundary();
    }

    static hot::Task<int32_t> call(Host& h, const char* name, int32_t x)
    {
        void* t = h.loader.thunk(name);
        if (!t)
            throw std::runtime_error(std::string("no thunk ") + name);
        return reinterpret_cast<TaskFn1>(t)(h.actor, x);
    }
    static hot::Task<int32_t> call0(Host& h, const char* name)
    {
        void* t = h.loader.thunk(name);
        if (!t)
            throw std::runtime_error(std::string("no thunk ") + name);
        return reinterpret_cast<TaskFn0>(t)(h.actor);
    }

    // A call of a sync script method (async_sched_v1.hot) through its thunk.
    template <class R, class... A> static R sync_call(Host& h, const char* name, A... args)
    {
        void* t = h.loader.thunk(name);
        if (!t)
            throw std::runtime_error(std::string("no thunk ") + name);
        return reinterpret_cast<R (*)(Actor*, A...)>(t)(h.actor, args...);
    }
    static int ticks(Host& h)
    {
        int* p = hot::field_ptr<int>(h.inst.shell, "ticks");
        return p ? *p : -1;
    }

    static int steps(Host& h)
    {
        int* p = hot::field_ptr<int>(h.inst.shell, "steps");
        return p ? *p : -1;
    }
};

// ---------------------------------------------------------------- the host drives the tasks

TEST_P(AsyncHost, SyncWaitGivesTheValue)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Twice", 21);
    ASSERT_TRUE(t.valid());
    EXPECT_FALSE(t.done()); // a task is lazy
    EXPECT_EQ(hot::sync_wait(t), 42);
}

TEST_P(AsyncHost, HostDrivesTheTaskWithResume)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Twice", 4);
    EXPECT_FALSE(t.done());
    t.resume();
    ASSERT_TRUE(t.done());
    EXPECT_EQ(t.result(), 8);
    EXPECT_THROW(t.result(), hot::ScriptError); // the result moves out once
}

// The script frame suspends at the host awaitable. The host resumes it.
TEST_P(AsyncHost, HostAwaitableSuspendsTheFrame)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Gated", 1);
    t.resume();
    EXPECT_FALSE(t.done());
    EXPECT_TRUE(gate().waiting());
    EXPECT_EQ(steps(h), 1);
    EXPECT_THROW(t.result(), hot::ScriptError); // "task is not done"
    t.resume();                                 // a started task: it does nothing
    EXPECT_EQ(steps(h), 1);
    ASSERT_TRUE(gate().open(10));
    ASSERT_TRUE(t.done());
    EXPECT_EQ(steps(h), 2);
    EXPECT_EQ(t.result(), 11);
}

// Parent awaits Twice, then Gated. The gate resumes the child, and the child resumes the parent.
TEST_P(AsyncHost, ParentAwaitsChild)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Parent", 3);
    t.resume();
    EXPECT_FALSE(t.done());
    ASSERT_TRUE(gate().waiting());
    ASSERT_TRUE(gate().open(1));
    ASSERT_TRUE(t.done());
    EXPECT_EQ(t.result(), 6 + 7);
}

TEST_P(AsyncHost, ScriptErrorAfterResumeReachesResult)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call0(h, "AsyncRunner::FailAfterGate");
    t.resume();
    EXPECT_FALSE(t.done());
    ASSERT_TRUE(gate().open(0));
    ASSERT_TRUE(t.done());
    EXPECT_EQ(steps(h), 10);
    try
    {
        (void)t.result();
        ADD_FAILURE() << "no error";
    }
    catch (const hot::TaskCancelled& e)
    {
        ADD_FAILURE() << "a cancellation: " << e.what();
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_NE(std::string(e.what()).find("boom after resume"), std::string::npos) << e.what();
    }
}

// The script throws hot.TaskCancelled. The result has the dynamic type. A script catch of
// hot.TaskCancelled sees it at the await.
TEST_P(AsyncHost, ScriptThrowsTaskCancelled)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    {
        hot::Task<int32_t> t = call0(h, "AsyncRunner::Cancel");
        EXPECT_FALSE(t.done()); // the throw is in the task, not in the call
        t.resume();
        ASSERT_TRUE(t.done());
        try
        {
            (void)t.result();
            ADD_FAILURE() << "no error";
        }
        catch (const hot::TaskReloadCancelled& e)
        {
            ADD_FAILURE() << "a reload cancellation: " << e.what();
        }
        catch (const hot::TaskCancelled& e)
        {
            EXPECT_STREQ(e.what(), "cancelled by script");
        }
    }
    EXPECT_EQ(hot::sync_wait(call0(h, "AsyncRunner::CatchCancel")), -7);
}

// The host calls the thunk of the base method. The object is of the derived class, so the
// override runs. "Run" calls Compute through the slot inside the script.
TEST_P(AsyncHost, VirtualOverrideThroughTheBase)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_override.hot", "AsyncDerived"));
    EXPECT_EQ(hot::sync_wait(call(h, "AsyncBase::Compute", 2)), 30);
    EXPECT_EQ(hot::sync_wait(call(h, "AsyncBase::Run", 4)), 50);
}

// A ".cpp" script with plain hot::Task coroutines.
TEST_P(AsyncHost, CppScriptCoroutine)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_cpp.cpp", "CppAsync"));
    hot::Task<int32_t> t = call(h, "CppAsync::Quarter", 20);
    EXPECT_EQ(hot::sync_wait(t), 5);
    int* calls = hot::field_ptr<int>(h.inst.shell, "calls");
    ASSERT_NE(calls, nullptr);
    EXPECT_EQ(*calls, 1);
    // The state of the class gives the origin, so a task of the ".cpp" script pins generation 1.
    const uint32_t pins0 = hot::pin_count(1);
    {
        hot::Task<int32_t> lazy = call(h, "CppAsync::Half", 8);
        EXPECT_EQ(hot::pin_count(1), pins0 + 1);
    }
    EXPECT_EQ(hot::pin_count(1), pins0);
}

// A ".cpp" coroutine of generation 1 that did not start is cancelled at its start after a reload.
TEST_P(AsyncHost, CppCoroutineIsCancelledAfterReload)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_cpp.cpp", "CppAsync"));
    hot::Task<int32_t> t = call(h, "CppAsync::Quarter", 20);
    ASSERT_NO_FATAL_FAILURE(reload(h, "async_cpp.cpp", 2, "async_cpp"));
    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    t.resume();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
    ASSERT_TRUE(t.done());
    EXPECT_EQ(*hot::field_ptr<int>(h.inst.shell, "calls"), 0);
    try
    {
        (void)t.result();
        ADD_FAILURE() << "no error";
    }
    catch (const hot::TaskReloadCancelled& e)
    {
        EXPECT_STREQ(e.what(), "task cancelled by reload: CppAsync");
    }
    EXPECT_EQ(hot::sync_wait(call(h, "CppAsync::Quarter", 20)), 5); // generation 2 runs
}

// ---------------------------------------------------------------- reloads

// A task waits at the gate. A reload makes its code stale. The resume cancels the task: the
// old code does not run. The new generation works.
TEST_P(AsyncHost, ReloadCancelsTheSuspendedTask)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Gated", 1);
    t.resume();
    ASSERT_TRUE(gate().waiting());
    EXPECT_EQ(steps(h), 1);

    ASSERT_NO_FATAL_FAILURE(reload(h, "async_host_v2.hot", 2, "async_host_v1"));
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_FALSE(t.done());

    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    ASSERT_TRUE(gate().open(5));
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
    EXPECT_NE(log.find("AsyncRunner::Gated"), std::string::npos) << log;
    ASSERT_TRUE(t.done());
    EXPECT_EQ(steps(h), 1); // the rest of the old body did not run
    try
    {
        (void)t.result();
        ADD_FAILURE() << "no error";
    }
    catch (const hot::TaskReloadCancelled& e)
    {
        EXPECT_STREQ(e.what(), "task cancelled by reload: AsyncRunner::Gated");
    }

    // The new generation: Twice gives x * 3, Gated adds 100.
    EXPECT_EQ(hot::sync_wait(call(h, "AsyncRunner::Twice", 2)), 6);
    hot::Task<int32_t> g = call(h, "AsyncRunner::Gated", 1);
    g.resume();
    ASSERT_TRUE(gate().open(5));
    ASSERT_TRUE(g.done());
    EXPECT_EQ(g.result(), 106);
    EXPECT_EQ(steps(h), 3);
}

// The parent and the child wait (the child at the gate). Both are stale after the reload.
TEST_P(AsyncHost, ReloadCancelsTheChainOfTasks)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Parent", 3);
    t.resume();
    ASSERT_TRUE(gate().waiting());
    ASSERT_NO_FATAL_FAILURE(reload(h, "async_host_v2.hot", 2, "async_host_v1"));

    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    ASSERT_TRUE(gate().open(1));
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 2); // the child and the parent
    ASSERT_TRUE(t.done());
    EXPECT_THROW((void)t.result(), hot::TaskReloadCancelled);
}

// A task that never started before the reload is cancelled at its start.
TEST_P(AsyncHost, NeverStartedTaskIsCancelledAtStart)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Gated", 1);
    ASSERT_NO_FATAL_FAILURE(reload(h, "async_host_v2.hot", 2, "async_host_v1"));

    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    t.resume();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
    ASSERT_TRUE(t.done());
    EXPECT_FALSE(gate().waiting()); // the old body did not start
    EXPECT_EQ(steps(h), 0);
    EXPECT_THROW((void)t.result(), hot::TaskReloadCancelled);
}

// A task pins its generation. The retired generation unloads only after the task objects die.
// Loader::reset fails while a task lives. AOT code does not unload, so the AOT mode skips.
TEST_P(AsyncHost, RetiredGenerationUnloadsAfterTheTaskDies)
{
    if (aot())
        GTEST_SKIP() << "AOT code does not unload";
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    {
        hot::Task<int32_t> done_task = call(h, "AsyncRunner::Twice", 2);
        done_task.resume();
        ASSERT_TRUE(done_task.done());
        hot::Task<int32_t> waiting = call(h, "AsyncRunner::Gated", 1);
        waiting.resume();
        ASSERT_TRUE(gate().waiting());
        EXPECT_EQ(hot::pin_count(1), 2u);

        ASSERT_NO_FATAL_FAILURE(reload(h, "async_host_v2.hot", 2, "async_host_v1"));
        {
            const hot::LoaderStats st = h.loader.stats();
            ASSERT_NE(find_gen(st, 1), nullptr);
            EXPECT_EQ(find_gen(st, 1)->state, hot::GenState::Retired);
            EXPECT_EQ(find_gen(st, 1)->pins, 2u);
        }

        delete h.actor;
        h.actor = nullptr;
        std::string error;
        EXPECT_FALSE(h.loader.reset(error));
        EXPECT_NE(error.find("pinned (a closure or a task is alive)"), std::string::npos) << error;

        EXPECT_EQ(done_task.result(), 4);
        gate().drop(); // the gate must not hold the frame when the task dies
    }
    EXPECT_EQ(hot::pin_count(1), 0u);
    h.loader.frame_boundary();
    {
        const hot::LoaderStats st = h.loader.stats();
        ASSERT_NE(find_gen(st, 1), nullptr);
        EXPECT_EQ(find_gen(st, 1)->state, hot::GenState::Unloaded);
        EXPECT_EQ(find_gen(st, 1)->pins, 0u);
    }
    std::string error;
    EXPECT_TRUE(h.loader.reset(error)) << error;
}

// A finally block around an await. Without a reload it runs once. After a reload the frame is
// stale: its finally block must not run when the frame dies, because the state block of
// generation 1 is gone. The first task dies at the gate. The second resumes after the reload
// and gets the cancel first.
TEST_P(AsyncHost, FinallyOfAStaleFrameDoesNotRun)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    auto finals = [&h] { return *hot::field_ptr<int>(h.inst.shell, "finals"); };
    {
        hot::Task<int32_t> t = call(h, "AsyncRunner::WithFinally", 1);
        t.resume();
        ASSERT_TRUE(gate().open(2));
        ASSERT_TRUE(t.done());
        EXPECT_EQ(t.result(), 3);
        EXPECT_EQ(finals(), 1);
    }
    auto at_gate = std::make_unique<hot::Task<int32_t>>(call(h, "AsyncRunner::WithFinally", 1));
    at_gate->resume();
    ASSERT_TRUE(gate().waiting());
    gate().drop();
    auto resumed = std::make_unique<hot::Task<int32_t>>(call(h, "AsyncRunner::WithFinally", 1));
    resumed->resume();
    ASSERT_TRUE(gate().waiting());

    ASSERT_NO_FATAL_FAILURE(reload(h, "async_host_v2.hot", 2, "async_host_v1"));
    EXPECT_EQ(finals(), 1);
    testing::internal::CaptureStderr();
    ASSERT_TRUE(gate().open(2)); // the guard cancels the frame; it stays until the owner destroys it
    testing::internal::GetCapturedStderr();
    ASSERT_TRUE(resumed->done());
    EXPECT_THROW((void)resumed->result(), hot::TaskReloadCancelled);
    resumed.reset();
    at_gate.reset();
    EXPECT_EQ(finals(), 1); // no stale finally block ran

    // The new generation runs its finally block.
    hot::Task<int32_t> t = call(h, "AsyncRunner::WithFinally", 1);
    t.resume();
    ASSERT_TRUE(gate().open(2));
    EXPECT_EQ(t.result(), 103);
    EXPECT_EQ(finals(), 2);
}

// A worker thread opens the gate. The frame resumes on the worker, and sync_wait on the main
// thread returns.
TEST_P(AsyncHost, WorkerThreadResumeCompletesSyncWait)
{
    if (!HOT_THREAD_SAFE)
        GTEST_SKIP() << "HOT_THREAD_SAFE is off";
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_host_v1.hot", "AsyncRunner"));
    hot::Task<int32_t> t = call(h, "AsyncRunner::Gated", 1);
    std::thread worker([]
    {
        while (!gate().waiting())
            std::this_thread::yield();
        gate().open(5);
    });
    const int v = hot::sync_wait(t);
    worker.join();
    EXPECT_EQ(v, 6);
    EXPECT_EQ(steps(h), 2);
}

// ---------------------------------------------------------------- the scheduler

// A script spawns a task. The host ticks the scheduler until the task ends.
TEST_P(AsyncHost, ScriptSpawnsATaskAndTheHostTicks)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_sched_v1.hot", "SchedRunner"));
    const hot::TaskHandle id = sync_call<hot::TaskHandle>(h, "SchedRunner::Start");
    EXPECT_NE(id.value(), 0u);
    EXPECT_TRUE(id.alive());
    EXPECT_EQ(hot::scheduler().size(), 1u);
    EXPECT_EQ(ticks(h), 0); // spawn does not run the body
    for (int i = 0; i < 4; ++i)
    {
        hot::scheduler().tick();
        h.loader.frame_boundary();
    }
    EXPECT_EQ(ticks(h), 3);
    EXPECT_EQ(hot::scheduler().size(), 0u);
    EXPECT_FALSE(id.alive());
}

// t.step() runs the task to its first suspension. The task sleeps, so it is not done.
TEST_P(AsyncHost, ScriptStepReturnsFalseAtTheFirstSuspension)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_sched_v1.hot", "SchedRunner"));
    EXPECT_FALSE(sync_call<bool>(h, "SchedRunner::Once", int32_t(4)));
    EXPECT_EQ(hot::scheduler().size(), 0u);
}

// t.run_until_done waits for the sleep of the task.
TEST_P(AsyncHost, ScriptRunUntilDoneReturnsTrue)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_sched_v1.hot", "SchedRunner"));
    EXPECT_TRUE(sync_call<bool>(h, "SchedRunner::Finish", int32_t(4)));
    EXPECT_EQ(hot::scheduler().size(), 0u);
}

// The error of a spawned task has no awaiter: it goes to the unhandled-error handler.
TEST_P(AsyncHost, SpawnedTaskErrorReachesTheHandler)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_sched_v1.hot", "SchedRunner"));
    std::vector<std::string> reports;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport& r) { reports.push_back(r.message); });
    sync_call<hot::TaskHandle>(h, "SchedRunner::SpawnBoom");
    for (int i = 0; i < 3; ++i)
        hot::scheduler().tick();
    hot::set_script_error_handler(nullptr);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0], "spawned boom");
    EXPECT_EQ(hot::scheduler().size(), 0u);
}

// h.cancel() destroys the task before it ends.
TEST_P(AsyncHost, ScriptCancelsASpawnedTask)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_sched_v1.hot", "SchedRunner"));
    const hot::TaskHandle id = sync_call<hot::TaskHandle>(h, "SchedRunner::StartLong");
    hot::scheduler().tick();
    hot::scheduler().tick();
    EXPECT_EQ(ticks(h), 2);
    EXPECT_TRUE(id.alive());
    EXPECT_TRUE(sync_call<bool>(h, "SchedRunner::IsAlive", id));
    sync_call<void>(h, "SchedRunner::Stop", id);
    EXPECT_FALSE(id.alive());
    EXPECT_FALSE(sync_call<bool>(h, "SchedRunner::IsAlive", id));
    EXPECT_EQ(hot::scheduler().size(), 0u);
    hot::scheduler().tick();
    EXPECT_EQ(ticks(h), 2);
    EXPECT_EQ(hot::pin_count(1), 0u);
}

// A spawned task pins generation 1. A reload makes it stale. The next tick cancels it: the old
// code does not run, and nobody gets a report. After cancel_all the generation unloads.
// AOT code does not unload, so the AOT mode skips the last part.
TEST_P(AsyncHost, ReloadCancelsASpawnedTaskAtTheNextTick)
{
    Host h;
    ASSERT_NO_FATAL_FAILURE(start(h, "async_sched_v1.hot", "SchedRunner"));
    std::vector<std::string> reports;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport& r) { reports.push_back(r.message); });
    sync_call<hot::TaskHandle>(h, "SchedRunner::StartLong");
    hot::scheduler().tick();
    EXPECT_EQ(ticks(h), 1);
    EXPECT_EQ(hot::pin_count(1), 1u);

    ASSERT_NO_FATAL_FAILURE(reload(h, "async_sched_v2.hot", 2, "async_sched_v1"));
    EXPECT_EQ(hot::scheduler().size(), 1u);
    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    hot::scheduler().tick();
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1);
    EXPECT_EQ(ticks(h), 1); // the old body did not run again
    EXPECT_EQ(hot::scheduler().size(), 0u);
    hot::set_script_error_handler(nullptr);
    EXPECT_TRUE(reports.empty());

    hot::scheduler().cancel_all();
    EXPECT_EQ(hot::pin_count(1), 0u);
    h.loader.frame_boundary();
    if (aot())
        return;
    const hot::LoaderStats st = h.loader.stats();
    ASSERT_NE(find_gen(st, 1), nullptr);
    EXPECT_EQ(find_gen(st, 1)->state, hot::GenState::Unloaded);
}

std::string mode_name(const ::testing::TestParamInfo<Mode>& info)
{
    switch (info.param)
    {
    case Mode::Files:
        return "Files";
    case Mode::Jit:
        return "Jit";
    case Mode::Aot:
        return "Aot";
    }
    return "Unknown";
}

INSTANTIATE_TEST_SUITE_P(Backends, AsyncHost, ::testing::Values(Mode::Files, Mode::Jit, Mode::Aot), mode_name);

} // namespace
