// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The state block of a script object, the reflection helpers and the field access.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
    // This header declares _aligned_malloc and _aligned_free.
    #include <malloc.h>
#endif

#include "hot/runtime/hidden.h"
#include "hot/runtime/records.h"
#include "hot/runtime/shell.h"

namespace hot
{

// The runtime keeps a list of the live objects. The reload commit uses it.
HOT_EXPORT void track_object(ShellBase* self);
HOT_EXPORT void untrack_object(ShellBase* self);

// An aligned block and its free function. A block from aligned_alloc must go to aligned_free.
// The "size" must be a multiple of "align" for std::aligned_alloc.
HOT_HIDDEN void* aligned_alloc(size_t align, size_t size)
{
#ifdef _WIN32
    // Windows has no std::aligned_alloc. Every block comes from _aligned_malloc.
    return _aligned_malloc(size, align);
#else
    // malloc gives this alignment already, and it is faster than aligned_alloc. std::free
    // frees both kinds of block.
    if (align <= alignof(std::max_align_t))
        return std::malloc(size);
    return std::aligned_alloc(align, size);
#endif
}

HOT_HIDDEN void aligned_free(void* p)
{
#ifdef _WIN32
    // A block from _aligned_malloc goes to _aligned_free.
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// Allocates a state block for a layout. Returns null when memory is out.
HOT_HIDDEN char* alloc_state(const StateLayout* l)
{
    // "align" comes from alignof, so it is a power of two: a mask rounds up with no division.
    size_t align = l->align;
    size_t size = (l->size + align - 1) & ~(align - 1);
    if (size == 0)
        size = align;
    return static_cast<char*>(hot::aligned_alloc(align, size));
}

// Destroys the fields in reverse order. It does not free the block.
HOT_HIDDEN void destroy_fields(const StateLayout* l, char* mem)
{
    for (uint32_t i = l->field_count; i-- > 0;)
    {
        const FieldInfo& f = l->fields[i];
        if (f.destroy)
            f.destroy(mem + f.offset);
    }
}

// Writes the owner shell into the first slot of a block. The layout must say has_owner.
HOT_HIDDEN void set_owner(const StateLayout* l, char* mem, ShellBase* self)
{
    if (l->has_owner)
        *reinterpret_cast<ShellBase**>(mem) = self;
}

// Makes the state block of a new object and runs the field initializers.
HOT_NODEBUG inline void init_state(ShellBase* self, ClassRecord* cls)
{
    // A record of a later core module can make the shell (S27). The object uses the canonical record.
    if (cls->canonical)
        cls = cls->canonical;
    self->cls = cls;
    const StateLayout* l = cls->layout;
    char* mem = alloc_state(l);
    if (!mem)
        throw std::bad_alloc();
    set_owner(l, mem, self);
    uint32_t i = 0;
    try
    {
        for (; i < l->field_count; ++i)
        {
            const FieldInfo& f = l->fields[i];
            if (f.init)
                f.init(mem, mem + f.offset);
        }
    }
    catch (...)
    { // an initializer threw: destroy the fields that it made before, in reverse order, and free the block
        while (i-- > 0)
        {
            const FieldInfo& f = l->fields[i];
            if (f.destroy)
                f.destroy(mem + f.offset);
        }
        hot::aligned_free(mem);
        throw;
    }
    self->state = mem;
    track_object(self);
}

// Makes the state block of a new object as a copy of the state of the object "src". The new
// object has the class of "cls" (the canonical record). A field is copied with FieldInfo::copy, or
// with a byte copy when the field is trivially copyable. A field with no copy function throws
// std::runtime_error. A throw from a copy destroys the fields that exist and frees the block.
// The script constructor does not run.
HOT_NODEBUG inline void clone_state(ShellBase* self, ClassRecord* cls, const ShellBase* src)
{
    if (cls->canonical)
        cls = cls->canonical;
    self->cls = cls;
    ScriptEntry entry; // design 6.3: a commit must not migrate the source during the copy
    const StateLayout* l = cls->layout;
    const char* from = static_cast<const char*>(src->state);
    if (!from)
        throw std::runtime_error(std::string("class ") + cls->name + ": the source object has no state");
    char* mem = alloc_state(l);
    if (!mem)
        throw std::bad_alloc();
    set_owner(l, mem, self);
    uint32_t i = 0;
    try
    {
        for (; i < l->field_count; ++i)
        {
            const FieldInfo& f = l->fields[i];
            if (f.flags & kTriviallyCopyable)
                std::memcpy(mem + f.offset, from + f.offset, f.size);
            else if (f.copy)
                f.copy(mem + f.offset, from + f.offset);
            else
                throw std::runtime_error(
                    std::string("class ") + cls->name + " is not copyable: field '" + f.name + "'");
        }
    }
    catch (...)
    { // a copy threw: destroy the fields that exist, in reverse order, and free the block
        while (i-- > 0)
        {
            const FieldInfo& f = l->fields[i];
            if (f.destroy)
                f.destroy(mem + f.offset);
        }
        hot::aligned_free(mem);
        throw;
    }
    self->state = mem;
    track_object(self);
}

// Destroys the fields in reverse order and frees the state block.
HOT_NODEBUG inline void destroy_state(ShellBase* self)
{
    if (!self->state)
        return;
    ScriptEntry entry; // design 6.3: a commit must not migrate the object during the destructor
    if (self->cls->dtor)
    { // the script destructor of the current generation
        try
        {
            self->cls->dtor(self);
        }
        catch (...)
        {
            std::fprintf(stderr, "[script] ERROR: the destructor of %s threw\n", self->cls->name);
        }
    }
    untrack_object(self);
    char* mem = static_cast<char*>(self->state);
    destroy_fields(self->cls->layout, mem);
    hot::aligned_free(mem);
    self->state = nullptr;
}

// Optimized AOT code keeps the fields in the shell. The type "T" of a field names a type for the
// brace initializer of the member, also when the type is a pointer type.
template <class T> using field_id_t = T;

// The value of a field initializer in optimized AOT code. The shell has the field as a member, so
// the initializer is a prvalue. The check refuses a conversion that only a C-style cast accepts.
template <class T, class... A> HOT_HIDDEN constexpr T field_init(A&&... a)
{
    static_assert(std::is_constructible_v<T, A&&...>, "the field initializer does not fit the type of the field");
    return T(std::forward<A>(a)...);
}

// Optimized AOT code: sets the record of a new object. The fields are members of the shell, so
// the C++ constructors have made them already.
HOT_NODEBUG inline void init_object(ShellCore* self, ClassRecord* cls) noexcept
{
    self->cls = cls;
}

// Optimized AOT code: runs the script destructor of an object. Each shell calls it in its own
// destructor, before the fields of that shell die. Only the shell of the class of the object
// (the record "own") runs the script destructor. A shell of a parent class does nothing.
HOT_NODEBUG inline void finish_object(ShellCore* self, const ClassRecord* own) noexcept
{
    if (self->cls != own || !own->dtor)
        return;
    try
    {
        own->dtor(self);
    }
    catch (...)
    {
        std::fprintf(stderr, "[script] ERROR: the destructor of %s threw\n", own->name);
    }
}

// True when T is a raw code pointer: a function pointer, a member function pointer, or an
// array of them. A code pointer can point into a generation module (design 4.9).
template <class T>
inline constexpr bool is_code_pointer_v =
    (std::is_pointer_v<std::remove_all_extents_t<T>>
        && std::is_function_v<std::remove_pointer_t<std::remove_all_extents_t<T>>>)
    || std::is_member_function_pointer_v<std::remove_all_extents_t<T>>;

// True when the translator found a function pointer in a field of the native struct T (S26).
// The translator specializes it to true_type in the generated code. The default is false.
template <class T> struct native_holds_code : std::false_type
{
};

// True when a value of T cannot point into the code of the module that made it (no vtable, no
// function pointer). A trivially copyable type is not polymorphic. A raw function pointer and a
// std::function hold code. The standard containers with the default allocator, hash and order
// are free of code when their elements are. A trivially copyable native struct with a function
// pointer field holds code: the translator marks it with native_holds_code. An engine can add a
// specialization of holds_no_code for a type that the translator cannot see.
template <class T>
struct holds_no_code
    : std::bool_constant<std::is_trivially_copyable_v<T> && !is_code_pointer_v<T> && !native_holds_code<T>::value>
{
};
template <class Sig> struct holds_no_code<std::function<Sig>> : std::false_type
{
};
template <class C, class Tr> struct holds_no_code<std::basic_string<C, Tr, std::allocator<C>>> : std::true_type
{
};
template <class T> struct holds_no_code<std::vector<T, std::allocator<T>>> : holds_no_code<T>
{
};
template <class T, size_t N> struct holds_no_code<std::array<T, N>> : holds_no_code<T>
{
};
template <class T> struct holds_no_code<std::optional<T>> : holds_no_code<T>
{
};
template <class A, class B>
struct holds_no_code<std::pair<A, B>> : std::bool_constant<holds_no_code<A>::value && holds_no_code<B>::value>
{
};
template <class K, class V>
struct holds_no_code<std::map<K, V, std::less<K>, std::allocator<std::pair<const K, V>>>>
    : std::bool_constant<holds_no_code<K>::value && holds_no_code<V>::value>
{
};
template <class K, class V>
struct holds_no_code<std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, std::allocator<std::pair<const K, V>>>>
    : std::bool_constant<holds_no_code<K>::value && holds_no_code<V>::value>
{
};
template <class T> struct holds_no_code<const T> : holds_no_code<T>
{
};
// A C array holds code when its element does.
template <class T, size_t N> struct holds_no_code<T[N]> : holds_no_code<T>
{
};
template <class T, size_t N> struct holds_no_code<const T[N]> : holds_no_code<T>
{
};

// True for hot::Value<T>, the box of a [value] field (value.h specializes it). The migration of
// a state shares the object of a box. It does not copy the object, because the object is a live
// object that the loader migrates by itself.
template <class T> struct is_value_box : std::false_type
{
};

// Helpers for the reflection table of generated code. T is the field type.
// True when a copy of T compiles. The test goes through the element types: the copy constructor
// of std::vector<std::unique_ptr<X>> exists, but its body does not compile. A container has a
// "value_type" (std::vector, std::map, std::optional, std::array). A pair, a tuple and a
// variant have their own elements.
template <class T, class = void> struct deep_copyable : std::is_copy_constructible<T>
{
};

template <class T> struct deep_copyable<T, std::void_t<typename T::value_type>>
    : std::bool_constant<std::is_copy_constructible_v<T> && deep_copyable<std::remove_cv_t<typename T::value_type>>::value>
{
};

template <class A, class B> struct deep_copyable<std::pair<A, B>, void>
    : std::bool_constant<std::is_copy_constructible_v<std::pair<A, B>> && deep_copyable<std::remove_cv_t<A>>::value
          && deep_copyable<std::remove_cv_t<B>>::value>
{
};

template <class... Ts> struct deep_copyable<std::tuple<Ts...>, void>
    : std::bool_constant<std::is_copy_constructible_v<std::tuple<Ts...>>
          && (deep_copyable<std::remove_cv_t<Ts>>::value && ...)>
{
};

template <class... Ts> struct deep_copyable<std::variant<Ts...>, void>
    : std::bool_constant<std::is_copy_constructible_v<std::variant<Ts...>>
          && (deep_copyable<std::remove_cv_t<Ts>>::value && ...)>
{
};

template <class T> inline constexpr bool deep_copyable_v = deep_copyable<T>::value;

// True when a field of type T moves to a new state block: a copy, or for a type that does not
// copy, a move. A C array needs a copy of each element.
template <class T> inline constexpr bool field_movable_v = deep_copyable_v<std::remove_all_extents_t<T>>
    || (!std::is_array_v<T> && std::is_move_constructible_v<T>);

template <class T> HOT_HIDDEN constexpr uint8_t field_flags()
{
    return static_cast<uint8_t>((std::is_trivially_copyable_v<T> ? kTriviallyCopyable : 0)
        | (std::is_nothrow_move_constructible_v<std::remove_all_extents_t<T>> ? kNothrowMove : 0) | (holds_no_code<T>::value ? 0 : kMayHoldCode)
        | (field_movable_v<T> ? 0 : kNotMovable));
}

// The flags of a field that holds a [value] script class (design 5.3). The loader looks at the
// fields of the sub layout for kMayHoldCode, so the flag of the struct itself is not set.
template <class T> HOT_HIDDEN constexpr uint8_t value_field_flags()
{
    return static_cast<uint8_t>((field_flags<T>() & ~kMayHoldCode) | kScriptStruct);
}

// A field can be a C array, for example "int arr[256]" of a ".cpp" script. An array has no copy
// constructor and no destructor call. These helpers work on each element. They take raw
// addresses, so a const element type (a "const int lut[3]" field) works.
template <class T> HOT_NODEBUG inline void copy_into(void* dst, const void* src)
{
    if constexpr (std::is_array_v<T>)
    {
        using E = std::remove_extent_t<T>;
        for (size_t i = 0; i < std::extent_v<T>; ++i)
            copy_into<E>(static_cast<char*>(dst) + i * sizeof(E), static_cast<const char*>(src) + i * sizeof(E));
    }
    else
    {
        using U = std::remove_cv_t<T>;
        ::new (dst) U(*static_cast<const U*>(src));
    }
}

template <class T> HOT_NODEBUG inline void destroy_in(void* obj)
{
    if constexpr (std::is_array_v<T>)
    {
        using E = std::remove_extent_t<T>;
        for (size_t i = std::extent_v<T>; i > 0; --i)
            destroy_in<E>(static_cast<char*>(obj) + (i - 1) * sizeof(E));
    }
    else
    {
        using U = std::remove_cv_t<T>;
        static_cast<U*>(obj)->~U();
    }
}

template <class T> HOT_NODEBUG inline void move_field(void* dst, const void* src)
{
    if constexpr (!field_movable_v<T>)
    {
        // No copy and no move: the flag kNotMovable makes the migration run the initializer.
        (void)dst;
        (void)src;
    }
    else if constexpr (std::is_array_v<T>)
        copy_into<T>(dst, src);
    else if constexpr (is_value_box<T>::value)
        new (dst) T(static_cast<const T*>(src)->handle()); // the same object: no clone
    else if constexpr (deep_copyable_v<T>)
        new (dst) T(*static_cast<const T*>(src));
    else
        new (dst) T(std::move(*const_cast<T*>(static_cast<const T*>(src))));
}

template <class T> HOT_NODEBUG inline void destroy_field(void* field)
{
    destroy_in<T>(field);
}

template <class T> HOT_HIDDEN constexpr void (*field_destroyer())(void*)
{
    if constexpr (std::is_trivially_destructible_v<T>)
        return nullptr;
    else
        return &destroy_field<T>;
}

// The copy function of a field of type T for FieldInfo::copy: it copy-constructs the field in raw
// memory. The result is null when a copy of T does not compile.
template <class T> HOT_HIDDEN constexpr auto field_copy()
{
    if constexpr (deep_copyable_v<std::remove_all_extents_t<T>>)
        return &copy_into<T>;
    else
        return static_cast<void (*)(void*, const void*)>(nullptr);
}

// Reflection helpers.
// An optimized AOT class has no layout: the result is null.
inline const FieldInfo* find_field(const ClassRecord* cls, const char* name)
{
    if (!cls->layout)
        return nullptr;
    for (uint32_t i = 0; i < cls->layout->field_count; ++i)
        if (std::strcmp(cls->layout->fields[i].name, name) == 0)
            return &cls->layout->fields[i];
    return nullptr;
}

template <class T> inline const char* type_key()
{
    return typeid(typekey_t<T>).name();
}

// The type key of a native container of a [value] script class (design 5.3): "type#hash".
// "hash" is the hash of the layout of the [value] classes in the type. The text stays valid
// after the module unloads. A different hash with the same type resets the container.
HOT_NODEBUG HOT_EXPORT const char* layout_keyed(const char* type, const char* hash);

// True when the type key of a field is the typeid name, with or without a layout hash.
inline bool type_key_matches(const char* key, const char* name)
{
    const size_t n = std::strlen(name);
    return std::strncmp(key, name, n) == 0 && (key[n] == '\0' || key[n] == '#');
}

// Returns a pointer to a field, or null when the name or the type does not match.
template <class T> inline T* field_ptr(ShellCore* s, const char* name)
{
    const FieldInfo* f = find_field(s->cls, name);
    if (!f)
        return nullptr;
    // A record with a layout belongs to a tracked ShellBase.
    ShellBase* sb = static_cast<ShellBase*>(s);
    if (!sb->state || !type_key_matches(f->type_key, type_key<T>()))
        return nullptr;
    return reinterpret_cast<T*>(static_cast<char*>(sb->state) + f->offset);
}

} // namespace hot
