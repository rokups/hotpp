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

} // namespace hot
