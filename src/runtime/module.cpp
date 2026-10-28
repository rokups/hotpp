#include "spp/module.h"

#include <dlfcn.h>

namespace script {

LoadResult module_from_library(const std::string& path)
{
    LoadResult r;
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        r.error = e ? e : "dlopen failed";
        return r;
    }
    std::shared_ptr<void> owner(h, [](void* handle) { dlclose(handle); });
    auto fn = reinterpret_cast<Module::RegisterFn>(dlsym(h, "script_register"));
    if (!fn) {
        r.error = "symbol script_register not found in " + path;
        return r;  // "owner" closes the library
    }
    r.module = Module{fn, std::move(owner), path};
    return r;
}

}  // namespace script
