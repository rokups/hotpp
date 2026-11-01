// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The script traceback. With HOT_TRACEBACK=1 each script member function records a frame on a
// thread-local stack. A hot::ScriptError copies the stack as text when it is made (errors.h).
// The unwind pops the frames, so the copy must happen at the throw point.
#pragma once
#include "hot/config.h"
#include "hot/runtime/hidden.h"

#include <cstdint>
#include <string>

namespace hot
{
namespace detail
{

// One script call. "name" is "Class.Method". "name" and "file" point to string literals.
struct TraceFrame
{
    const char* name;
    const char* file;
    uint32_t line; // the script line of the statement that runs
};

// The frames of the calling thread. The array is a ring: when "depth" is more than kTraceMax,
// the array holds the innermost kTraceMax frames. The stack is in the host module, so a script
// module of any generation shares it.
constexpr uint32_t kTraceMax = 64;
struct TraceStack
{
    TraceFrame frames[kTraceMax];
    uint32_t depth = 0;
};

HOT_EXPORT TraceStack& trace_stack() noexcept;

// The text of the frames of the calling thread, innermost first. One frame in each line:
// "  at Hero.Update (hero.hot:12)". It is empty when the stack is empty.
HOT_EXPORT std::string trace_capture();

// The frame of one call. The constructor pushes and the destructor pops.
class TraceGuard
{
public:
    HOT_HIDDEN TraceGuard(const char* name, const char* file) noexcept
        : stack_(&trace_stack())
    {
        frame_ = &stack_->frames[stack_->depth % kTraceMax];
        // The slot of a frame that is more than kTraceMax levels out holds a live outer frame.
        // The guard keeps that frame and puts it back at the pop.
        restore_ = stack_->depth >= kTraceMax;
        if (restore_)
            saved_ = *frame_;
        frame_->name = name;
        frame_->file = file;
        frame_->line = 0;
        ++stack_->depth;
    }
    HOT_HIDDEN ~TraceGuard()
    {
        --stack_->depth;
        if (restore_)
            *frame_ = saved_;
    }
    TraceGuard(const TraceGuard&) = delete;
    TraceGuard& operator=(const TraceGuard&) = delete;
    HOT_HIDDEN void line(uint32_t l) noexcept { frame_->line = l; }

private:
    TraceStack* stack_;
    TraceFrame* frame_;
    TraceFrame saved_;
    bool restore_;
};

} // namespace detail
} // namespace hot

// The translator writes HOT_TRACE_FRAME at the start of a script member function, and
// HOT_TRACE_LINE before a statement. __LINE__ is the script line, because the translator writes
// "#line" for the script. Both macros are empty when HOT_TRACEBACK is 0.
#if HOT_TRACEBACK
    #define HOT_TRACE_FRAME(name, file) ::hot::detail::TraceGuard __hot_trace(name, file)
    #define HOT_TRACE_LINE() __hot_trace.line(static_cast<uint32_t>(__LINE__))
#else
    #define HOT_TRACE_FRAME(name, file)
    #define HOT_TRACE_LINE()
#endif
