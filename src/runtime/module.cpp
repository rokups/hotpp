#include "spp/module.h"

#ifdef _WIN32
// unverified: not built on MSVC. The Windows branch uses LoadLibraryA and GetProcAddress.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <atomic>
#include <filesystem>
#include <system_error>

namespace spp {

namespace {

#ifdef _WIN32
// unverified: not built on MSVC. These helpers wrap the Windows loader.
std::string last_error_text()
{
    const DWORD code = GetLastError();
    char* buf = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string text = "error code " + std::to_string(code);
    if (n != 0 && buf) text += ": " + std::string(buf, n);
    if (buf) LocalFree(buf);
    return text;
}
#endif

// Opens the library at "path". When "owned_copy" is true, the keep-alive object also removes
// the file after dlclose: the file is a private copy for a core instance.
LoadResult open_library(const std::string& path, const std::string& origin, bool owned_copy)
{
    LoadResult r;
#ifdef _WIN32
    // unverified: not built on MSVC.
    void* h = reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) {
#ifdef _WIN32
        // unverified: not built on MSVC.
        r.error = "LoadLibraryA failed: " + last_error_text();
#else
        const char* e = dlerror();
        r.error = e ? e : "dlopen failed";
#endif
        if (owned_copy) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        return r;
    }
    std::shared_ptr<void> owner(h, [path, owned_copy](void* handle) {
#ifdef _WIN32
        // unverified: not built on MSVC.
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
        if (owned_copy) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    });
#ifdef _WIN32
    // unverified: not built on MSVC.
    auto fn = reinterpret_cast<Module::RegisterFn>(
        reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), "script_register")));
#else
    auto fn = reinterpret_cast<Module::RegisterFn>(dlsym(h, "script_register"));
#endif
    if (!fn) {
        r.error = "symbol script_register not found in " + path;
        return r;  // "owner" closes the library
    }
    r.module = Module{fn, std::move(owner), origin, {}};
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
    if (ec) dir = fs::path(path).parent_path();
#ifdef _WIN32
    // unverified: not built on MSVC.
    const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const fs::path copy = dir / ("spp-core-" + std::to_string(pid) + "-" +
                                 std::to_string(sequence.fetch_add(1)) + "-" +
                                 fs::path(path).filename().string());
    if (!fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec)) {
        r.error = "cannot copy " + path + " for the core module: " + ec.message();
        return r;
    }
    return open_library(copy.string(), path + " (core)", true);
}

}  // namespace

LoadResult module_from_library(const std::string& path)
{
    LoadResult r = open_library(path, path, false);
    if (r) r.module->make_core = [path] { return open_core_copy(path); };
    return r;
}

}  // namespace spp
