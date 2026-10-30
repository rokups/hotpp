// Build-time configuration of the S++ runtime.
//
// CMake sets SPP_USE_FMT on the runtime library. The value is 1 when string interpolation
// "${...}" uses fmt, and 0 when it uses std::format. The build chooses the value from the C++
// standard and from the CMake option SPP_USE_FMT. No runtime option exists.
#pragma once

#include <string>
#include <vector>

#ifndef SPP_USE_FMT
    #define SPP_USE_FMT 0
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
