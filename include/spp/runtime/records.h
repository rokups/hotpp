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
    // A value of the field type can point into the code of the module that made it: a vtable or
    // a function pointer. Examples: the control block of a std::shared_ptr, a std::function.
    // While a live state block has such a field, no retired generation unloads (design 4.9).
    kMayHoldCode = 8,
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
    // The names of the native bases: the primary base, then the interface bases, with ", "
    // between them (design 4.4). For example "Actor, IDamageable".
    const char* base_name;
    uint64_t shell_hash;    // hash of the shell layout (see shell_layout_hash)
    // The onReload hook of the script class, or null. The commit calls it after the migration.
    void (*on_reload)(ShellBase* self, const OldState& old) = nullptr;
    // Script constructors (design 7.3). The canonical record of a class keeps "construct" and
    // "construct_key". They live in the module that made the shell: the core module (module.h),
    // or the first generation of the class when there is no core.
    //  - construct: makes a shell and gives it the base arguments. "args" points to a
    //    std::tuple of the decayed types of the "super(...)" arguments. "key" is
    //    typeid(tuple).name(). Each constructor overload of the first generation gives a
    //    known key. An unknown key gives nullptr: a later generation changed the types.
    //    Each script shell class has it (S20a). Null for a record without a script shell.
    //  - construct_key: the key of the first overload. Information only.
    //  - new_default: makes a new object with default constructor arguments. It evaluates
    //    the "super(...)" arguments one time, makes the shell through "construct" and runs
    //    the constructor chain. The factory of the engine calls it. A commit copies it from
    //    the new record, so the current generation runs. Null: no constructor.
    //  - canonical: set by the loader in each generation record that is not canonical. It
    //    points to the canonical record of the class: the record of the core module, or the
    //    first record of the class when there is no core.
    ShellBase* (*construct)(const char* key, const void* args) = nullptr;
    const char* construct_key = nullptr;
    ShellBase* (*new_default)() = nullptr;
    ClassRecord* canonical = nullptr;
    // The script destructor, or null. destroy_state calls it before it destroys the fields.
    // A commit copies it from the new record, so the current generation runs at destruction.
    void (*dtor)(ShellBase* self) = nullptr;
    // The signature key of each slot (typeid(Sig).name()), or null (design 5.3). The loader
    // sets it when a reload changes the signature of a script function. spp::call compares
    // the key with its own signature. A call with another signature detaches: it does not run.
    const char* const* slot_keys = nullptr;
    // True when the shell derives from std::enable_shared_from_this (design 7.10.1): the class
    // uses "this" as a handle or has script children. The bit changes the shell layout.
    bool shares_this = false;
};

}  // namespace spp
