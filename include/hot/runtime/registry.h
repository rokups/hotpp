// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The registry that a generation module fills in script_register.
#pragma once
#include "hot/runtime/hidden.h"
#include "hot/runtime/migration.h"
#include "hot/runtime/records.h"
#include "hot/runtime/shell.h"

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

// A free function of a unit for the code of another unit (call_fn). "sig" is typeid(Sig).name() of
// the function type. The text lives in the module of the entry.
struct FnEntry
{
    std::string name;
    std::string sig;
    void* fn = nullptr;
};

namespace detail
{
// A new unique id for each function table. The count is global to all modules of the process.
HOT_EXPORT uint64_t next_fn_table_id();
} // namespace detail

// The free functions of a unit, by name. "id" is unique for the process, so a call site can find a
// table swap even when a new table has the address of an old one.
struct FnTable
{
    uint64_t id = detail::next_fn_table_id();
    std::unordered_map<std::string, FnEntry> entries;
    const FnEntry* find(const std::string& name) const
    {
        auto it = entries.find(name);
        return it == entries.end() ? nullptr : &it->second;
    }
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

    HOT_EXPORT void add(ClassRecord* record, Factory create);
    // A thunk is a stable function address for a native delegate (section 4.11). The first
    // registration of a key keeps its address (append-only).
    HOT_NODEBUG HOT_EXPORT void add_thunk(const std::string& name, void* fn);
    // A thunk for a changed signature of a script function (design 5.3). "sig" is
    // typeid(Sig).name() of the slot signature. "slot" is the slot that the thunk calls.
    HOT_NODEBUG HOT_EXPORT void add_thunk(const std::string& name, void* fn, const char* sig, unsigned slot);
    HOT_NODEBUG HOT_EXPORT void add_thunk(const ThunkEntry& entry);
    // A free function for other units. A second call with the same name replaces the entry.
    // "sig_key" is typeid(Sig).name() of the function type.
    HOT_NODEBUG HOT_EXPORT void add_fn(const std::string& name, const char* sig_key, void* fn);
    const FnEntry* find_fn(const std::string& name) const { return fns_.find(name); }
    const FnTable& fn_table() const { return fns_; }
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
    FnTable fns_;
    ModuleStateInfo module_state_;
    uint32_t* live_flag_ = nullptr;
};

// The slot of a call site of call_fn: the function that the site found, and the id of the table it
// came from. The site is a static variable of the generated code. It follows the thread rule of
// script code (threads.h).
struct FnSite
{
    uint64_t table_id = 0;
    void* fn = nullptr;
};

namespace detail
{
template <class Sig> struct sig_fnptr;
template <class R, class... P> struct sig_fnptr<R(P...)>
{
    using type = R (*)(P...);
};
// Finds the function "name" in the table of the unit and checks the signature key. It throws
// ScriptError when the unit has no table, the name is missing or the key differs.
HOT_NODEBUG HOT_EXPORT void* resolve_fn(const UnitRef& unit, const char* name, const char* sig_key);
} // namespace detail

// Calls the free function "name" of another unit through its function table. "Sig" is the type of
// the function, for example "int(int, float)". A different signature throws ScriptError. The
// site caches the function while the table of the unit stays the same.
template <class Sig, class... A>
HOT_HIDDEN typename detail::sig_result<Sig>::type call_fn(
    const UnitRef& unit, FnSite& site, const char* name, A&&... a)
{
    const FnTable* t = unit.info.fns;
    if (HOT_UNLIKELY(!t || site.table_id != t->id))
    {
        site.fn = detail::resolve_fn(unit, name, typeid(Sig).name());
        site.table_id = t->id;
    }
    return reinterpret_cast<typename detail::sig_fnptr<Sig>::type>(site.fn)(std::forward<A>(a)...);
}

} // namespace hot

// The entry point of a generation module. The loader finds it by name. This declaration gives it
// default visibility (a DLL export on Windows), so the module can build with -fvisibility=hidden.
// The generated definition has no attribute: it takes the attribute of this declaration.
extern "C" HOT_MODULE_EXPORT void script_register(hot::Registry& reg);
