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

struct ShellBase;

// The base of the exceptions that the runtime checks throw.
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

// A null pointer or a null handle was dereferenced.
class HOT_EXPORT NullReference : public ScriptError
{
public:
    using ScriptError::ScriptError;
    ~NullReference() override;
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
    const ShellBase* self;  // the object that ran the entry point, or null
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
// only at the outermost frame. The thread-local count is in the host module, so a script module
// of any generation shares it.
HOT_EXPORT uint32_t& call_depth() noexcept;

[[noreturn]] HOT_EXPORT void throw_null_reference(const char* what);
[[noreturn]] HOT_EXPORT void throw_index_out_of_range(std::size_t index, std::size_t size);

// Reports the exception "ep" that no script code handled. "self" can be null.
HOT_EXPORT void report_unhandled(const ShellBase* self, std::exception_ptr ep);

} // namespace detail

} // namespace hot
