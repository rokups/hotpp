// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// AOT compilation (design M7). The generated C++ of all scripts is part of the game binary.
// There is no loader, no reload, no JIT and no Clang at run time.
//
// "hotc --aot" writes the C++ of each script and one registration file. The registration
// file defines the constant table hot::aot_table. The table lists the register function
// of each script. A script registers its classes through the same Registry calls as a
// generation module. The class records, the slot tables and the factories are constant data
// of the binary.
//
// A program with AOT scripts and no loader links hot_aot (the core of the runtime) and the
// static library of the scripts. It must not link hot_runtime (loader, migration, dlopen).
// A program with a loader can use the same scripts: Loader::load_aot takes the table. The
// library of the scripts then links only hot::runtime_core.
#pragma once
#include "hot/runtime/migration.h"
#include "hot/runtime/records.h"
#include "hot/runtime/registry.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hot
{

// One script of the AOT table.
struct AotScript
{
    const char* name; // the file name of the script without the extension
    void (*register_fn)(Registry&); // the register function that hotc generated
};

struct AotTable
{
    const AotScript* scripts;
    size_t count;
    // The slot and signature manifests of all scripts as one text (the lines "Cls::fn gen" and
    // "sig Cls::fn Sig"). A run-time backend takes it through Backend::seed_slots, so a later
    // generation keeps the slot numbers of the AOT code. It can be null (an older table).
    const char* slots = nullptr;
    // True for optimized AOT code (hotc --aot-optimized): the scripts have no slots, so the
    // loader cannot override or reload them.
    bool optimized = false;
};

// The table that the registration file of "hotc --aot" defines. A program that does not
// use AOT scripts does not refer to it.
extern const AotTable aot_table;

// Registers the classes of all scripts of a table at start-up. The registry lives as long as
// the program uses script objects. The constructor and the destructor run on the main thread.
class AotRegistry
{
public:
    // Calls the register function of each script. It makes the module variable blocks.
    // It throws std::runtime_error when two scripts have a class of the same name or
    // an initializer of a module variable throws.
    explicit AotRegistry(const AotTable& table);
    ~AotRegistry();
    AotRegistry(const AotRegistry&) = delete;
    AotRegistry& operator=(const AotRegistry&) = delete;

    bool has(const std::string& class_name) const { return registry_.find(class_name) != nullptr; }
    // The names of all script classes, sorted.
    std::vector<std::string> class_names() const;
    const ClassRecord* record(const std::string& class_name) const;

    // Makes an object with the factory of the class (the constructor runs with default
    // arguments). The caller owns the object, or gives it to the engine, for example
    // World::Add. Throws std::runtime_error for an unknown class.
    Instance create(const std::string& class_name) const;

    // Like create. "Native" is the native base of the class, for example Actor. The object
    // must have that base.
    template <class Native> Native* create_as(const std::string& class_name) const
    {
        return static_cast<Native*>(create(class_name).native);
    }

    // The stable function address for a native delegate (design 4.11), or null.
    void* thunk(const std::string& name) const { return registry_.thunk(name); }

    size_t class_count() const { return registry_.entries().size(); }
    size_t script_count() const { return script_count_; }

private:
    Registry registry_;
    std::vector<ModuleStateInfo> module_blocks_;
    size_t script_count_ = 0;
};

} // namespace hot
