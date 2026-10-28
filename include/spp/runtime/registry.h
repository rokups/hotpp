// The registry that a generation module fills in script_register.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include "spp/runtime/migration.h"
#include "spp/runtime/records.h"

namespace spp {

// A generation module fills the registry in script_register.
class Registry {
public:
    void add(ClassRecord* record, Factory create);
    // A thunk is a stable function address for a native delegate (section 4.11).
    void add_thunk(const std::string& name, void* fn);
    const ClassEntry* find(const std::string& name) const;
    ClassEntry* find(const std::string& name);
    void* thunk(const std::string& name) const;
    uint32_t max_generation() const { return max_generation_; }
    const std::unordered_map<std::string, ClassEntry>& entries() const { return entries_; }
    const std::unordered_map<std::string, void*>& thunks() const { return thunks_; }

private:
    std::unordered_map<std::string, ClassEntry> entries_;
    std::unordered_map<std::string, void*> thunks_;
    uint32_t max_generation_ = 0;
};

}  // namespace spp
