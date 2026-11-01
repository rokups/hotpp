// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The registry that a generation module fills in script_register.
#pragma once
#include "hot/runtime/hidden.h"
#include "hot/runtime/migration.h"
#include "hot/runtime/records.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace hot
{

// The module state block of a generation (design 5.3, 4.7): the module-level script variables.
// "layout" is the field table. "slot" is the pointer variable inside the generation module. The
// loader writes the address of the live block there. The code of the generation reads it.
struct ModuleStateInfo
{
    const StateLayout* layout = nullptr;
    void** slot = nullptr;
};

// A thunk in the registry (section 4.11, design 5.3). The key of a thunk is its name for the
// first signature of a function. A thunk for a changed signature has the key "name#sig".
struct ThunkEntry
{
    void* fn = nullptr;
    std::string name; // "Cls::fn"
    // The signature key: typeid(Sig).name() of the slot signature. Empty: the first signature
    // of the function. Then the slot has no key and the thunk is not checked.
    std::string sig;
    unsigned slot = 0; // the slot that the thunk calls (only with a signature key)
};

// A generation module fills the registry in script_register.
class Registry
{
public:
    // A module with module variables calls it once. Without a call the module has no block.
    void set_module_state(const StateLayout* layout, void** slot) { module_state_ = {layout, slot}; }
    const ModuleStateInfo& module_state() const { return module_state_; }
    void clear_module_state() { module_state_ = {}; }

    // The generated code calls it once. "flag" is the live flag of the module: the guard of each
    // function of the module reads it. The Loader sets it to 1 when the module is live and to 0
    // when a newer module of the same unit replaces it. Without a call the flag is null: the
    // module has the old guard, which compares the generation with hot::active_generation.
    void set_live_flag(uint32_t* flag) { live_flag_ = flag; }
    uint32_t* live_flag() const { return live_flag_; }

    void add(ClassRecord* record, Factory create);
    // A thunk is a stable function address for a native delegate (section 4.11). The first
    // registration of a key keeps its address (append-only).
    HOT_NODEBUG void add_thunk(const std::string& name, void* fn);
    // A thunk for a changed signature of a script function (design 5.3). "sig" is
    // typeid(Sig).name() of the slot signature. "slot" is the slot that the thunk calls.
    HOT_NODEBUG void add_thunk(const std::string& name, void* fn, const char* sig, unsigned slot);
    HOT_NODEBUG void add_thunk(const ThunkEntry& entry);
    const ClassEntry* find(const std::string& name) const;
    ClassEntry* find(const std::string& name);
    // The thunk of the latest signature of a name, or the thunk with this exact key.
    void* thunk(const std::string& name) const;
    bool has_thunk(const std::string& key) const { return thunks_.count(key) != 0; }
    // The key of the latest thunk of a name, or empty.
    std::string current_thunk_key(const std::string& name) const;
    static std::string thunk_key(const std::string& name, const std::string& sig)
    {
        return sig.empty() ? name : name + "#" + sig;
    }
    uint32_t max_generation() const { return max_generation_; }
    const std::unordered_map<std::string, ClassEntry>& entries() const { return entries_; }
    const std::unordered_map<std::string, ThunkEntry>& thunks() const { return thunks_; }

private:
    std::unordered_map<std::string, ClassEntry> entries_;
    std::unordered_map<std::string, ThunkEntry> thunks_; // by key
    std::unordered_map<std::string, std::string> current_; // the name to the latest key
    uint32_t max_generation_ = 0;
    ModuleStateInfo module_state_;
    uint32_t* live_flag_ = nullptr;
};

} // namespace hot

// The entry point of a generation module. The loader finds it by name. This declaration gives it
// default visibility (a DLL export on Windows), so the module can build with -fvisibility=hidden.
// The generated definition has no attribute: it takes the attribute of this declaration.
extern "C" HOT_MODULE_EXPORT void script_register(hot::Registry& reg);
