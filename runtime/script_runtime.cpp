#include "script_runtime.h"

#include <dlfcn.h>

#include <cstdio>

namespace script {

uint32_t active_generation = 0;

void stale_call(const char* what)
{
    std::fprintf(stderr, "[script] stale call ignored: %s (active generation %u)\n", what,
                 active_generation);
}

void Registry::add(ClassRecord* record, Factory create)
{
    entries_[record->name] = ClassEntry{record, create};
    if (record->generation > max_generation_) max_generation_ = record->generation;
}

const ClassEntry* Registry::find(const std::string& name) const
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

bool Loader::load(const std::string& path, std::string& error)
{
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        error = dlerror();
        return false;
    }
    using RegisterFn = void (*)(Registry&);
    auto fn = reinterpret_cast<RegisterFn>(dlsym(h, "script_register"));
    if (!fn) {
        error = "symbol script_register not found";
        dlclose(h);
        return false;
    }
    fn(registry_);
    handles_.push_back(h);
    active_generation = registry_.max_generation();
    return true;
}

}  // namespace script
