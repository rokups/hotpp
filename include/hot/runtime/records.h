// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Records of the script runtime: the field table, the state layout and the class record.
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <typeinfo>

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
struct Instance;

// The decayed type of a constructor parameter (design 7.3). It removes the reference and the
// top-level const and volatile. A type alias is the same type, so it gives the same key.
template <class P> using ctor_param_t = typekey_t<std::remove_reference_t<P>>;

// The key type of a constructor signature. It is an empty class, so a parameter can be a
// reference to an incomplete type. The key is typeid(CtorSignature<decayed parameters>).name().
template <class... P> struct CtorSignature
{
};

// The signature key of a constructor with the parameters "P". The key of the host arguments and
// the key of the script constructor are equal when the decayed types are equal. Compare keys
// with std::strcmp: a type_info address or hash_code can be different in two modules.
template <class... P> inline const char* ctor_key()
{
    return typeid(CtorSignature<ctor_param_t<P>...>).name();
}

// Bit i is set when parameter i can change its argument: a "T&" or a "T&&" of a type that is not
// const. Such a parameter does not accept a const argument of the host.
template <class... P> constexpr uint64_t ctor_write_mask()
{
    uint64_t mask = 0, bit = 1;
    ((mask |= (std::is_reference_v<P> && !std::is_const_v<std::remove_reference_t<P>>) ? bit : 0, bit <<= 1), ...);
    return mask;
}

// Gives the argument "p" of a constructor thunk to a parameter of type "P". "p" points to an
// object of type ctor_param_t<P>. A "T&&" parameter moves the object. A value parameter copies
// it, or moves it when the type cannot be copied.
template <class P> constexpr decltype(auto) ctor_arg(void* p)
{
    using D = ctor_param_t<P>;
    D& v = *static_cast<D*>(p);
    if constexpr (std::is_rvalue_reference_v<P>)
        return static_cast<D&&>(v);
    else if constexpr (std::is_lvalue_reference_v<P> || std::is_copy_constructible_v<D>)
        return static_cast<D&>(v);
    else
        return static_cast<D&&>(v);
}

// One script constructor in the constructor table of a class (design 7.3).
//  - key: ctor_key<parameters>(). It is unique in the table.
//  - make: makes the object with the constructor. "args[i]" points to an object of type
//    ctor_param_t of parameter i. The array has one entry for each parameter.
//  - write_mask: ctor_write_mask<parameters>().
// The table ends with an entry whose key is null.
struct CtorEntry
{
    const char* key;
    Instance (*make)(void** args);
    uint64_t write_mask;
};

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
// Tag type. It marks the clone constructor of a shell. The constructor builds a shell that has a
// copy of the state of another object (design 5.3, [value]). It does not run a script constructor.
struct CloneFrom
{
};
// The key of the clone constructor in ClassRecord::construct and ClassRecord::construct_keys. The
// "args" of "construct" point to a "const ShellBase*": the object to copy. The text is the same in
// all modules. The compare uses strcmp.
inline constexpr const char* kCloneKey = "hot::CloneFrom";

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
    // Copy-constructs the field in raw memory "dst" from the field "src". Null when the type
    // cannot be copied (hot::field_copy in state.h). The copy of a [value] object uses it.
    void (*copy)(void* dst, const void* src) = nullptr;
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
    // The script attributes of the class: the names of the tags "[Name]" on the class, in source
    // order, ending with nullptr. Null: no attribute. The text lives in the module of the record.
    // A commit copies the pointer into the canonical record.
    const char* const* attributes = nullptr;
    // The constructor table of the class: one entry for each script constructor, ending with
    // a null key. Null: no table, for example a removed class. A commit copies it from the
    // new record, so the constructors of the current generation run. ClassEntry::construct
    // uses it.
    const CtorEntry* ctors = nullptr;
};

} // namespace hot
