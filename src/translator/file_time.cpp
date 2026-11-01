// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "file_time.h"

#include <algorithm>
#include <filesystem>
#include <set>

#ifdef _WIN32
    #include <system_error>

    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

namespace hotc
{

#ifdef _WIN32

namespace
{
// A FILETIME counts 100 ns from 1601-01-01. The stamps count ns from 1970-01-01, the same
// epoch as std::chrono::system_clock (the "recent" test of the cache compares with it).
uint64_t ns_of_ticks(uint64_t ticks)
{
    constexpr uint64_t kEpochDiff = 116444736000000000ull;
    return ticks > kEpochDiff ? (ticks - kEpochDiff) * 100ull : 0;
}
} // namespace

FileTimes file_times(const std::string& path)
{
    FileTimes r;
    const std::wstring wide = std::filesystem::u8path(path).wstring();
    // NTFS has a change time and a file index, as the ctime and the inode of POSIX. An edit
    // that resets the last write time changes the change time. A replace gives a new index.
    HANDLE h = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        FILE_BASIC_INFO basic{};
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic))
            && GetFileInformationByHandle(h, &info);
        CloseHandle(h);
        if (ok)
        {
            r.exists = true;
            r.mtime_ns = ns_of_ticks(static_cast<uint64_t>(basic.LastWriteTime.QuadPart));
            r.ctime_ns = ns_of_ticks(static_cast<uint64_t>(basic.ChangeTime.QuadPart));
            r.ino = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                r.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            return r;
        }
    }
    // No handle (for example no access): the attributes only. The change time is the write time.
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data))
        return r;
    r.exists = true;
    r.mtime_ns = ns_of_ticks((static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
        | data.ftLastWriteTime.dwLowDateTime);
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
    // The list keeps the order of the forms (the clang_path form first). The set finds a repeated form.
    std::vector<std::string> out = {g};
    std::set<std::string> seen = {g};
    auto add = [&](std::string s)
    {
        if (seen.insert(s).second)
            out.push_back(std::move(s));
    };
    // libclang adds "\\" after the include folder and after the folder of the includer. These
    // separators are near the end of the name, so each form of the last separators is added.
    // The folder before them has the "/" form (clang_path) or the "\\" form.
    std::vector<size_t> seps;
    for (size_t k = 0; k < g.size(); ++k)
        if (g[k] == '/')
            seps.push_back(k);
    // Each relative include ("../b/y.h") adds a "\\" before its first part. So the mix covers
    // the separators from the first ".." part, back by the longest run of ".." parts (the folder
    // where that include started). At most eight separators are in the mix.
    size_t m = seps.size() < 4 ? seps.size() : 4;
    size_t first_dot_dot = seps.size(), run = 0, longest = 0;
    for (size_t i = 0; i < seps.size(); ++i)
    {
        const bool dot_dot = g.compare(seps[i] + 1, 2, "..") == 0
            && (seps[i] + 3 == g.size() || g[seps[i] + 3] == '/');
        run = dot_dot ? run + 1 : 0;
        longest = run > longest ? run : longest;
        if (dot_dot && first_dot_dot == seps.size())
            first_dot_dot = i;
    }
    if (first_dot_dot < seps.size())
    {
        const size_t from = first_dot_dot > longest ? first_dot_dot - longest : 0;
        m = std::max(m, seps.size() - from);
    }
    m = std::min<size_t>(m, 8);
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

std::string text_without_dot_dot(std::string p)
{
    auto sep = [](char c) { return c == '/' || c == '\\'; };
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t k = 0; k + 2 < p.size(); ++k)
        {
            // p[k] is a separator, then "..", then a separator or the end.
            if (!sep(p[k]) || p[k + 1] != '.' || p[k + 2] != '.' || !(k + 3 == p.size() || sep(p[k + 3])))
                continue;
            size_t start = k;
            while (start > 0 && !sep(p[start - 1]))
                --start;
            const std::string name = p.substr(start, k - start);
            if (name.empty() || name == ".." || name == "." || name.back() == ':')
                continue;
            p.erase(start, k + 3 - start + (k + 3 < p.size() ? 1 : 0));
            changed = true;
            break;
        }
    }
    return p;
}

std::vector<std::string> host_path_alternatives(const std::string& path)
{
    std::vector<std::string> out;
    for (std::string s : {std::filesystem::path(path).lexically_normal().string(), text_without_dot_dot(path)})
        if (s != path && std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(std::move(s));
    return out;
}

hot::VfsStat host_stat(hot::FileSystem& fs, const std::string& path)
{
    hot::VfsStat st = fs.stat(path);
    if (st.exists)
        return st;
    for (const std::string& name : host_path_alternatives(path))
    {
        st = fs.stat(name);
        if (st.exists)
            return st;
    }
    return {};
}

bool host_read(hot::FileSystem& fs, const std::string& path, std::string& out)
{
    if (fs.read(path, out))
        return true;
    for (const std::string& name : host_path_alternatives(path))
        if (fs.read(name, out))
            return true;
    return false;
}

} // namespace hotc
