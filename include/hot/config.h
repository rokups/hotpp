// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Build-time configuration of the Hot++ runtime.
//
// CMake sets HOT_USE_FMT on the runtime library. The value is 1 when string interpolation
// "${...}" uses fmt, and 0 when it uses std::format. The build chooses the value from the C++
// standard and from the CMake option HOT_USE_FMT. No runtime option exists.
//
// CMake also sets HOT_CXX_STANDARD on the runtime library: 17, 20 or 23. This is the C++
// standard of the host. The backends compile the generated code with this standard. The
// translator parses the native headers of the imports with this standard. The generated code
// and the headers in include/hot need C++17 only.
#pragma once

#include <string>
#include <vector>

#ifndef HOT_USE_FMT
    #define HOT_USE_FMT 0
#endif
// The C++ standard of the host: 17, 20 or 23. Without the CMake value, the macro uses the
// standard of the compiler that reads this header.
#ifndef HOT_CXX_STANDARD
    #if __cplusplus >= 202302L
        #define HOT_CXX_STANDARD 23
    #elif __cplusplus >= 202002L
        #define HOT_CXX_STANDARD 20
    #else
        #define HOT_CXX_STANDARD 17
    #endif
#endif
// The runtime checks of a script: the null test of a pointer and a handle, and the bounds test
// of an array. The value 0 removes them from the generated code. The default is 1.
#ifndef HOT_RUNTIME_CHECKS
    #define HOT_RUNTIME_CHECKS 1
#endif
// The thread safety of the runtime. The value 1 means that any thread can create and destroy
// script objects: the list of live objects has a small lock. The value 0 removes the lock, and
// only one thread at a time creates or destroys script objects. The default is 1. The host and the
// runtime library must use the same value. The constructor of the Loader checks this.
#ifndef HOT_THREAD_SAFE
    #define HOT_THREAD_SAFE 1
#endif
// The script traceback (hot/runtime/trace.h). The value 1 makes each script function record a
// frame, and each script exception keep the call stack of its throw point. The default is 0.
#ifndef HOT_TRACEBACK
    #define HOT_TRACEBACK 0
#endif
// The view type of "T[]". CMake sets HOT_VIEW_STD=1 for the option HOT_VIEW_TYPE=std. Without it,
// the view type is hot::span. std::span needs C++20. The value is one constant for the build.
#if defined(HOT_VIEW_STD) && __cplusplus < 202002L && !(defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    #error "HOT_VIEW_STD needs C++20 (std::span). Use HOT_VIEW_TYPE=hot or a C++20 compiler mode."
#endif
// The include directory of fmt, or an empty text when the compiler finds fmt by itself.
#ifndef HOT_FMT_INCLUDE_DIR
    #define HOT_FMT_INCLUDE_DIR ""
#endif

namespace hot
{

// The value for hotc::TranslateOptions::format_lib: "fmt" or "std".
constexpr const char* format_lib_name()
{
    return HOT_USE_FMT ? "fmt" : "std";
}

// The view type of "T[]": "hot" (hot::span) or "std" (std::span).
constexpr const char* view_type_name()
{
#ifdef HOT_VIEW_STD
    return "std";
#else
    return "hot";
#endif
}

// The standard name for the option -std= of the compiler: "c++17", "c++20" or "c++2b".
constexpr const char* cxx_std_name()
{
    return HOT_CXX_STANDARD >= 23 ? "c++2b" : HOT_CXX_STANDARD >= 20 ? "c++20" : "c++17";
}

// The flags that the compile command of a script needs for the format library.
// The list is empty for std::format.
inline std::vector<std::string> format_compile_flags()
{
    std::vector<std::string> flags;
    if (HOT_USE_FMT)
    {
        flags.push_back("-DFMT_HEADER_ONLY");
        if (HOT_FMT_INCLUDE_DIR[0] != '\0')
            flags.push_back(std::string("-I") + HOT_FMT_INCLUDE_DIR);
    }
    return flags;
}

// The flags that the compile command of a script needs for the runtime checks.
// The list is empty when the checks are on.
inline std::vector<std::string> checks_compile_flags()
{
    std::vector<std::string> flags;
    if (!HOT_RUNTIME_CHECKS)
        flags.push_back("-DHOT_RUNTIME_CHECKS=0");
    return flags;
}

// The flags that the compile command of a script needs for the thread safety.
// The list is empty when the thread safety is on.
inline std::vector<std::string> thread_safe_compile_flags()
{
    std::vector<std::string> flags;
    if (!HOT_THREAD_SAFE)
        flags.push_back("-DHOT_THREAD_SAFE=0");
    return flags;
}

// The flags that the compile command of a script needs for the traceback.
// The list is empty when the traceback is off.
inline std::vector<std::string> traceback_compile_flags()
{
    std::vector<std::string> flags;
    if (HOT_TRACEBACK)
        flags.push_back("-DHOT_TRACEBACK=1");
    return flags;
}

} // namespace hot
