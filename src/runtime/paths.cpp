// default_paths(): see include/hot/paths.h.
//
// CMake defines HOT_DEFAULT_CXXFLAGS ("|" between the items) in every build.
// A development build also gets HOT_DEFAULT_CXX, HOT_DEFAULT_INCLUDES, HOT_DEFAULT_RESOURCE_DIR
// and HOT_DEFAULT_WORK_DIR. A portable build defines HOT_PORTABLE and none of these paths.
#include "hot/paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include <vector>

namespace hot
{

namespace
{

namespace fs = std::filesystem;

std::vector<std::string> split_list(const std::string& s)
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, '|');)
        if (!item.empty())
            v.push_back(item);
    return v;
}

#ifdef HOT_DEFAULT_CXXFLAGS
const char* const k_cxxflags = HOT_DEFAULT_CXXFLAGS;
#else
const char* const k_cxxflags = "";
#endif

#ifdef HOT_PORTABLE

std::string env_text(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// The directory of the running executable.
fs::path exe_dir()
{
    std::error_code ec;
#if defined(__linux__)
    std::vector<char> buf(4096);
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n > 0)
        return fs::path(std::string(buf.data(), static_cast<size_t>(n))).parent_path();
#endif
    fs::path cwd = fs::current_path(ec);
    return ec ? fs::path() : cwd;
}

bool is_executable_file(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
}

std::string find_on_path(const std::string& name)
{
    const char* path = std::getenv("PATH");
    if (!path)
        return {};
    std::stringstream ss(path);
    for (std::string dir; std::getline(ss, dir, ':');)
    {
        if (dir.empty())
            continue;
        const fs::path candidate = fs::path(dir) / name;
        if (is_executable_file(candidate))
            return candidate.string();
    }
    return {};
}

std::vector<std::string> find_include_dirs()
{
    const std::string from_env = env_text("HOT_INCLUDE_DIR");
    if (!from_env.empty())
        return {from_env};
    const fs::path dir = exe_dir();
    if (dir.empty())
        return {};
    const fs::path candidates[] = {dir / "hot" / "include", dir / ".." / "include"};
    for (const fs::path& c : candidates)
    {
        std::error_code ec;
        if (fs::exists(c / "hot" / "script.h", ec))
            return {c.lexically_normal().string()};
    }
    return {};
}

// Run "<compiler> -print-resource-dir" and return the first line of the output.
std::string query_resource_dir(const std::string& compiler)
{
    if (compiler.empty() || compiler.find('\'') != std::string::npos || compiler.find('\n') != std::string::npos)
        return {};
    const std::string cmd = "'" + compiler + "' -print-resource-dir 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return {};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        out += buf;
    if (pclose(pipe) != 0)
        return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    if (out.find('\n') != std::string::npos)
        return {};
    return out;
}

// The work directory of a portable host. A path that other users can predict or write is not safe.
// XDG_RUNTIME_DIR belongs to the user. Without it, the name of the temp directory has the user id.
std::string portable_work_dir()
{
    const std::string runtime = env_text("XDG_RUNTIME_DIR");
    if (!runtime.empty())
        return (fs::path(runtime) / "hot_work").string();
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec)
        return {};
#ifndef _WIN32
    return (tmp / ("hot_work-" + std::to_string(geteuid()))).string();
#else
    return (tmp / "hot_work").string();
#endif
}

DefaultPaths compute_paths()
{
    DefaultPaths p;
    p.compiler = env_text("HOT_CXX");
    if (p.compiler.empty())
        p.compiler = find_on_path("clang++");
    p.include_dirs = find_include_dirs();
    p.resource_dir = env_text("HOT_RESOURCE_DIR");
    if (p.resource_dir.empty())
        p.resource_dir = query_resource_dir(p.compiler);
    p.work_dir = portable_work_dir();
    p.cxxflags = split_list(k_cxxflags);
    return p;
}

#else // development build: the build tree gives the values

DefaultPaths compute_paths()
{
    DefaultPaths p;
    p.compiler = HOT_DEFAULT_CXX;
    p.include_dirs = split_list(HOT_DEFAULT_INCLUDES);
    p.resource_dir = HOT_DEFAULT_RESOURCE_DIR;
    p.work_dir = HOT_DEFAULT_WORK_DIR;
    p.cxxflags = split_list(k_cxxflags);
    return p;
}

#endif

} // namespace

std::string check_work_dir(const std::string& dir)
{
    if (dir.empty())
        return "hot: no work directory found. Set TMPDIR or XDG_RUNTIME_DIR.";
#ifdef _WIN32
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? "hot: cannot create the work directory '" + dir + "': " + ec.message() : std::string();
#else
    const std::string prefix = "hot: the work directory '" + dir + "' ";
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0)
    {
        if (errno != ENOENT)
            return prefix + "is not usable: " + std::strerror(errno);
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
            return prefix + "cannot be created: " + std::strerror(errno);
        if (lstat(dir.c_str(), &st) != 0)
            return prefix + "is not usable: " + std::strerror(errno);
    }
    if (!S_ISDIR(st.st_mode))
        return prefix + "is not a directory. Remove it or set TMPDIR to another directory.";
    if (st.st_uid != geteuid())
        return prefix + "belongs to another user. Remove it or set TMPDIR to another directory.";
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        return prefix + "can be written by other users. Run chmod go-w on it or remove it.";
    return {};
#endif
}

DefaultPaths default_paths()
{
    // The portable search runs one time. The compiler query starts a process.
    static const DefaultPaths cached = compute_paths();
    return cached;
}

} // namespace hot
