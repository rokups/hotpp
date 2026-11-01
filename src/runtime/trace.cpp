// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The thread-local frame stack of the script traceback (hot/runtime/trace.h).
#include "hot/runtime/trace.h"

namespace hot::detail
{

TraceStack& trace_stack() noexcept
{
    static thread_local TraceStack stack;
    return stack;
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
