// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "file_time.h"

#ifdef _WIN32
    #include <filesystem>
    #include <system_error>

    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

namespace hotc
{

#ifdef _WIN32

FileTimes file_times(const std::string& path)
{
    FileTimes r;
    WIN32_FILE_ATTRIBUTE_DATA data;
    const std::wstring wide = std::filesystem::u8path(path).wstring();
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data))
        return r;
    r.exists = true;
    // A FILETIME counts 100 ns from 1601-01-01. The stamps count ns from 1970-01-01, the same
    // epoch as std::chrono::system_clock (the "recent" test of the cache compares with it).
    constexpr uint64_t kEpochDiff = 116444736000000000ull;
    const uint64_t ticks = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
        | data.ftLastWriteTime.dwLowDateTime;
    r.mtime_ns = ticks > kEpochDiff ? (ticks - kEpochDiff) * 100ull : 0;
    r.ctime_ns = r.mtime_ns;
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        r.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return r;
}

std::string clang_path(const std::string& path)
{
    const std::filesystem::path p = std::filesystem::u8path(path);
    if (path.size() > 1 && (path[0] == '/' || path[0] == '\\') && path[1] != '/' && path[1] != '\\')
    {
        std::error_code ec;
        const std::filesystem::path full = std::filesystem::absolute(p, ec);
        if (!ec)
            return full.generic_string();
    }
    return p.generic_string();
}

std::vector<std::string> clang_spellings(const std::string& path)
{
    const std::string g = clang_path(path);
    std::vector<std::string> out = {g};
    auto add = [&](std::string s)
    {
        for (const std::string& o : out)
            if (o == s)
                return;
        out.push_back(std::move(s));
    };
    // libclang adds "\\" after the include folder and after the folder of the includer. These
    // separators are near the end of the name, so each form of the last separators is added.
    // The folder before them has the "/" form (clang_path) or the "\\" form.
    std::vector<size_t> seps;
    for (size_t k = 0; k < g.size(); ++k)
        if (g[k] == '/')
            seps.push_back(k);
    const size_t m = seps.size() < 4 ? seps.size() : 4;
    for (int lead = 0; lead < 2; ++lead)
        for (unsigned mask = 0; mask < (1u << m); ++mask)
        {
            std::string s = g;
            for (size_t i = 0; i < seps.size(); ++i)
            {
                const size_t from_end = seps.size() - 1 - i;
                const bool native = from_end < m ? ((mask >> from_end) & 1u) != 0 : lead == 1;
                s[seps[i]] = native ? '\\' : '/';
            }
            add(s);
        }
    return out;
}

#else

std::string clang_path(const std::string& path)
{
    return path;
}

std::vector<std::string> clang_spellings(const std::string& path)
{
    return {path};
}

namespace
{
uint64_t ns_of(const struct timespec& ts)
{
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
} // namespace

FileTimes file_times(const std::string& path)
{
    FileTimes r;
    struct ::stat st;
    if (::stat(path.c_str(), &st) != 0)
        return r;
    r.exists = true;
    r.mtime_ns = ns_of(st.st_mtim);
    r.ctime_ns = ns_of(st.st_ctim);
    r.size = static_cast<uint64_t>(st.st_size);
    r.ino = static_cast<uint64_t>(st.st_ino);
    return r;
}

#endif

} // namespace hotc
