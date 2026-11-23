// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/backend.h"
#include "hot/module.h"

#ifdef _WIN32
    // The Windows branch uses LoadLibraryA and GetProcAddress.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    // An AOT-only build (HOT_AOT_ONLY) loads no library, so it has no dlfcn.h.
    #ifndef HOT_AOT_ONLY
        #include <dlfcn.h>
    #endif
    #include <unistd.h>
#endif
#ifdef __APPLE__
    #include <mach-o/dyld.h>
    #include "hot/command_line.h"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>

namespace hot
{

// Internal helpers for the core copy. The tests call them. They are not public API.
// The folder for the core copies of this application: "<temp>/hot-core-<exe name>".
std::filesystem::path core_app_folder(const std::filesystem::path& temp)
{
    std::string name;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        name = std::filesystem::path(std::wstring(buf, n)).filename().string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0)
        name = std::filesystem::path(buf).filename().string();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf);
    if (n > 0)
        name = std::filesystem::path(std::string(buf, static_cast<size_t>(n))).filename().string();
#endif
    if (name.empty())
        name = "app";
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-')
            c = '_';
    return temp / ("hot-core-" + name);
}

// Deletes each regular file (and each symlink, not its target) in the folder. A failure is not an
// error: a file in use or a file of a concurrent run stays. A subfolder stays.
void sweep_core_folder(const std::filesystem::path& folder)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code ec2;
        const fs::file_status st = it->symlink_status(ec2);
        if (!ec2 && (fs::is_regular_file(st) || fs::is_symlink(st)))
            fs::remove(it->path(), ec2);
    }
}

namespace
{

#ifdef _WIN32
// These helpers wrap the Windows loader.
std::string last_error_text()
{
    const DWORD code = GetLastError();
    char* buf = nullptr;
    const DWORD n =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string text = "error code " + std::to_string(code);
    if (n != 0 && buf)
        text += ": " + std::string(buf, n);
    if (buf)
        LocalFree(buf);
    return text;
}
#endif

// Removes a private core copy. The fallback directory "hot-core~XXXXXX" holds only this copy: remove
// it too. The name has a "~", so it never equals the shared folder "hot-core-<exe name>".
void remove_owned_copy(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.filename().string().rfind("hot-core~", 0) == 0)
        std::filesystem::remove(dir, ec);
}

// Opens the library at "path". When "owned_copy" is true, the keep-alive object also removes
// the file after dlclose: the file is a private copy for a core instance.
#ifdef HOT_AOT_ONLY
// An AOT-only platform (iOS, Android, web, UWP) cannot load generated code. The function is an
// error stub, so the build has no dlopen and no LoadLibrary.
LoadResult open_library(const std::string& path, const std::string&, bool owned_copy)
{
    LoadResult r;
    r.error = "this build cannot load a library (HOT_AOT_ONLY): " + path;
    if (owned_copy)
        remove_owned_copy(path);
    return r;
}
#else
LoadResult open_library(const std::string& path, const std::string& origin, bool owned_copy)
{
    LoadResult r;
#ifdef _WIN32
    void* h = reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h)
    {
#ifdef _WIN32
        r.error = "LoadLibraryA failed: " + last_error_text();
#else
        const char* e = dlerror();
        r.error = e ? e : "dlopen failed";
#ifdef __APPLE__
        if (const std::string hint = macos_entitlement_hint(r.error); !hint.empty())
            r.error += "\n" + hint;
#endif
#endif
        if (owned_copy)
            remove_owned_copy(path);
        return r;
    }
#ifdef _WIN32
    // A loaded DLL cannot be deleted: the keep-alive object removes the copy after FreeLibrary.
#else
    // The mapping stays valid after unlink. A crash or a kill then leaves no copy in the temp directory.
    if (owned_copy)
        remove_owned_copy(path);
#endif
    std::shared_ptr<void> owner(h, [path, owned_copy](void* handle)
    {
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
        if (owned_copy)
            remove_owned_copy(path);
    });
#ifdef _WIN32
    auto fn = reinterpret_cast<Module::RegisterFn>(
        reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), "script_register")));
#else
    auto fn = reinterpret_cast<Module::RegisterFn>(dlsym(h, "script_register"));
#endif
    if (!fn)
    {
        r.error = "symbol script_register not found in " + path;
        return r; // "owner" closes the library
    }
    // A missing "hot_main" is not an error: the module has no entry point.
#ifdef _WIN32
    auto entry = reinterpret_cast<Module::EntryMainFn>(
        reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), "hot_main")));
#else
    auto entry = reinterpret_cast<Module::EntryMainFn>(dlsym(h, "hot_main"));
#endif
    // A module that the tool hotc built with "--gen N" names its generation. Without the symbol
    // the loader picks the number.
#ifdef _WIN32
    auto gen = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), "hot_module_generation")));
#else
    auto gen = reinterpret_cast<const uint32_t*>(dlsym(h, "hot_module_generation"));
#endif
    r.module = Module{fn, std::move(owner), origin, {}, {}};
    r.module->entry_main = entry;
    r.module->generation = gen ? *gen : 0u;
    return r;
}
#endif

// Copies "src" to the new file "dst". The creation is exclusive and does not follow a symlink.
bool copy_new_file(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err)
{
    std::ifstream in(src, std::ios::binary);
    if (!in)
    {
        err = "cannot read " + src.string();
        return false;
    }
#ifdef _WIN32
    HANDLE h = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = "cannot create " + dst.string() + ": " + last_error_text();
        return false;
    }
#else
    const int fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700);
    if (fd < 0)
    {
        err = "cannot create " + dst.string() + ": " + std::strerror(errno);
        return false;
    }
#endif
    char buf[65536];
    bool ok = true;
    while (ok && (in.read(buf, sizeof buf), in.gcount() > 0))
    {
        const char* p = buf;
        size_t n = static_cast<size_t>(in.gcount());
        while (ok && n > 0)
        {
#ifdef _WIN32
            DWORD w = 0;
            ok = WriteFile(h, p, static_cast<DWORD>(n), &w, nullptr) && w > 0;
#else
            const ssize_t w = write(fd, p, n);
            ok = w > 0;
#endif
            if (ok)
            {
                p += w;
                n -= static_cast<size_t>(w);
            }
        }
    }
    ok = ok && in.eof();
#ifdef _WIN32
    CloseHandle(h);
#else
    close(fd);
#endif
    if (!ok)
    {
        std::error_code ec;
        std::filesystem::remove(dst, ec);
        err = "cannot write " + dst.string();
    }
    return ok;
}

#ifndef _WIN32
// The shared folder is safe when it is a real directory of this user that no other user can write.
bool shared_folder_ok(const std::filesystem::path& dir)
{
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    struct stat st;
    return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid()
        && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}
#endif

LoadResult open_core_copy_once(const std::string& path)
{
    namespace fs = std::filesystem;
    LoadResult r;
    std::error_code ec;
    fs::path temp = fs::temp_directory_path(ec);
    if (ec)
        temp = fs::path(path).parent_path();
    fs::path dir = core_app_folder(temp);
    bool shared = true;
#ifdef _WIN32
    fs::create_directories(dir, ec); // The temp directory of a Windows user is private.
#else
    if (!shared_folder_ok(dir))
    {
        // Another user can own the shared folder: use a private directory (mode 0700) for this copy.
        std::string tmpl = (temp / "hot-core~XXXXXX").string();
        if (!mkdtemp(tmpl.data()))
        {
            r.error = "cannot create a directory for the core module: " + std::string(std::strerror(errno));
            return r;
        }
        dir = tmpl;
        shared = false;
    }
#endif
    if (shared)
    {
        static std::once_flag swept;
        std::call_once(swept, [&] { sweep_core_folder(dir); });
    }
    std::random_device rd;
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "%08x-", static_cast<unsigned>(rd()));
    const fs::path copy = dir / (suffix + fs::path(path).filename().string());
    if (!copy_new_file(path, copy, r.error))
    {
        r.error = "cannot copy " + path + " for the core module: " + r.error;
        remove_owned_copy(copy.string());
        return r;
    }
    return open_library(copy.string(), path + " (core)", true);
}

// A core instance of a library (module.h). dlopen gives the same handle for the same file,
// also through a hard link. So the core instance loads a private copy of the file.
LoadResult open_core_copy(const std::string& path)
{
    // A concurrent instance of the same application sweeps the shared folder at its start. It can
    // delete a copy that this instance made but did not load yet. So try one more time.
    LoadResult r = open_core_copy_once(path);
    if (!r)
        r = open_core_copy_once(path);
    return r;
}

} // namespace

LoadResult module_from_library(const std::string& path)
{
    LoadResult r = open_library(path, path, false);
    if (r)
        r.module->make_core = [path] { return open_core_copy(path); };
    return r;
}


namespace
{

// The default PendingLoad: the result of Backend::load, kept for finish.
class ImmediateLoad final : public PendingLoad
{
public:
    explicit ImmediateLoad(LoadResult result) : result_(std::move(result))
    {
        head.error = result_.error;
        head.reason = result_.reason;
        head.dependencies = result_.dependencies;
        head.iface_hash = result_.iface_hash;
        head.import_hashes = result_.import_hashes;
        head.iface_info = result_.iface_info;
        head.use_info = result_.use_info;
        ok = result_.error.empty();
    }
    LoadResult finish() override { return std::move(result_); }

private:
    LoadResult result_;
};

} // namespace

std::unique_ptr<PendingLoad> Backend::begin_load(const ScriptSource& source, uint32_t generation, LoadMode mode)
{
    return std::make_unique<ImmediateLoad>(load(source, generation, mode));
}

std::vector<std::unique_ptr<PendingLoad>> Backend::begin_load_many(
    const std::vector<ScriptSource>& sources, uint32_t generation, LoadMode mode)
{
    std::vector<std::unique_ptr<PendingLoad>> result;
    result.reserve(sources.size());
    for (const ScriptSource& source : sources)
    {
        try
        {
            result.push_back(begin_load(source, generation, mode));
        }
        catch (const std::exception& e)
        {
            LoadResult failed;
            failed.error = e.what()[0] ? e.what() : "the load failed";
            failed.reason = ReloadReason::CompileError;
            result.push_back(std::make_unique<ImmediateLoad>(std::move(failed)));
        }
    }
    return result;
}

unsigned build_jobs()
{
#ifdef _WIN32
    return 1;
#else
    if (const char* env = std::getenv("HOT_JOBS"))
    {
        char* end = nullptr;
        const unsigned long value = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0' && value > 0 && value <= 4096)
            return static_cast<unsigned>(value);
    }
    // Peak memory grows with the job count and with the project. Set HOT_JOBS to change the count.
    return std::min(std::max(1u, std::thread::hardware_concurrency()), kDefaultMaxJobs);
#endif
}

unsigned build_jobs_for(size_t units, unsigned hardware, unsigned cap)
{
    if (units > 1 && units <= std::max(1u, hardware) && units <= cap)
        return static_cast<unsigned>(units);
    return std::min(std::max(1u, hardware), cap);
}

unsigned build_jobs_for(size_t units)
{
#ifdef _WIN32
    return 1;
#else
    if (std::getenv("HOT_JOBS"))
        return build_jobs();
    return build_jobs_for(units, std::thread::hardware_concurrency(), kDefaultMaxJobs);
#endif
}

std::vector<std::string> run_compiles(std::vector<PendingLoad*>& pending, unsigned jobs)
{
    std::vector<std::string> errors(pending.size());
    // The largest unit starts first, so the last unit to end is a small one. The sort is stable:
    // equal costs keep the order of the caller. The errors keep the index of the caller.
    std::vector<size_t> order(pending.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pending[a]->cost > pending[b]->cost; });
    std::atomic<size_t> next{0};
    const auto work = [&]
    {
        for (size_t n = next.fetch_add(1); n < order.size(); n = next.fetch_add(1))
        {
            const size_t i = order[n];
            try
            {
                pending[i]->compile();
            }
            catch (const std::exception& e)
            {
                errors[i] = e.what()[0] ? e.what() : "the compile failed";
            }
            catch (...)
            {
                errors[i] = "the compile failed";
            }
        }
    };
    const size_t count = std::min<size_t>(jobs, pending.size());
    if (count <= 1)
    {
        work();
        return errors;
    }
    std::vector<std::thread> threads;
    try
    {
        // This thread is a job too: it starts count - 1 threads.
        for (size_t i = 1; i < count; ++i)
            threads.emplace_back(work);
    }
    catch (...)
    {
        // A thread did not start: the started threads and this thread do the work.
    }
    work();
    for (std::thread& t : threads)
        t.join();
    return errors;
}

} // namespace hot
