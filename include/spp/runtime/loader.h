// The loader of generation modules: validation, commit and unload.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "spp/module.h"
#include "spp/runtime/records.h"
#include "spp/runtime/registry.h"

namespace spp {

// The state of a generation module in the loader (section 4.9).
enum class GenState : uint8_t {
    Active,    // the live generation
    Retired,   // a reload replaced it; the code stays loaded
    Unloaded,  // the loader released the code (dlclose or ORC tracker removal)
};

struct GenerationInfo {
    uint32_t generation;
    GenState state;
    uint32_t pins;       // pin_count(generation)
    uint32_t code_refs;  // registry entries, thunks and live objects that use the module code
    std::string origin;
};

// One script class in the stats (design 8.4).
struct ClassInfo {
    std::string name;
    uint32_t generation;  // the generation of the dispatch table
    uint32_t slots;       // the number of slots in the dispatch table
    size_t live_objects;
    bool removed;         // the source dropped the class; the slots point to the removed class handler
};

struct LoaderStats {
    uint32_t active_generation = 0;
    std::vector<GenerationInfo> generations;  // in load order
    size_t live_objects = 0;
    size_t core_modules = 0;                  // loaded core modules (module.h); they never unload
    bool state_holds_code = false;            // a live state block blocks each unload (kMayHoldCode)
    std::vector<ClassInfo> classes;           // sorted by name
    size_t slots = 0;                         // the sum of the slots of all classes
    uint64_t removed_class_calls = 0;         // calls that the removed class handler ignored
};

// The text of the stats for a log or a command channel ("script_stats", design 8.4).
std::string format_stats(const LoaderStats& stats);

// The handler that the slots of a removed class (design 6.1) point to. It logs the first
// call, counts the calls and returns zero (a null pointer, 0 or false). The caller may pass any
// arguments. A floating point result is not defined.
void* removed_class_handler();
uint64_t removed_class_call_count();

// Restart request (design 6.6). The runtime calls it when it cannot go on, for example
// when a native move constructor throws during a migration. The engine sets a handler
// that saves its work and restarts the process. Without a handler the runtime aborts.
// When the handler returns, the failed commit returns false, the old generation stays
// active and restart_requested() is true.
using RestartHandler = void (*)(const char* reason);
void set_restart_handler(RestartHandler handler);  // null: abort
bool restart_requested();
void clear_restart_request();
void request_restart(const char* reason);

// Registers generation modules. A module has the function: extern "C" void script_register(Registry&).
//
// Threads. The Loader has one mutex.
// It guards "pending_", "registry_", the class records in "registry_",
// and the write of active_generation.
//   - request_reload and reload_pending can run on any thread.
//     They can run while workers run script code. They take the mutex.
//     They read only data that changes in commit_pending (under the same mutex)
//     and the data of the modules, which does not change.
//     They write only the staged data of the new Pending.
//   - commit_pending runs only at the frame boundary, on the main thread,
//     when no worker is in script code. It takes the mutex.
//     So it waits for a validation that runs at that time.
//     It also checks in_script_tick() (debug check).
//   - load, frame_boundary, stats, find and thunk run on the main thread only.
//     The workers do not call them.
//
// The first module goes through load(). A later module goes through
// request_reload(), which only validates (no side effect on live objects),
// and then commit_pending(), which the World loop calls at a safe point.
// A class that the new generation does not have is removed (design 6.1). Its record stays,
// the live objects stay valid, and each slot points to removed_class_handler().
// The canonical record of a class stays the record of the class for ever. A
// commit copies the slots and the layout of the new record into it.
// A thunk keeps the address of the first registration (append-only).
//
// Core modules (design 4.9, M8). When a generation adds a class or a thunk, the loader asks
// the Module for a core instance (Module::make_core). The core is a second instance of the
// same code. It gives the canonical record, the factory, the shell constructor (and so the
// vtable of each object) and the thunks of the new parts. The loader keeps each core until
// it is destroyed. A generation with a core holds no entry of the registry.
//
// The loader owns every Module. A generation that a reload replaced is
// retired. frame_boundary() unloads a retired generation when all of these hold:
//   1. its pin count is zero;
//   2. at least one frame boundary passed after the commit that retired it;
//   3. no code of the module is in use (code_refs is zero).
// Condition 3 counts the entries of the registry that the module holds (only without a core),
// and the live objects that use the module: see live_by_module.
// No retired generation unloads while a live state block has a field with kMayHoldCode
// (records.h), for example a std::shared_ptr. Its control block can have the vtable of
// any generation that ran. A value that native code keeps is not seen (design 4.9).
class Loader {
public:
    Loader();
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Registers the first module.
    bool load(Module module, std::string& error);

    // Validation phase. Returns false and leaves the live generation unchanged
    // when the module is not acceptable. A Backend or module_from_library makes the Module.
    bool request_reload(Module module, std::string& error);
    bool reload_pending() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_ != nullptr;
    }
    // Commit phase. Call it only at a frame boundary. Returns true when a
    // reload was committed. On false, "error" tells why (empty: nothing pending).
    bool commit_pending(std::string& error);

    // Call it at each frame boundary, after commit_pending. It unloads the
    // retired generations that may unload, then counts the boundary.
    // Returns the number of generations that it unloaded.
    unsigned frame_boundary();

    LoaderStats stats() const;
    std::string stats_text() const { return format_stats(stats()); }

    const ClassEntry* find(const std::string& name) const { return registry_.find(name); }
    // The module state of the live generation: the layout and the slot. Null layout: none.
    ModuleStateInfo module_state() const { return {mod_layout_, mod_slot_}; }
    void* thunk(const std::string& name) const { return registry_.thunk(name); }

private:
    struct Pending;
    struct Gen {
        uint32_t generation = 0;
        GenState state = GenState::Active;
        Module module;               // empty after the unload
        uint64_t retired_frame = 0;  // the frame counter at the retirement
        uint32_t registry_refs = 0;  // classes and thunks that this module registered
    };
    void add_generation(Module module, uint32_t generation);
    void note_records(const Registry& reg, uint32_t generation);
    // Makes the module state block of a registry and sets the slot. False: an initializer threw.
    bool make_module_block(const ModuleStateInfo& info, std::string& error);
    // The number of live objects for each module (the module that made the object).
    std::unordered_map<uint32_t, uint32_t> live_by_module() const;
    uint32_t code_refs(const Gen& g, const std::unordered_map<uint32_t, uint32_t>& live) const;
    // True when a live state block (an object or the module block) has a field with kMayHoldCode.
    bool state_holds_code() const;
    // Sets the signature key of the slot of a keyed thunk in the canonical record (design 5.3).
    void set_slot_key(const ThunkEntry& t);

    mutable std::mutex mutex_;  // see "Threads" above
    Registry registry_;
    std::vector<Module> cores_;  // the core modules; the destructor removes them after "gens_"
    std::vector<Gen> gens_;  // the loader owns all modules; a retired one stays until it unloads
    std::unordered_map<const ClassRecord*, uint32_t> record_gen_;  // the module that holds a record
    uint64_t frame_ = 0;
    std::unique_ptr<Pending> pending_;
    // The dispatch tables of the removed classes (design 6.1), by class name. The record of a
    // removed class keeps its place in "registry_" while objects live.
    std::unordered_map<std::string, std::vector<void*>> removed_;
    // The live module state block (module-level script variables). Null: the script has none.
    // "mod_slot_" is the slot variable of the module that made the layout. It points to the block.
    const StateLayout* mod_layout_ = nullptr;
    void** mod_slot_ = nullptr;
    char* mod_block_ = nullptr;
    // The signature keys of the slots (ClassRecord::slot_keys), by class name. A class gets a
    // table when a reload changes the signature of one of its script functions (design 5.3).
    std::unordered_map<std::string, std::vector<const char*>> slot_keys_;
    std::set<std::string> key_text_;  // the text of the keys; a module can unload
};

}  // namespace spp
