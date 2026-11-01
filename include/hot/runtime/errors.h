// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The exceptions of a script and the report of an unhandled exception.
// These types are NOT HOT_HIDDEN. The host and the script module must use the same RTTI,
// so a "catch" in the host finds an exception that the script throws.
#pragma once

#include "hot/runtime/hidden.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>

namespace hot
{

struct ShellCore;

// The base of the exceptions that the runtime checks throw.
HOT_EXPORT_STD_BEGIN // a std base and a std member (export.h)
class HOT_EXPORT ScriptError : public std::runtime_error
{
public:
    // With HOT_TRACEBACK=1 the constructor copies the script call stack (trace.h).
    explicit ScriptError(const std::string& what);
    explicit ScriptError(const char* what);
    ~ScriptError() override;

    // The script call stack of the throw point, one frame in each line, innermost first.
    // It is empty when the traceback is off.
    const std::string& traceback() const noexcept { return traceback_; }

private:
    std::string traceback_;
};
HOT_EXPORT_STD_END

// A null pointer or a null handle was dereferenced.
class HOT_EXPORT NullReference : public ScriptError
{
public:
    using ScriptError::ScriptError;
    ~NullReference() override;
};

// A task ended without a result because of a cancellation. User code can throw it. A task whose
// body ends with it counts as cancelled: report_unhandled does not see it. The awaiter of the
// task gets the exception with its dynamic type (task.h).
class HOT_EXPORT TaskCancelled : public ScriptError
{
public:
    using ScriptError::ScriptError;
    ~TaskCancelled() override;
};

// A reload cancelled the task: the code of the task is not live (message "task cancelled by
// reload: <what>").
class HOT_EXPORT TaskReloadCancelled : public TaskCancelled
{
public:
    using TaskCancelled::TaskCancelled;
    ~TaskReloadCancelled() override;
};

// An array index was negative or not less than the size of the array.
class HOT_EXPORT IndexOutOfRange : public ScriptError
{
public:
    IndexOutOfRange(std::size_t index, std::size_t size);
    ~IndexOutOfRange() override;
    std::size_t index() const noexcept { return index_; }
    std::size_t size() const noexcept { return size_; }

private:
    std::size_t index_;
    std::size_t size_;
};

// The data of an unhandled exception. The pointers are valid only during the handler call.
struct ScriptErrorReport
{
    const char* type;       // for example "hot::NullReference"
    const char* message;    // the text of what()
    const ShellCore* self;  // the object that ran the entry point, or null
    const char* class_name; // the class of "self", or "?"
    const char* traceback;  // the script call stack (trace.h); empty when HOT_TRACEBACK is 0
};

using ScriptErrorHandler = std::function<void(const ScriptErrorReport&)>;

// Sets the handler of an unhandled exception. An empty function selects the default:
// one line on stderr.
HOT_EXPORT void set_script_error_handler(ScriptErrorHandler handler);
HOT_EXPORT ScriptErrorHandler script_error_handler();

namespace detail
{

// The number of hot::call frames on the calling thread (shell.h). The report of an error is
// only at the outermost frame. The thread-local count is in the host runtime, so a script module
// of any generation shares it.
#if defined(__ELF__) && (defined(__clang__) || defined(__GNUC__))
    // ELF (Linux): the count is in static TLS. A script module reads it with the initial-exec
    // model: one instruction, no call. The host runtime must be in the executable, in a library
    // that loads at the program start, or in a library that dlopen loads into the surplus of the
    // static TLS (glibc.rtld.optional_static_tls, see doc/embedding-rules.md).
    #define HOT_ELF_TLS_DEPTH 1
extern __thread uint32_t g_call_depth __attribute__((visibility("default"), tls_model("initial-exec")));

// The offset of g_call_depth from the thread pointer. The offset of a static TLS variable is
// the same in each thread.
HOT_EXPORT intptr_t call_depth_tp_offset() noexcept;

    #ifdef HOT_JIT_TLS_OFFSET
// The JIT links its code without TLS relocations. The jit backend defines this absolute symbol:
// its address is call_depth_tp_offset(). The compiler makes the same instruction as for
// the initial-exec model: incl %fs:(offset).
extern "C" char hot_call_depth_tp_offset[];
    #endif
#else
    // Other targets: the count is a native thread_local variable of the runtime. A module
    // reads it through an exported function, because a module cannot import TLS data on each
    // target.
    #define HOT_ELF_TLS_DEPTH 0
HOT_EXPORT uint32_t& call_depth_slot() noexcept;
#endif

// The count of the calling thread.
HOT_HIDDEN uint32_t& call_depth() noexcept
{
#if HOT_ELF_TLS_DEPTH && defined(HOT_JIT_TLS_OFFSET)
    return *reinterpret_cast<uint32_t*>(static_cast<char*>(__builtin_thread_pointer()) +
                                        reinterpret_cast<intptr_t>(hot_call_depth_tp_offset));
#elif HOT_ELF_TLS_DEPTH
    return g_call_depth;
#else
    return call_depth_slot();
#endif
}

[[noreturn]] HOT_EXPORT void throw_null_reference(const char* what);
[[noreturn]] HOT_EXPORT void throw_index_out_of_range(std::size_t index, std::size_t size);

// Reports the exception "ep" that no script code handled. "self" can be null.
HOT_EXPORT void report_unhandled(const ShellCore* self, std::exception_ptr ep);

} // namespace detail

} // namespace hot
