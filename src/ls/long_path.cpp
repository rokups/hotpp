// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "long_path.h"

#ifdef _WIN32
    #include <filesystem>

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace hot::ls
{

#ifdef _WIN32

namespace
{
// GetLongPathNameW expands each part of an existing path. It fails for a missing path.
bool expand(const std::wstring& in, std::wstring& out)
{
    const DWORD n = GetLongPathNameW(in.c_str(), nullptr, 0);
    if (n == 0)
        return false;
    out.resize(n);
    const DWORD m = GetLongPathNameW(in.c_str(), out.data(), n);
    if (m == 0 || m >= n)
        return false;
    out.resize(m);
    return true;
}
} // namespace

std::string long_path(const std::string& path)
{
    if (path.find('~') == std::string::npos)
        return path; // a short name always has "~"
    const std::filesystem::path p = utf8_path(path);
    std::wstring out;
    if (expand(p.wstring(), out))
        return path_utf8(std::filesystem::path(out));
    // A file that does not exist yet: expand the folder, then add the name.
    if (p.has_parent_path() && p.has_filename() && expand(p.parent_path().wstring(), out))
        return path_utf8(std::filesystem::path(out) / p.filename());
    return path;
}

#else

std::string long_path(const std::string& path)
{
#ifndef __APPLE__
    return path; // Linux keeps the path of the client: a link must not change the document URI
#else
    if (path.empty() || path[0] != '/')
        return path; // only an absolute path has a real path
    // The part of the path that exists has no symbolic links: macOS "/var" is "/private/var".
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::weakly_canonical(utf8_path(path), ec);
    return ec || p.empty() ? path : path_utf8(p);
#endif
}

#endif

} // namespace hot::ls
