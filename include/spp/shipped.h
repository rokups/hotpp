// Shipping mode (design M7). The generated C++ of all scripts is part of the game binary.
// There is no loader, no reload, no JIT and no Clang at run time.
//
// "sppc --ship" writes the C++ of each script and one registration file. The registration
// file defines the constant table spp::shipped_table. The table lists the register function
// of each script. A script registers its classes through the same Registry calls as a
// generation module. The class records, the slot tables and the factories are constant data
// of the binary.
//
// A program that ships scripts links spp_ship (the core of the runtime) and the static
// library of the scripts. It must not link spp_runtime (loader, migration, dlopen).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "spp/runtime/migration.h"
#include "spp/runtime/records.h"
#include "spp/runtime/registry.h"

namespace spp {

// One script of the shipped table.
struct ShippedScript {
    const char* name;                 // the file name of the script without the extension
    void (*register_fn)(Registry&);  // the register function that sppc generated
};

struct ShippedTable {
    const ShippedScript* scripts;
    size_t count;
};

// The table that the registration file of "sppc --ship" defines. A program that does not
// use shipped scripts does not refer to it.
extern const ShippedTable shipped_table;

// Registers the classes of all scripts of a table at start-up. The registry lives as long as
// the program uses script objects. The constructor and the destructor run on the main thread.
class ShippedRegistry {
public:
    // Calls the register function of each script. It makes the module variable blocks.
    // It throws std::runtime_error when two scripts have a class of the same name or
    // an initializer of a module variable throws.
    explicit ShippedRegistry(const ShippedTable& table);
    ~ShippedRegistry();
    ShippedRegistry(const ShippedRegistry&) = delete;
    ShippedRegistry& operator=(const ShippedRegistry&) = delete;

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
    template <class Native>
    Native* create_as(const std::string& class_name) const
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

}  // namespace spp
