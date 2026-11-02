// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The factory of a script class and the plan that migrates the state of a live object.
#pragma once
#include "hot/runtime/records.h"
#include "hot/runtime/shell.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <vector>

namespace hot
{

// A new object. "native" is the Actor* (the native base) of the shell.
struct Instance
{
    void* native;
    ShellCore* shell;
};
using Factory = Instance (*)();

struct ClassEntry;
// The text of each constructor of the class, for example "(), (Urho3D::Context*)".
std::string constructor_list(const ClassEntry& entry);
// Throws std::invalid_argument: the class has no constructor for the key "key" of the host
// arguments, or argument "const_arg" (when it is not -1) is const and the constructor changes it.
// The text names the class and lists its constructors.
[[noreturn]] void throw_no_constructor(const ClassEntry& entry, const char* key, int const_arg = -1);

struct ClassEntry
{
    ClassRecord* record;
    // The factory: makes an object with the default constructor (design 7.3).
    Factory create;

    // The constructor table entry with the signature key "key" (ctor_key), or null.
    const CtorEntry* find_constructor(const char* key) const
    {
        if (record && record->ctors)
            for (const CtorEntry* c = record->ctors; c->key; ++c)
                if (std::strcmp(c->key, key) == 0)
                    return c;
        return nullptr;
    }

    // True when the class has a constructor with the parameters "P" after decay (ctor_key).
    template <class... P> bool has_constructor() const { return find_constructor(ctor_key<P...>()) != nullptr; }

    // Makes an object with the script constructor whose decayed parameter types are the decayed
    // types of the arguments. The match is exact: no conversion, no default argument and no
    // overload resolution. A "T&" parameter refers to the argument. A "T&&" parameter moves
    // from the argument. A value parameter gets a copy. Without arguments and without a "()"
    // constructor, it calls "create". When no constructor matches, it throws
    // std::invalid_argument with the list of the constructors of the class.
    template <class... Args> Instance construct(Args&&... args) const
    {
        const char* key = ctor_key<Args...>();
        const CtorEntry* c = find_constructor(key);
        if (!c)
        {
            if constexpr (sizeof...(Args) == 0)
                return create();
            else
                throw_no_constructor(*this, key);
        }
        constexpr uint64_t host_const = const_mask<Args...>();
        if (const uint64_t bad = c->write_mask & host_const)
        {
            int i = 0;
            while (!(bad & (uint64_t(1) << i)))
                ++i;
            throw_no_constructor(*this, key, i);
        }
        void* ptrs[sizeof...(Args) + 1] = {
            const_cast<void*>(static_cast<const volatile void*>(std::addressof(args)))..., nullptr};
        return c->make(ptrs);
    }

private:
    // Bit i is set when argument i is const.
    template <class... Args> static constexpr uint64_t const_mask()
    {
        uint64_t mask = 0, bit = 1;
        ((mask |= std::is_const_v<std::remove_reference_t<Args>> ? bit : 0, bit <<= 1), ...);
        return mask;
    }
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

} // namespace hot
