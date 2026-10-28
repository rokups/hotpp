// Records of the script runtime: the field table, the state layout and the class record.
#pragma once
#include <cstdint>

namespace script {

// Tag type. It marks the constructor of a shell that the script runtime makes.
struct Construct {};

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
};

}  // namespace script
