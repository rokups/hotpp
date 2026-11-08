// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The reaper of task frames (include/hot/runtime/task.h). It is a coroutine, so this file is
// C++20 also in a C++17 host (src/runtime/CMakeLists.txt). The runtime has the one copy: a
// module can unload, so the code of the reaper must not be in a module.

#include "hot/runtime/export.h"

#include <coroutine>
#include <exception>
#include <utility>

namespace hot
{
namespace detail
{
namespace
{
// The coroutine of end_frame_after_suspend. It loops: each resume destroys one frame, then it
// transfers to the next coroutine. It is suspended when another frame uses it, so it can run again.
struct FrameReaper
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
        FrameReaper* r;
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept
        {
            // The returned handle goes into the frame of the reaper. That frame lives.
            std::coroutine_handle<> dead = std::exchange(r->dead, {});
            std::coroutine_handle<> next = std::exchange(r->next, {});
            dead.destroy();
            return next ? next : std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };
    static Loop run(FrameReaper* r)
    {
        for (;;)
            co_await Step{r};
    }

    std::coroutine_handle<> dead;
    std::coroutine_handle<> next;
    std::coroutine_handle<> loop;
    FrameReaper() : loop(run(this).h) {}
    FrameReaper(const FrameReaper&) = delete;
    FrameReaper& operator=(const FrameReaper&) = delete;
    ~FrameReaper() { loop.destroy(); } // the thread ends: the loop is suspended
};
} // namespace

HOT_EXPORT std::coroutine_handle<> end_frame_after_suspend(std::coroutine_handle<> dead, std::coroutine_handle<> next) noexcept
{
    // A frame that ends on this thread runs on this thread: the reaper is free now.
    thread_local FrameReaper reaper;
    reaper.dead = dead;
    reaper.next = next;
    return reaper.loop;
}

// The C++17 part of the runtime (threads.cpp) refers to this function. Thus a host that does not
// use Task in its own code also links this file, and a script with async methods finds the export.
void* frame_reaper_link() noexcept
{
    return reinterpret_cast<void*>(&end_frame_after_suspend);
}

} // namespace detail
} // namespace hot
