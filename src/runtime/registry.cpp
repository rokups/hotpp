#include "spp/runtime/registry.h"

namespace script {

void Registry::add(ClassRecord* record, Factory create)
{
    entries_[record->name] = ClassEntry{record, create};
    if (record->generation > max_generation_) max_generation_ = record->generation;
}

void Registry::add_thunk(const std::string& name, void* fn)
{
    thunks_.emplace(name, fn);  // the first address stays
}

const ClassEntry* Registry::find(const std::string& name) const
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

ClassEntry* Registry::find(const std::string& name)
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

void* Registry::thunk(const std::string& name) const
{
    auto it = thunks_.find(name);
    return it == thunks_.end() ? nullptr : it->second;
}

}  // namespace script
