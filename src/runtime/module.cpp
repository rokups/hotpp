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
    #include <process.h>
    #include <windows.h>
#else
    #include <dlfcn.h>
    #include <unistd.h>
#endif
#ifdef __APPLE__
    #include "hot/command_line.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>

namespace hot
{

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

// Opens the library at "path". When "owned_copy" is true, the keep-alive object also removes
// the file after dlclose: the file is a private copy for a core instance.
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
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        return r;
    }
    std::shared_ptr<void> owner(h, [path, owned_copy](void* handle)
    {
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
        if (owned_copy)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
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

// A core instance of a library (module.h). dlopen gives the same handle for the same file,
// also through a hard link. So the core instance loads a private copy of the file.
LoadResult open_core_copy(const std::string& path)
{
    static std::atomic<unsigned> sequence{0};
    namespace fs = std::filesystem;
    LoadResult r;
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec)
        dir = fs::path(path).parent_path();
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const fs::path copy = dir
        / ("hot-core-" + std::to_string(pid) + "-" + std::to_string(sequence.fetch_add(1)) + "-"
            + fs::path(path).filename().string());
    if (!fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec))
    {
        r.error = "cannot copy " + path + " for the core module: " + ec.message();
        return r;
    }
    return open_library(copy.string(), path + " (core)", true);
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

std::vector<std::string> run_compiles(std::vector<PendingLoad*>& pending, unsigned jobs)
{
    std::vector<std::string> errors(pending.size());
    std::atomic<size_t> next{0};
    const auto work = [&]
    {
        for (size_t i = next.fetch_add(1); i < pending.size(); i = next.fetch_add(1))
        {
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
