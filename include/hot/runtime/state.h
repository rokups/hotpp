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
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _MSC_VER
    // unverified: not built on MSVC. This header declares _aligned_malloc and _aligned_free.
    #include <malloc.h>
#endif

#include "hot/runtime/hidden.h"
#include "hot/runtime/records.h"
#include "hot/runtime/shell.h"

namespace hot
{

// The runtime keeps a list of the live objects. The reload commit uses it.
void track_object(ShellBase* self);
void untrack_object(ShellBase* self);

// An aligned block and its free function. A block from aligned_alloc must go to aligned_free.
// The "size" must be a multiple of "align" for std::aligned_alloc.
HOT_HIDDEN void* aligned_alloc(size_t align, size_t size)
{
#ifdef _MSC_VER
    // unverified: not built on MSVC. MSVC has no std::aligned_alloc.
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
#ifdef _MSC_VER
    // unverified: not built on MSVC. A block from _aligned_malloc goes to _aligned_free.
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

// Helpers for the reflection table of generated code. T is the field type.
template <class T> HOT_HIDDEN constexpr uint8_t field_flags()
{
    return static_cast<uint8_t>((std::is_trivially_copyable_v<T> ? kTriviallyCopyable : 0)
        | (std::is_nothrow_move_constructible_v<std::remove_all_extents_t<T>> ? kNothrowMove : 0) | (holds_no_code<T>::value ? 0 : kMayHoldCode));
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
    if constexpr (std::is_array_v<T>)
        copy_into<T>(dst, src);
    else
        new (dst) T(*static_cast<const T*>(src));
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

// Reflection helpers.
inline const FieldInfo* find_field(const ClassRecord* cls, const char* name)
{
    for (uint32_t i = 0; i < cls->layout->field_count; ++i)
        if (std::strcmp(cls->layout->fields[i].name, name) == 0)
            return &cls->layout->fields[i];
    return nullptr;
}

template <class T> inline const char* type_key()
{
    return typeid(T).name();
}

// The type key of a native container of a [value] script class (design 5.3): "type#hash".
// "hash" is the hash of the layout of the [value] classes in the type. The text stays valid
// after the module unloads. A different hash with the same type resets the container.
HOT_NODEBUG const char* layout_keyed(const char* type, const char* hash);

// True when the type key of a field is the typeid name, with or without a layout hash.
inline bool type_key_matches(const char* key, const char* name)
{
    const size_t n = std::strlen(name);
    return std::strncmp(key, name, n) == 0 && (key[n] == '\0' || key[n] == '#');
}

// Returns a pointer to a field, or null when the name or the type does not match.
template <class T> inline T* field_ptr(ShellBase* s, const char* name)
{
    const FieldInfo* f = find_field(s->cls, name);
    if (!f || !type_key_matches(f->type_key, type_key<T>()))
        return nullptr;
    return reinterpret_cast<T*>(static_cast<char*>(s->state) + f->offset);
}

} // namespace hot
