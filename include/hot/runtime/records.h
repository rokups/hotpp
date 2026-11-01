// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Records of the script runtime: the field table, the state layout and the class record.
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hot
{

// The type of a type key. The cv-qualifiers of the elements of an array are qualifiers of the
// array, and typeid ignores them. MSVC keeps them in the name, clang does not. A host that MSVC
// compiles and a module that clang compiles must give the same key, so the key removes them.
template <class T> struct typekey
{
    using type = std::remove_cv_t<T>;
};
template <class T, size_t N> struct typekey<T[N]>
{
    using type = typename typekey<T>::type[N];
};
template <class T> struct typekey<T[]>
{
    using type = typename typekey<T>::type[];
};
template <class T> using typekey_t = typename typekey<T>::type;

struct ShellBase;
struct ShellCore;
struct StateLayout;

// An opaque state type. A slot holds a pointer to a member function of the script state. The
// runtime stores it as a member of this type and casts it back at the call (shell.h).
struct StateBase
{
    void* __removed(); // the slot of a removed class (loader.cpp)
};
using SlotFn = void (StateBase::*)();
class OldState;

// Tag type. It marks the constructor of a shell that the script runtime makes.
struct Construct
{
};
// Tag type. A derived shell (design 4.5) gives it to the shell of its script parent. That
// constructor builds the native base only. The derived shell makes the state block.
struct ConstructChild
{
};

enum FieldFlags : uint8_t
{
    kTriviallyCopyable = 1,
    kNothrowMove = 2,
    kScriptStruct = 4,
    // A value of the field type can point into the code of the module that made it: a vtable or
    // a function pointer. Examples: the control block of a std::shared_ptr, a std::function.
    // While a live state block has such a field, no retired generation unloads (design 4.9).
    kMayHoldCode = 8,
    // The field type has no copy constructor and no move constructor (for example std::atomic).
    // The migration cannot carry the value, so the new field runs its initializer again.
    kNotMovable = 16,
};

// One row of the reflection table of a state struct (section 4.7).
struct FieldInfo
{
    const char* name; // stable field identifier
    const char* alias; // previous name from [alias("...")], or null
    const char* type_key; // mangled C++ type name of the field
    uint32_t offset, size, align;
    uint8_t flags;
    void (*init)(void* self, void* field); // runs the field initializer
    void (*move)(void* dst, const void* src); // move-construct
    void (*destroy)(void* field); // null for a trivial type
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
struct StateLayout
{
    const char* name;
    uint32_t size, align;
    uint32_t field_count;
    const FieldInfo* fields;
    // When true, the block starts with a ShellBase* that the runtime writes (set_owner).
    bool has_owner = false;
};

// Runtime record of a script class. It lives as long as the session.
struct ClassRecord
{
    const char* name;
    uint32_t generation;
    const SlotFn* slots; // dispatch table: one entry for each slot
    uint32_t slot_count;
    const StateLayout* layout;
    ClassRecord* super;
    // The names of the native bases: the primary base, then the interface bases, with ", "
    // between them (design 4.4). For example "Actor, IDamageable".
    const char* base_name;
    uint64_t shell_hash; // hash of the shell layout (see shell_layout_hash)
    // The onReload hook of the script class, or null. The commit calls it after the migration.
    void (*on_reload)(ShellBase* self, const OldState& old) = nullptr;
    // Script constructors (design 7.3). The canonical record of a class keeps "construct" and
    // "construct_key". They live in the module that made the shell: the core module (module.h),
    // or the first generation of the class when there is no core.
    //  - construct: makes a shell and gives it the base arguments. "args" points to a
    //    std::tuple of the decayed types of the "base(...)" arguments. "key" is
    //    typeid(tuple).name(). Each constructor overload of the first generation gives a
    //    known key. An unknown key gives nullptr. A later generation with a new key gets a new core
    //    module, and its "construct" replaces this one (S27).
    //    Each script shell class has it (S20a). Null for a record without a script shell.
    //    "mem" null: the shell comes from "new". Else "construct" makes the shell in "mem"
    //    (placement new). Then "mem" has the size and the alignment of the shell, and the caller
    //    frees it (make_shared_shell in shell.h).
    //  - construct_key: the key of the first overload. Information only.
    //  - new_default: makes a new object with default constructor arguments. It evaluates
    //    the "base(...)" arguments one time, makes the shell through "construct" and runs
    //    the constructor chain. The factory of the engine calls it. A commit copies it from
    //    the new record, so the current generation runs. Null: no constructor.
    //  - canonical: set by the loader in each generation record that is not canonical. It
    //    points to the canonical record of the class: the record of the core module, or the
    //    first record of the class when there is no core.
    ShellBase* (*construct)(void* mem, const char* key, const void* args) = nullptr;
    const char* construct_key = nullptr;
    ShellCore* (*new_default)() = nullptr;
    ClassRecord* canonical = nullptr;
    // The script destructor, or null. destroy_state calls it before it destroys the fields.
    // A commit copies it from the new record, so the current generation runs at destruction.
    void (*dtor)(ShellCore* self) = nullptr;
    // The signature key of each slot (typeid(Sig).name()), or null (design 5.3). The loader
    // sets it when a reload changes the signature of a script function. hot::call compares
    // the key with its own signature. A call with another signature detaches: it does not run.
    const char* const* slot_keys = nullptr;
    // True when the shell derives from std::enable_shared_from_this (design 7.10.1): the class
    // uses "this" as a handle or has script children. The bit changes the shell layout.
    bool shares_this = false;
    // The keys that "construct" accepts: the keys of all constructor overloads of the module
    // that made it, ending with nullptr. Null without "construct". The loader compares the keys
    // of a new generation with this list. A new key makes a new core module, and its "construct"
    // replaces the canonical one (S27). The generation record has its own list.
    const char* const* construct_keys = nullptr;
};

} // namespace hot
