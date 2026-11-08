// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The suspend trampolines of task frames (include/hot/runtime/task.h). A trampoline is a
// coroutine, so this file is C++20 also in a C++17 host (src/runtime/CMakeLists.txt). The runtime
// has the one copy: a module can unload, so the code of a trampoline must not be in a module.

#include "hot/runtime/export.h"

#include <atomic>
#include <coroutine>
#include <exception>

namespace hot
{
namespace detail
{
// The same declarations as in include/hot/runtime/task.h.
using SuspendWork = std::coroutine_handle<> (*)(void* ctx, std::coroutine_handle<> frame) noexcept;
HOT_EXPORT std::atomic<void (*)(void*)>& suspend_probe() noexcept;

namespace
{
struct Pool;

// A coroutine that loops: each resume runs one work item, then it transfers to the coroutine
// that the work returns. A work item can suspend another frame on the same thread: that frame
// gets another trampoline, because this one is busy until its work returns.
struct Trampoline
{
    struct Loop
    {
        struct promise_type
        {
            Loop get_return_object() noexcept { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
            std::suspend_always initial_suspend() const noexcept { return {}; }
            std::suspend_always final_suspend() const noexcept { return {}; }
            void return_void() const noexcept {}
            void unhandled_exception() const noexcept { std::terminate(); }
        };
        std::coroutine_handle<promise_type> h;
    };
    struct Step
    {
        Trampoline* t;
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept;
        void await_resume() const noexcept {}
    };
    static Loop run(Trampoline* t)
    {
        for (;;)
            co_await Step{t};
    }

    SuspendWork work = nullptr;
    void* ctx = nullptr;
    std::coroutine_handle<> frame;
    Pool* pool;
    Trampoline* next_free = nullptr;
    std::coroutine_handle<> loop;
    explicit Trampoline(Pool* p) : pool(p), loop(run(this).h) {}
    Trampoline(const Trampoline&) = delete;
    Trampoline& operator=(const Trampoline&) = delete;
    ~Trampoline() { loop.destroy(); } // the thread ends: the loop is suspended
};

// The free trampolines of one thread. A busy trampoline is not in the list.
struct Pool
{
    Trampoline* free = nullptr;
    Pool() = default;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    ~Pool()
    {
        while (Trampoline* t = free)
        {
            free = t->next_free;
            delete t;
        }
    }
};

std::coroutine_handle<> Trampoline::Step::await_suspend(std::coroutine_handle<>) const noexcept
{
    // The frame of the work is suspended, and only this thread can use it until the work
    // publishes the suspension. After the publication the work does not touch that frame.
    Trampoline& s = *t;
    std::coroutine_handle<> next = s.work(s.ctx, s.frame);
    s.next_free = s.pool->free; // free again: nothing runs on this thread until the transfer
    s.pool->free = &s;
    // The returned handle goes into the frame of the trampoline. That frame lives.
    return next ? next : std::noop_coroutine();
}
} // namespace

HOT_EXPORT std::coroutine_handle<> after_suspend(SuspendWork work, void* ctx, std::coroutine_handle<> frame) noexcept
{
    thread_local Pool pool;
    Trampoline* t = pool.free;
    if (t)
        pool.free = t->next_free;
    else
        t = new Trampoline(&pool); // noexcept: no memory ends the process
    t->work = work;
    t->ctx = ctx;
    t->frame = frame;
    if (void (*probe)(void*) = suspend_probe().load(std::memory_order_acquire))
        probe(frame.address());
    return t->loop;
}

HOT_EXPORT std::atomic<void (*)(void*)>& suspend_probe() noexcept
{
    static std::atomic<void (*)(void*)> probe{nullptr};
    return probe;
}

// The C++17 part of the runtime (threads.cpp) refers to this function. Thus a host that does not
// use Task in its own code also links this file, and a script with async methods finds the export.
void* frame_reaper_link() noexcept
{
    return reinterpret_cast<void*>(&after_suspend);
}

} // namespace detail
} // namespace hot
