// The registry that a generation module fills in script_register.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include "spp/runtime/migration.h"
#include "spp/runtime/records.h"

namespace spp {

// The module state block of a generation (design 5.3, 4.7): the module-level script variables.
// "layout" is the field table. "slot" is the pointer variable inside the generation module. The
// loader writes the address of the live block there. The code of the generation reads it.
struct ModuleStateInfo {
    const StateLayout* layout = nullptr;
    void** slot = nullptr;
};

// A generation module fills the registry in script_register.
class Registry {
public:
    // A module with module variables calls it once. Without a call the module has no block.
    void set_module_state(const StateLayout* layout, void** slot) { module_state_ = {layout, slot}; }
    const ModuleStateInfo& module_state() const { return module_state_; }
    void clear_module_state() { module_state_ = {}; }

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
    ModuleStateInfo module_state_;
};

}  // namespace spp
