// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The order of the loaded modules for the symbol search of JIT code on Windows. The functions
// have no Windows calls, so the tests check them on each platform.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hot::jit
{

namespace detail
{

inline char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The file name of "path" in lower case: the text after the last '\' or '/'.
inline std::string lower_file_name(std::string_view path)
{
    const std::size_t slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    std::string out(path);
    for (char& c : out)
        c = lower_ascii(c);
    return out;
}

inline bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

} // namespace detail

// True when "path" names a DLL of a C or C++ runtime: ucrtbase, vcruntime, msvcp, msvcr, msvcrt,
// concrt, crtdll or an API set of the C runtime (api-ms-win-crt-*). Each of these DLLs has its own
// state: a FILE, a heap, a locale. Only the runtime DLLs of the host can give JIT code a name.
inline bool is_c_runtime_dll(std::string_view path)
{
    const std::string name = detail::lower_file_name(path);
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dll") != 0)
        return false;
    for (std::string_view prefix : {"ucrtbase", "vcruntime", "msvcp", "msvcr", "concrt", "crtdll", "api-ms-win-crt-"})
        if (detail::starts_with(name, prefix))
            return true;
    return false;
}

// The rank of a host runtime DLL in the search: vcruntime (0), msvcp and concrt (1), ucrtbase
// (2), other (3). ucrtbase also exports the C++ exception names of vcruntime (__CxxFrameHandler3,
// __current_exception, __processing_throw and more), with its own exception state for each
// thread. The host code uses the state of vcruntime: for example std::current_exception() of
// msvcp reads it. A JIT catch with the frame handler of ucrtbase writes the other state, and then
// std::current_exception() gives null. So vcruntime comes before ucrtbase.
inline int host_runtime_rank(std::string_view path)
{
    const std::string name = detail::lower_file_name(path);
    if (detail::starts_with(name, "vcruntime"))
        return 0;
    if (detail::starts_with(name, "msvcp") || detail::starts_with(name, "concrt"))
        return 1;
    if (detail::starts_with(name, "ucrtbase"))
        return 2;
    return 3;
}

// The search order of the loaded modules. "modules" has the paths of the modules in the load
// order. "host" has the indexes of the runtime DLLs of the host. The result starts with the host
// indexes (each one time), in the order of host_runtime_rank. Host indexes with the same rank keep
// the order of "host". The other modules follow in the load order. A C runtime DLL that is not a
// host DLL is not in the result.
inline std::vector<std::size_t> symbol_search_order(
    const std::vector<std::string>& modules, const std::vector<std::size_t>& host)
{
    std::vector<std::size_t> order;
    std::vector<bool> used(modules.size(), false);
    for (int rank = 0; rank <= 3; ++rank)
        for (std::size_t i : host)
            if (i < modules.size() && !used[i] && host_runtime_rank(modules[i]) == rank)
            {
                used[i] = true;
                order.push_back(i);
            }
    for (std::size_t i = 0; i < modules.size(); ++i)
        if (!used[i] && !is_c_runtime_dll(modules[i]))
            order.push_back(i);
    return order;
}

} // namespace hot::jit
