// The factory of a script class and the plan that migrates the state of a live object.
#pragma once
#include "spp/runtime/records.h"
#include "spp/runtime/shell.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spp
{

// A new object. "native" is the Actor* (the native base) of the shell.
struct Instance
{
    void* native;
    ShellBase* shell;
};
using Factory = Instance (*)();

struct ClassEntry
{
    ClassRecord* record;
    Factory create;
};

// What the migration does for one field of the new layout (section 4.7).
enum class FieldAction : uint8_t
{
    Move, // copy or move the old field "old_index"
    Convert, // the type changed: run the convert hook of the new field on the old field
    Init, // run the initializer
    Nested, // a [value] script class in both layouts: migrate its fields (design 5.3)
};

struct MigrationPlan;

struct FieldStep
{
    FieldAction action;
    uint32_t old_index; // valid for Move, Convert and Nested
    // The plan of the fields of the value class (Nested only).
    std::shared_ptr<MigrationPlan> nested = nullptr;
};

struct MigrationPlan
{
    std::vector<FieldStep> steps; // one step for each new field, in order
    std::vector<uint32_t> removed; // old fields that nothing receives
    std::vector<uint32_t> changed; // old fields whose type changed (converted or lost)
    std::vector<std::string> warnings; // data loss that the reload accepts; the commit logs them
};

// Readable text for the log (design 6.6). "type_key" is a mangled type name from typeid, for
// example "i". The result is the C++ name, for example "int". Unknown text stays as it is.
std::string readable_type(const char* type_key);
// The class name of a state layout name. The generated name "HeroState_g2" gives "Hero".
std::string readable_layout(const char* layout_name);

// Compares the old and the new field tables. It has no side effects. A type change never
// rejects the reload: it converts the value (hook) or it uses the default value (warning).
MigrationPlan build_plan(const StateLayout& old_layout, const StateLayout& new_layout);

} // namespace spp
