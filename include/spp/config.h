// Build-time configuration of the S++ runtime.
//
// CMake sets SPP_USE_FMT on the runtime library. The value is 1 when string interpolation
// "${...}" uses fmt, and 0 when it uses std::format. The build chooses the value from the C++
// standard and from the CMake option SPP_USE_FMT. No runtime option exists.
//
// CMake also sets SPP_CXX_STANDARD on the runtime library: 17, 20 or 23. This is the C++
// standard of the host. The backends compile the generated code with this standard. The
// translator parses the native headers of the imports with this standard. The generated code
// and the headers in include/spp need C++17 only.
#pragma once

#include <string>
#include <vector>

#ifndef SPP_USE_FMT
    #define SPP_USE_FMT 0
#endif
// The C++ standard of the host: 17, 20 or 23. Without the CMake value, the macro uses the
// standard of the compiler that reads this header.
#ifndef SPP_CXX_STANDARD
    #if __cplusplus >= 202302L
        #define SPP_CXX_STANDARD 23
    #elif __cplusplus >= 202002L
        #define SPP_CXX_STANDARD 20
    #else
        #define SPP_CXX_STANDARD 17
    #endif
#endif
// The include directory of fmt, or an empty text when the compiler finds fmt by itself.
#ifndef SPP_FMT_INCLUDE_DIR
    #define SPP_FMT_INCLUDE_DIR ""
#endif

namespace spp
{

// The value for sppc::TranslateOptions::format_lib: "fmt" or "std".
constexpr const char* format_lib_name()
{
    return SPP_USE_FMT ? "fmt" : "std";
}

// The standard name for the option -std= of the compiler: "c++17", "c++20" or "c++2b".
constexpr const char* cxx_std_name()
{
    return SPP_CXX_STANDARD >= 23 ? "c++2b" : SPP_CXX_STANDARD >= 20 ? "c++20" : "c++17";
}

// The flags that the compile command of a script needs for the format library.
// The list is empty for std::format.
inline std::vector<std::string> format_compile_flags()
{
    std::vector<std::string> flags;
    if (SPP_USE_FMT)
    {
        flags.push_back("-DFMT_HEADER_ONLY");
        if (SPP_FMT_INCLUDE_DIR[0] != '\0')
            flags.push_back(std::string("-I") + SPP_FMT_INCLUDE_DIR);
    }
    return flags;
}

} // namespace spp
