// Records of the script runtime: the field table, the state layout and the class record.
#pragma once
#include <cstdint>

namespace spp {

struct ShellBase;
struct StateLayout;
class OldState;

// Tag type. It marks the constructor of a shell that the script runtime makes.
struct Construct {};
// Tag type. A derived shell (design 4.5) gives it to the shell of its script parent. That
// constructor builds the native base only. The derived shell makes the state block.
struct ConstructChild {};

enum FieldFlags : uint8_t {
    kTriviallyCopyable = 1,
    kNothrowMove = 2,
    kScriptStruct = 4,
};

// One row of the reflection table of a state struct (section 4.7).
struct FieldInfo {
    const char* name;      // stable field identifier
    const char* alias;     // previous name from [alias("...")], or null
    const char* type_key;  // mangled C++ type name of the field
    uint32_t offset, size, align;
    uint8_t flags;
    void (*init)(void* self, void* field);     // runs the field initializer
    void (*move)(void* dst, const void* src);  // move-construct
    void (*destroy)(void* field);              // null for a trivial type
    // Optional convert hook for a change of the field type. "convert_from" is the type key
    // of the parameter. The hook runs when the old field has that type key. "dst" is raw
    // memory. The hook constructs the new field there. Both members are null without a hook.
    const char* convert_from = nullptr;
    void (*convert)(void* dst, const void* src) = nullptr;
    // The layout of a [value] script class stored inline in this field (design 5.3), else
    // null. The flag kScriptStruct is set then. The migration recurses into the layout.
    const StateLayout* sub = nullptr;
};

// Size, alignment and fields of the state struct of one generation.
struct StateLayout {
    const char* name;
    uint32_t size, align;
    uint32_t field_count;
    const FieldInfo* fields;
};

// Runtime record of a script class. It lives as long as the session.
struct ClassRecord {
    const char* name;
    uint32_t generation;
    void** slots;  // dispatch table: one entry for each slot
    uint32_t slot_count;
    const StateLayout* layout;
    ClassRecord* super;
    const char* base_name;  // name of the native base class
    uint64_t shell_hash;    // hash of the shell layout (see shell_layout_hash)
    // The onReload hook of the script class, or null. The commit calls it after the migration.
    void (*on_reload)(ShellBase* self, const OldState& old) = nullptr;
    // Script constructors (design 7.3). The first record of a class keeps "construct" and
    // "construct_key". They live in the module that made the shell.
    //  - construct: makes a shell and gives it the base arguments. "args" points to a
    //    std::tuple of the decayed types of the "super(...)" arguments. Null when no code
    //    of the script builds the class with "new" and the class has no constructor.
    //  - construct_key: typeid(tuple).name() of those arguments in the generation that
    //    made "construct". A later generation that changes the types cannot build the class.
    //  - ctor_default: runs the constructor with default values on a new shell. The
    //    factory of the engine calls it. A commit copies it from the new record. Null: no
    //    constructor.
    //  - canonical: set by a commit in a record of a later generation. It points to the
    //    first record of the class.
    ShellBase* (*construct)(const void* args) = nullptr;
    const char* construct_key = nullptr;
    void (*ctor_default)(ShellBase* self) = nullptr;
    ClassRecord* canonical = nullptr;
    // The script destructor, or null. destroy_state calls it before it destroys the fields.
    // A commit copies it from the new record, so the current generation runs at destruction.
    void (*dtor)(ShellBase* self) = nullptr;
};

}  // namespace spp
