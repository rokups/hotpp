// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The exceptions of a script and the report of an unhandled exception (hot/runtime/errors.h).
#include "hot/runtime/errors.h"

#include "hot/runtime/records.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/trace.h"

#include <cstdio>
#include <exception>
#include <mutex>
#include <typeinfo>
#include <utility>

namespace hot
{

void abstract_called(const char* cls, const char* fn)
{
    throw ScriptError(std::string("abstract method '") + cls + "." + fn + "' called");
}

#if HOT_TRACEBACK
ScriptError::ScriptError(const std::string& what) : std::runtime_error(what), traceback_(detail::trace_capture()) {}
ScriptError::ScriptError(const char* what) : std::runtime_error(what), traceback_(detail::trace_capture()) {}
#else
ScriptError::ScriptError(const std::string& what) : std::runtime_error(what) {}
ScriptError::ScriptError(const char* what) : std::runtime_error(what) {}
#endif
ScriptError::~ScriptError() = default;
NullReference::~NullReference() = default;
ArithmeticError::~ArithmeticError() = default;
TaskCancelled::~TaskCancelled() = default;
TaskReloadCancelled::~TaskReloadCancelled() = default;

IndexOutOfRange::IndexOutOfRange(std::size_t index, std::size_t size)
    : ScriptError("index " + std::to_string(index) + " is out of range for an array of size " +
                  std::to_string(size)),
      index_(index),
      size_(size)
{
}
IndexOutOfRange::~IndexOutOfRange() = default;

namespace
{

std::mutex& handler_mutex()
{
    static std::mutex m;
    return m;
}
ScriptErrorHandler& handler_slot()
{
    static ScriptErrorHandler h;
    return h;
}

} // namespace

void set_script_error_handler(ScriptErrorHandler handler)
{
    std::lock_guard<std::mutex> lock(handler_mutex());
    handler_slot() = std::move(handler);
}

ScriptErrorHandler script_error_handler()
{
    std::lock_guard<std::mutex> lock(handler_mutex());
    return handler_slot();
}

namespace detail
{

#if HOT_ELF_TLS_DEPTH
__thread uint32_t g_call_depth = 0;
uint32_t g_max_call_depth = HOT_MAX_CALL_DEPTH;

intptr_t call_depth_tp_offset() noexcept
{
    return reinterpret_cast<char*>(&g_call_depth) - static_cast<char*>(__builtin_thread_pointer());
}
#else
uint32_t& call_depth_slot() noexcept
{
    static thread_local uint32_t depth = 0;
    return depth;
}

uint32_t g_max_call_depth = HOT_MAX_CALL_DEPTH;

uint32_t max_call_depth_slot() noexcept
{
    return g_max_call_depth;
}
#endif

static_assert(HOT_MAX_CALL_DEPTH <= kMaxCallDepthLimit, "HOT_MAX_CALL_DEPTH must be 65519 or less");

void frame_overflow(uint32_t frames)
{
    if (std::uncaught_exceptions() > 0 && frames < max_call_depth_value() + kUnwindHeadroom)
        return;
    throw ScriptError("stack overflow");
}

void throw_null_reference(const char* what)
{
    throw NullReference(what);
}

void throw_index_out_of_range(std::size_t index, std::size_t size)
{
    throw IndexOutOfRange(index, size);
}

void report_unhandled(const ShellCore* self, std::exception_ptr ep)
{
    if (!ep)
        return;
    const char* type = "unknown";
    std::string message, traceback;
    try
    {
        std::rethrow_exception(ep);
    }
    catch (const ScriptError& e)
    {
        if (dynamic_cast<const NullReference*>(&e))
            type = "hot::NullReference";
        else if (dynamic_cast<const IndexOutOfRange*>(&e))
            type = "hot::IndexOutOfRange";
        else if (dynamic_cast<const ArithmeticError*>(&e))
            type = "hot::ArithmeticError";
        else
            type = "hot::ScriptError";
        message = e.what();
        traceback = e.traceback();
    }
    catch (const std::exception& e)
    {
        type = typeid(e).name();
        message = e.what();
    }
    catch (...)
    {
        message = "unknown exception";
    }
    const char* class_name = (self && self->cls && self->cls->name) ? self->cls->name : "?";
    ScriptErrorReport report{type, message.c_str(), self, class_name, traceback.c_str()};
    ScriptErrorHandler handler = script_error_handler();
    if (handler)
        handler(report);
    else
    {
        std::fprintf(stderr, "[script] ERROR: unhandled %s in %s: %s\n", type, class_name, message.c_str());
        if (!traceback.empty())
            std::fputs(traceback.c_str(), stderr);
    }
}

} // namespace detail

void set_max_call_depth(uint32_t limit) noexcept
{
    detail::g_max_call_depth = limit > detail::kMaxCallDepthLimit ? detail::kMaxCallDepthLimit : limit;
}

uint32_t max_call_depth() noexcept
{
    return detail::g_max_call_depth;
}

} // namespace hot
