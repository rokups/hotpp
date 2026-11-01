// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A host awaitable for the async integration tests: a manual gate.
// A script frame waits at "await asynctest.wait_gate()". The host opens the gate with a value.
// Then the frame resumes on the thread that opens the gate. The test program defines gate().
#pragma once

#include "hot/runtime/task.h"

#include <coroutine>
#include <mutex>

namespace asynctest
{

// One frame can wait at the gate. The host resumes it with hot::detail::resume_frame, so the
// generation guard of the task runs.
struct Gate
{
    std::mutex m;
    hot::detail::TaskPromiseBase* promise = nullptr;
    std::coroutine_handle<> handle;
    int value = 0;
    int waits = 0; // the number of suspensions at the gate

    bool waiting()
    {
        std::lock_guard<std::mutex> lock(m);
        return static_cast<bool>(handle);
    }

    // Resumes the waiting frame. The value is the result of the await. Returns false when no
    // frame waits.
    bool open(int v)
    {
        hot::detail::TaskPromiseBase* p = nullptr;
        std::coroutine_handle<> h;
        {
            std::lock_guard<std::mutex> lock(m);
            if (!handle)
                return false;
            p = promise;
            h = handle;
            promise = nullptr;
            handle = {};
            value = v;
        }
        hot::detail::resume_frame(*p, h);
        return true;
    }

    // Forgets the waiting frame without a resume. Call it before the owner destroys the task.
    void drop()
    {
        std::lock_guard<std::mutex> lock(m);
        promise = nullptr;
        handle = {};
    }
};

Gate& gate();

// The awaiter of the gate. Only a task frame can wait at the gate.
struct GateAwait
{
    Gate* g;

    bool await_ready() const noexcept { return false; }
    template <class P> void await_suspend(std::coroutine_handle<P> h)
    {
        std::lock_guard<std::mutex> lock(g->m);
        g->promise = &h.promise();
        g->handle = h;
        ++g->waits;
    }
    int await_resume() const noexcept { return g->value; }
};

inline GateAwait wait_gate() { return GateAwait{&gate()}; }

} // namespace asynctest
