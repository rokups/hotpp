// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The canonical form of a Windows path. A Windows path can have short 8.3 names
// ("C:\Users\RUNNER~1"), and clangd writes the long names ("C:\Users\runneradmin"). hot-ls
// compares the paths of clangd with its own paths, so both use the long names.
#pragma once

#include <filesystem>
#include <string>

namespace hot::ls
{

// hot-ls keeps a path as UTF-8 text (the LSP URIs are UTF-8). On Windows "path(std::string)" and
// "path::string()" use the ANSI code page, so the server converts with these two functions. The
// form of "u8path" and "u8string" changes in C++20 (char8_t): the functions hide the difference.
inline std::filesystem::path utf8_path(const std::string& s)
{
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

inline std::string path_utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The path with long names in place of short names. The last part of the path can be absent:
// then the parent folder gets the long names. On other systems the path does not change. The
// input and the result are UTF-8.
std::string long_path(const std::string& path);

} // namespace hot::ls
