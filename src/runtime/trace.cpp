// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The thread-local frame stack of the script traceback (hot/runtime/trace.h).
#include "hot/runtime/trace.h"

namespace hot::detail
{

namespace
{

// The big stack lives on the heap. The thread-local data is one pointer. A plugin that the host
// opens with dlopen can then use the initial-exec TLS model: the static TLS block has no room
// for a big object.
thread_local TraceStack* t_stack = nullptr;

// Used by frames that run after the thread-local owner is destroyed at the thread exit.
TraceStack g_late_stack;

struct StackOwner
{
    TraceStack* stack = nullptr;
    ~StackOwner()
    {
        t_stack = &g_late_stack;
        delete stack;
    }
};

TraceStack& trace_stack_slow() noexcept
{
    thread_local StackOwner owner;
    owner.stack = new TraceStack;
    t_stack = owner.stack;
    return *t_stack;
}

} // namespace

TraceStack& trace_stack() noexcept
{
    TraceStack* s = t_stack;
    return s ? *s : trace_stack_slow();
}

std::string trace_capture()
{
    const TraceStack& s = trace_stack();
    const uint32_t shown = s.depth < kTraceMax ? s.depth : kTraceMax;
    std::string text;
    for (uint32_t i = 0; i < shown; ++i)
    {
        const TraceFrame& f = s.frames[(s.depth - 1 - i) % kTraceMax];
        text += "  at ";
        text += f.name ? f.name : "?";
        text += " (";
        text += f.file ? f.file : "?";
        text += ':';
        text += std::to_string(f.line);
        text += ")\n";
    }
    if (s.depth > shown)
        text += "  ... " + std::to_string(s.depth - shown) + " more\n";
    return text;
}

} // namespace hot::detail
