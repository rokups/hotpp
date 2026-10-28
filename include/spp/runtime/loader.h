// The loader of generation modules: validation, commit and unload.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

struct LoaderStats {
    uint32_t active_generation = 0;
    std::vector<GenerationInfo> generations;  // in load order
    size_t live_objects = 0;
};

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
// The first record of a class stays the record of the class for ever. A
// commit copies the slots and the layout of the new record into it.
// A thunk keeps the address of the first registration (append-only).
//
// The loader owns every Module. A generation that a reload replaced is
// retired. frame_boundary() unloads a retired generation when all of these hold:
//   1. its pin count is zero;
//   2. at least one frame boundary passed after the commit that retired it;
//   3. no code of the module is in use (code_refs is zero).
// The record, the factory and the thunks of a class stay in the module that
// registered the class first. The vtable of an object stays in the module that
// made the object. Condition 3 keeps those modules loaded.
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

    const ClassEntry* find(const std::string& name) const { return registry_.find(name); }
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
    // The number of live objects for each module (the module that made the object).
    std::unordered_map<uint32_t, uint32_t> live_by_module() const;
    uint32_t code_refs(const Gen& g, const std::unordered_map<uint32_t, uint32_t>& live) const;

    mutable std::mutex mutex_;  // see "Threads" above
    Registry registry_;
    std::vector<Gen> gens_;  // the loader owns all modules; a retired one stays until it unloads
    std::unordered_map<const ClassRecord*, uint32_t> record_gen_;  // the module that holds a record
    uint64_t frame_ = 0;
    std::unique_ptr<Pending> pending_;
};

}  // namespace spp
