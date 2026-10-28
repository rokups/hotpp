// The state block of a script object, the reflection helpers and the field access.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <typeinfo>

#include "spp/runtime/records.h"
#include "spp/runtime/shell.h"

namespace script {

// The runtime keeps a list of the live objects. The reload commit uses it.
void track_object(ShellBase* self);
void untrack_object(ShellBase* self);

// Allocates a state block for a layout. Returns null when memory is out.
inline char* alloc_state(const StateLayout* l)
{
    size_t align = l->align;
    size_t size = (l->size + align - 1) / align * align;
    if (size == 0) size = align;
    return static_cast<char*>(std::aligned_alloc(align, size));
}

// Destroys the fields in reverse order. It does not free the block.
inline void destroy_fields(const StateLayout* l, char* mem)
{
    for (uint32_t i = l->field_count; i-- > 0;) {
        const FieldInfo& f = l->fields[i];
        if (f.destroy) f.destroy(mem + f.offset);
    }
}

// Makes the state block of a new object and runs the field initializers.
inline void init_state(ShellBase* self, ClassRecord* cls)
{
    self->cls = cls;
    const StateLayout* l = cls->layout;
    char* mem = alloc_state(l);
    if (!mem) throw std::bad_alloc();
    for (uint32_t i = 0; i < l->field_count; ++i) {
        const FieldInfo& f = l->fields[i];
        if (f.init) f.init(mem, mem + f.offset);
    }
    self->state = mem;
    track_object(self);
}

// Destroys the fields in reverse order and frees the state block.
inline void destroy_state(ShellBase* self)
{
    if (!self->state) return;
    untrack_object(self);
    char* mem = static_cast<char*>(self->state);
    destroy_fields(self->cls->layout, mem);
    std::free(mem);
    self->state = nullptr;
}

// Helpers for the reflection table of generated code. T is the field type.
template <class T>
constexpr uint8_t field_flags()
{
    return static_cast<uint8_t>((std::is_trivially_copyable_v<T> ? kTriviallyCopyable : 0) |
                                (std::is_nothrow_move_constructible_v<T> ? kNothrowMove : 0));
}

template <class T>
inline void move_field(void* dst, const void* src)
{
    new (dst) T(*static_cast<const T*>(src));
}

template <class T>
inline void destroy_field(void* field)
{
    static_cast<T*>(field)->~T();
}

template <class T>
constexpr void (*field_destroyer())(void*)
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
        if (std::strcmp(cls->layout->fields[i].name, name) == 0) return &cls->layout->fields[i];
    return nullptr;
}

template <class T>
inline const char* type_key()
{
    return typeid(T).name();
}

// Returns a pointer to a field, or null when the name or the type does not match.
template <class T>
inline T* field_ptr(ShellBase* s, const char* name)
{
    const FieldInfo* f = find_field(s->cls, name);
    if (!f || std::strcmp(f->type_key, type_key<T>()) != 0) return nullptr;
    return reinterpret_cast<T*>(static_cast<char*>(s->state) + f->offset);
}

}  // namespace script
