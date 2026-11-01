// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The loader of generation modules: validation, commit and unload.
#pragma once
#include "hot/aot.h"
#include "hot/config.h"
#include "hot/module.h"
#include "hot/runtime/records.h"
#include "hot/runtime/registry.h"
#include "hot/runtime/reload_result.h"
#include "hot/runtime/threads.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hot
{

class Backend;

// The state of a generation module in the loader (section 4.9).
enum class GenState : uint8_t
{
    Active, // the live generation
    Retired, // a reload replaced it; the code stays loaded
    Unloaded, // the loader released the code (dlclose or ORC tracker removal)
};

struct GenerationInfo
{
    uint32_t generation;
    GenState state;
    uint32_t pins; // pin_count(generation)
    uint32_t code_refs; // registry entries, thunks and live objects that use the module code
    std::string origin;
};

// One script class in the stats (design 8.4).
struct ClassInfo
{
    std::string name;
    uint32_t generation; // the generation of the dispatch table
    uint32_t slots; // the number of slots in the dispatch table
    size_t live_objects;
    bool removed; // the source dropped the class; the slots point to the removed class handler
};

struct LoaderStats
{
    uint32_t active_generation = 0;
    std::vector<GenerationInfo> generations; // in load order
    size_t live_objects = 0;
    size_t core_modules = 0; // loaded core modules (module.h); they never unload
    bool state_holds_code = false; // a live state block blocks each unload (kMayHoldCode)
    std::vector<ClassInfo> classes; // sorted by name
    size_t slots = 0; // the sum of the slots of all classes
    uint64_t removed_class_calls = 0; // calls that the removed class handler ignored
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
void set_restart_handler(RestartHandler handler); // null: abort
bool restart_requested();
void clear_restart_request();
void request_restart(const char* reason);

// Throws std::runtime_error when the HOT_THREAD_SAFE value of the host differs from the value of
// the runtime library (config.h). The constructor of the Loader calls it. The scripts get the
// value from the flags of the backend (thread_safe_compile_flags).
void check_thread_safe_build(bool host_thread_safe);

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
//   - StopTheWorld mode (the default, design 6.3): commit_pending runs only at the frame
//     boundary, on the main thread, when no worker is in script code. It takes the mutex.
//     So it waits for a validation that runs at that time.
//     It also checks in_script_tick() (debug check).
//   - EntryCounters mode (set_sync_mode, threads.h): commit_pending can run on any one thread
//     while other threads call script code. It opens a CommitWindow first: new script entries
//     block and the commit waits until no thread is in script code. Then it takes the mutex.
//     It fails when the calling thread is in script code (entry_depth() != 0).
//     frame_boundary may run on the commit thread after the commit.
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
class Loader
{
public:
    // The inline constructor passes the HOT_THREAD_SAFE value of the host TU to the check.
    Loader() : Loader(HOT_THREAD_SAFE != 0) {}
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Registers the first module. A full reload also uses it for the fresh generation, after
    // reset. The result is Applied or Failed. It calls Module::on_accept on success.
    // It fails with ReloadReason::BadModule when the loader already has a generation: call reset
    // first.
    ReloadResult load(Module module);

    // Registers all scripts of an AOT table (include/hot/aot.h) as generation 1. Each script is
    // one unit: the unit name is the name of the script. The classes of the scripts keep the
    // C++ of the binary; no library loads. A later reload of one unit (request_reload with
    // Module::unit set to the name of the script) leaves the other units alone. The result is
    // Applied or Failed. It fails with BadModule when the loader already has a generation, when
    // the table has no script, when two scripts have a class of the same name, or when an
    // initializer of a module variable throws. After a failure the loader is empty again.
    // The loader keeps a copy of each AOT class record: see aot_record().
    ReloadResult load_aot(const AotTable& table);

    // Drops all state of the loader for a full reload. The sequence of the host is: save the host
    // state, drop all script handles, closures and thunk pointers, reset, load the fresh module
    // (Backend::load with LoadMode::Fresh) and call load() again, then restore the host state.
    // It returns false and changes nothing when one of these is true (checked in this order):
    // a reload is pending; live script objects remain (the error names the classes and the
    // counts, for example "3 live objects remain: ClassA x2, ClassB x1"); a generation is pinned
    // (a closure or a running call); the call comes from script code.
    // The check for a call from script code is exact only in EntryCounters mode (and in a debug
    // build, which also checks in_script_tick()). In StopTheWorld mode with NDEBUG no counter
    // exists. The host must not call reset from script code.
    // On success the loader has no generation, no class and no module state block.
    // The libraries unload, active_generation is zero and the next load() starts at generation 1.
    // These host pointers dangle after a reset: every pointer from thunk(), every ClassEntry or
    // record pointer from find(), the layout and the slot from module_state(), every function
    // pointer, closure and handle that came from a script module. Call it on the main thread
    // when no worker runs script code.
    bool reset(std::string& error);

    // Validation phase. The result is Pending when the module is acceptable. Module::on_accept
    // runs later, when commit_pending makes the generation live. Else the result is Failed or
    // FullReloadRequired, with the reason and the classes involved, and the live generation
    // stays unchanged.
    // FullReloadRequired means that hot reload cannot apply the change: the host saves its
    // state, frees all script objects and loads the module again as a fresh generation.
    // A Backend or module_from_library makes the Module.
    ReloadResult request_reload(Module module);

    // The host calls this overload when it checked the LoadResult of a backend itself. A
    // failed LoadResult becomes a result without a call of the validation. A backend failure
    // with the reason SlotsRemoved becomes FullReloadRequired. Any other failure becomes
    // Failed with the reason of the LoadResult (CompileError when it is None).
    // A successful LoadResult goes to request_reload(Module).
    ReloadResult request_reload(LoadResult built);
    bool reload_pending() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_ != nullptr;
    }
    // Commit phase. Call it only at a frame boundary. The result is Applied when a reload
    // was committed. Else the result is Failed. Nothing pending gives Failed with the reason
    // None and an empty error. A commit that cannot run now gives Busy. A commit that cannot
    // migrate the state gives MigrationFailed.
    ReloadResult commit_pending();

    // Call it at each frame boundary, after commit_pending. It unloads the
    // retired generations that may unload, then counts the boundary.
    // Returns the number of generations that it unloaded.
    unsigned frame_boundary();

    // The synchronization mode of the commit (design 6.3, threads.h). The default is
    // StopTheWorld. The mode applies to the whole process, because the generation code reads
    // one global flag. Change it only when no thread is in script code. The destructor of the
    // Loader sets StopTheWorld again.
    void set_sync_mode(SyncMode mode);
    SyncMode sync_mode() const;

    LoaderStats stats() const;
    std::string stats_text() const { return format_stats(stats()); }

    const ClassEntry* find(const std::string& name) const { return registry_.find(name); }
    // The module state of the live generation: the layout and the slot. Null layout: none.
    // "unit" selects the unit (Module::unit). The empty unit is the default.
    ModuleStateInfo module_state(const std::string& unit = {}) const
    {
        const auto it = mod_blocks_.find(unit);
        return it == mod_blocks_.end() ? ModuleStateInfo{} : ModuleStateInfo{it->second.layout, it->second.slot};
    }
    void* thunk(const std::string& name) const { return registry_.thunk(name); }
    // The copy of the record of a class as load_aot registered it (before any reload changed the
    // canonical record), or null. A revert to the AOT code uses it.
    // Stages the AOT code of a unit as a new generation (a revert of a run-time override). Like
    // request_reload it only validates and returns Pending; commit_pending commits it.
    // The new generation number is active_generation + 1, so the check "the generation is newer"
    // of request_reload needs no bypass. The classes get the copies of their AOT records
    // (aot_record). The dispatch table keeps its size: the slots that the override added point to
    // the removed class handler. The live objects migrate back by field name and type: a field
    // that the AOT layout lacks is dropped, as in a normal reload. The live flag of the AOT code
    // goes to 1 and the flag of the override goes to 0 at the commit.
    // It fails with BadModule when the unit has no AOT module, and with Busy when a reload is
    // pending.
    // "backend" is required. The commit of the revert calls Backend::revert_slots with the AOT
    // slot lines of the classes of the unit only, so the next override of the unit sees the AOT
    // signatures (the backend learns an override only in on_accept). The backend must live until
    // the commit. A failed revert does not call it.
    ReloadResult revert_unit(const std::string& unit, Backend& backend);
    const ClassRecord* aot_record(const std::string& name) const
    {
        const auto it = aot_records_.find(name);
        return it == aot_records_.end() ? nullptr : &it->second;
    }

private:
    explicit Loader(bool host_thread_safe);
    struct Pending;
    struct Gen
    {
        uint32_t generation = 0;
        GenState state = GenState::Active;
        Module module; // empty after the unload
        uint64_t retired_frame = 0; // the frame counter at the retirement
        uint32_t registry_refs = 0; // classes and thunks that this module registered
    };
    // Puts the saved copies of the AOT records back into the canonical records.
    void restore_aot_records();
    // The body of load() for one unit. It does not check that the loader is empty.
    // "code_out" gets a copy of the registry of the module (load_aot keeps it).
    ReloadResult load_unit(Module module, Registry* code_out = nullptr);
    // The validation of request_reload for a Pending with a staged registry. It takes the mutex.
    ReloadResult validate_and_stage(std::unique_ptr<Pending> p);
    void add_generation(Module module, uint32_t generation);
    void note_records(const Registry& reg, uint32_t generation);
    // Makes the module state block of a registry and sets the slot. False: an initializer threw.
    bool make_module_block(const std::string& unit, const ModuleStateInfo& info, std::string& error);
    // The number of live objects for each module (the module that made the object).
    // Several units can share a generation number (the AOT units are all generation 1), so a
    // module is the pair (unit, generation).
    using ModuleKey = std::pair<std::string, uint32_t>;
    std::map<ModuleKey, uint32_t> live_by_module() const;
    uint32_t code_refs(const Gen& g, const std::map<ModuleKey, uint32_t>& live) const;
    // True when a live state block (an object or the module block) has a field with kMayHoldCode.
    bool state_holds_code() const;
    // Makes the module (and its core) of a unit live: the live flags of the older module of the
    // unit go to 0, the flags of the new module go to 1. A null flag (an old library) is skipped.
    void switch_live(const std::string& unit, const Registry& code, const Registry* core);
    // Sets the signature key of the slot of a keyed thunk in the canonical record (design 5.3).
    void set_slot_key(const ThunkEntry& t);
    // The commit itself, after the synchronization of the mode.
    ReloadResult commit_locked();

    mutable std::mutex mutex_; // see "Threads" above
    Registry registry_;
    std::vector<Module> cores_; // the core modules; the destructor removes them after "gens_"
    std::vector<Gen> gens_; // the loader owns all modules; a retired one stays until it unloads
    // The generation of the module that holds a record. The unit of the module is the unit of the
    // class (unit_of_).
    std::unordered_map<const ClassRecord*, uint32_t> record_gen_;
    uint64_t frame_ = 0;
    std::unique_ptr<Pending> pending_;
    // The dispatch tables of the removed classes (design 6.1), by class name. The record of a
    // removed class keeps its place in "registry_" while objects live.
    std::unordered_map<std::string, std::vector<SlotFn>> removed_;
    // The live module state block of a unit (module-level script variables). A unit without a
    // block has no entry. "slot" is the slot variable of the module that made the layout. It points
    // to the block. The empty unit (key "") is the unit of a module without a unit name.
    struct ModBlock
    {
        const StateLayout* layout = nullptr;
        void** slot = nullptr;
        char* block = nullptr;
    };
    std::unordered_map<std::string, ModBlock> mod_blocks_;
    // The unit of each class, from the first registration of the class. A reload of a unit
    // removes only the missing classes of this unit. A class never changes its unit.
    std::unordered_map<std::string, std::string> unit_of_;
    std::unordered_map<std::string, ClassRecord> aot_records_; // the AOT records, by class name
    // True after load_aot of an optimized table (AotTable::optimized). Then the loader refuses
    // request_reload, revert_unit and load_unit. Reset clears it.
    bool aot_optimized_ = false;
    std::string aot_slots_; // AotTable::slots of load_aot: the slot manifest of the AOT scripts
    // The AOT modules by unit, for revert_unit: the register function and the registry that the
    // function filled (the records in it are the canonical records, so it is not a copy of them).
    struct AotUnit
    {
        void (*register_fn)(Registry&) = nullptr;
        Registry reg;
    };
    std::map<std::string, AotUnit> aot_units_;
    // The records and the slot tables that revert_unit stages. record_gen_ and the canonical
    // records can point to them until reset.
    std::deque<ClassRecord> revert_records_;
    std::deque<std::vector<SlotFn>> revert_slots_;
    // The live flags (Registry::live_flag) of the live module and its core, by unit.
    std::unordered_map<std::string, std::vector<uint32_t*>> unit_live_;
    // The signature keys of the slots (ClassRecord::slot_keys), by class name. A class gets a
    // table when a reload changes the signature of one of its script functions (design 5.3).
    std::unordered_map<std::string, std::vector<const char*>> slot_keys_;
    std::set<std::string> key_text_; // the text of the keys; a module can unload
};

} // namespace hot
