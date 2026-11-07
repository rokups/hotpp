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
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hot
{

// One import of an AOT script: the unit name of the imported script and the interface hash that
// the importer compiled against (Module::imports).
struct AotImport
{
    const char* unit;
    uint64_t hash;
};

// One script of the AOT table. Each script is one unit. The unit name is "name".
struct AotScript
{
    const char* name; // the file name of the script without the extension
    void (*register_fn)(Registry&); // the register function that hotc generated
    uint64_t iface_hash = 0; // the interface hash of the script (Module::iface_hash)
    const AotImport* imports = nullptr; // the direct imports of the script
    size_t import_count = 0;
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

    // The entry of the class, or null.
    const ClassEntry* find(const std::string& class_name) const { return registry_.find(class_name); }

    // Like create. "Native" is the native base of the class, for example Actor. The object
    // must have that base. Without "args" the factory makes the object. With "args" the
    // constructor that matches the decayed types of "args" makes it (ClassEntry::construct).
    // Throws std::runtime_error for an unknown class, and std::invalid_argument when no
    // constructor matches.
    template <class Native, class... Args> Native* create_as(const std::string& class_name, Args&&... args) const
    {
        if constexpr (sizeof...(Args) == 0)
            return static_cast<Native*>(create(class_name).native);
        else
        {
            const ClassEntry* e = find(class_name);
            if (!e)
                throw std::runtime_error("no AOT script class " + class_name);
            return static_cast<Native*>(e->construct(std::forward<Args>(args)...).native);
        }
    }

    // The stable function address for a native delegate (design 4.11), or null.
    void* thunk(const std::string& name) const { return registry_.thunk(name); }

    size_t class_count() const { return registry_.entries().size(); }
    size_t script_count() const { return script_count_; }

private:
    void link_parents();
    void unlink();
    static ClassRecord* resolve_class(const char* name);
    static bool resolve_unit(const char* name, UnitInfo& out);

    Registry registry_;
    std::vector<ModuleStateInfo> module_blocks_;
    // The units of the scripts: the free functions and the module block (cross-module access).
    struct Unit
    {
        std::unique_ptr<FnTable> fns;
        ModuleStateInfo block;
    };
    std::map<std::string, Unit> units_;
    // A script parent in another unit: the linked slot table of the child and the record values
    // before the link. The destructor puts them back, because the records are constant data.
    struct Linked
    {
        std::vector<SlotFn> slots;
        const SlotFn* own_slots = nullptr;
    };
    std::map<std::string, Linked> linked_;
    // The tables of the interface methods (ClassRecord::iface_slots), by class name. The records
    // point into them, so the destructor clears the pointers first.
    std::map<std::string, std::vector<uint32_t>> iface_slots_;
    size_t script_count_ = 0;
};

} // namespace hot
