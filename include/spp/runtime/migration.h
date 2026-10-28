// The factory of a script class and the plan that migrates the state of a live object.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "spp/runtime/records.h"
#include "spp/runtime/shell.h"

namespace spp {

// A new object. "native" is the Actor* (the native base) of the shell.
struct Instance {
    void* native;
    ShellBase* shell;
};
using Factory = Instance (*)();

struct ClassEntry {
    ClassRecord* record;
    Factory create;
};

// What the migration does for one field of the new layout (section 4.7).
enum class FieldAction : uint8_t {
    Move,  // copy or move the old field "old_index"
    Init,  // run the initializer
};

struct FieldStep {
    FieldAction action;
    uint32_t old_index;  // valid for Move
};

struct MigrationPlan {
    std::vector<FieldStep> steps;        // one step for each new field, in order
    std::vector<uint32_t> removed;       // old fields that nothing receives
    std::string error;                   // not empty: the reload is rejected (type change)
};

// Compares the old and the new field tables. It has no side effects.
MigrationPlan build_plan(const StateLayout& old_layout, const StateLayout& new_layout);

}  // namespace spp
