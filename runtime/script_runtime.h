// Script runtime (design sections 4.4, 4.6 and 4.7).
// The header has the types and the inline hot path.
// The file script_runtime.cpp has the globals and the generation loader.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#define SCRIPT_UNLIKELY(x) __builtin_expect(!!(x), 0)

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
    const char* was;       // previous name from @was("..."), or null
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
};

// Base of each generated shell. The shell also derives from the native class.
struct ShellBase {
    ClassRecord* cls = nullptr;
    void* state = nullptr;
};

// The generation that is live now. A generation function checks it.
extern uint32_t active_generation;

// Called by a generation function when its generation is not live.
void stale_call(const char* what);

// Calls slot "slot" of the class record. "Sig" is the signature of the slot.
template <class Sig, class... A>
inline auto call(const ClassRecord* cls, unsigned slot, A&&... a)
{
    return reinterpret_cast<Sig*>(cls->slots[slot])(std::forward<A>(a)...);
}

// Makes the state block of a new object and runs the field initializers.
inline void init_state(ShellBase* self, ClassRecord* cls)
{
    self->cls = cls;
    const StateLayout* l = cls->layout;
    size_t align = l->align;
    size_t size = (l->size + align - 1) / align * align;
    char* mem = static_cast<char*>(std::aligned_alloc(align, size));
    if (!mem) throw std::bad_alloc();
    for (uint32_t i = 0; i < l->field_count; ++i) {
        const FieldInfo& f = l->fields[i];
        if (f.init) f.init(mem, mem + f.offset);
    }
    self->state = mem;
}

// Destroys the fields in reverse order and frees the state block.
inline void destroy_state(ShellBase* self)
{
    if (!self->state) return;
    const StateLayout* l = self->cls->layout;
    char* mem = static_cast<char*>(self->state);
    for (uint32_t i = l->field_count; i-- > 0;) {
        const FieldInfo& f = l->fields[i];
        if (f.destroy) f.destroy(mem + f.offset);
    }
    std::free(mem);
    self->state = nullptr;
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

// A generation module fills the registry in script_register.
class Registry {
public:
    void add(ClassRecord* record, Factory create);
    const ClassEntry* find(const std::string& name) const;
    uint32_t max_generation() const { return max_generation_; }

private:
    std::unordered_map<std::string, ClassEntry> entries_;
    uint32_t max_generation_ = 0;
};

// Loads generation modules. A module exports: extern "C" void script_register(Registry&).
// Step R0 loads one module and never unloads it.
class Loader {
public:
    bool load(const std::string& path, std::string& error);
    const ClassEntry* find(const std::string& name) const { return registry_.find(name); }

private:
    Registry registry_;
    std::vector<void*> handles_;
};

}  // namespace script
