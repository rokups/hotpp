#include "spp/module.h"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <system_error>

namespace spp {

namespace {

// Opens the library at "path". When "owned_copy" is true, the keep-alive object also removes
// the file after dlclose: the file is a private copy for a core instance.
LoadResult open_library(const std::string& path, const std::string& origin, bool owned_copy)
{
    LoadResult r;
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        r.error = e ? e : "dlopen failed";
        if (owned_copy) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        return r;
    }
    std::shared_ptr<void> owner(h, [path, owned_copy](void* handle) {
        dlclose(handle);
        if (owned_copy) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    });
    auto fn = reinterpret_cast<Module::RegisterFn>(dlsym(h, "script_register"));
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
    const fs::path copy = dir / ("spp-core-" + std::to_string(getpid()) + "-" +
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
