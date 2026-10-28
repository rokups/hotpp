// Script runtime (design sections 4.4, 4.6 and 4.7).
// The header has the types and the inline hot path.
// The file script_runtime.cpp has the globals and the generation loader.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
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
    const char* base_name;  // name of the native base class
    uint64_t shell_hash;    // hash of the shell layout (see shell_layout_hash)
};

// Hash of the shell layout. Two generations can share live objects only when
// the hash is equal. The hash covers the base class, the shell size and the
// number of native virtual functions (one stub in the shell for each).
constexpr uint64_t fnv1a(const char* s, uint64_t h = 14695981039346656037ull)
{
    for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 1099511628211ull;
    return h;
}

constexpr uint64_t hash_mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; ++i, v >>= 8) h = (h ^ (v & 0xff)) * 1099511628211ull;
    return h;
}

constexpr uint64_t shell_layout_hash(const char* base, uint64_t shell_size, uint64_t shell_align,
                                     uint64_t native_virtuals)
{
    return hash_mix(hash_mix(hash_mix(fnv1a(base), shell_size), shell_align), native_virtuals);
}

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

// What the migration does for one field of the new layout (section 4.7).
enum class FieldAction : uint8_t {
    Move,             // copy or move the old field "old_index"
    Init,             // run the initializer
    InitTypeChanged,  // run the initializer; the type of the old field is different
};

struct FieldStep {
    FieldAction action;
    uint32_t old_index;  // valid for Move and InitTypeChanged
};

struct MigrationPlan {
    std::vector<FieldStep> steps;        // one step for each new field, in order
    std::vector<uint32_t> removed;       // old fields that nothing receives
    std::vector<std::string> warnings;   // type changes
};

// Compares the old and the new field tables. It has no side effects.
MigrationPlan build_plan(const StateLayout& old_layout, const StateLayout& new_layout);

// A generation module fills the registry in script_register.
class Registry {
public:
    void add(ClassRecord* record, Factory create);
    // A thunk is a stable function address for a native delegate (section 4.11).
    void add_thunk(const std::string& name, void* fn);
    const ClassEntry* find(const std::string& name) const;
    ClassEntry* find(const std::string& name);
    void* thunk(const std::string& name) const;
    uint32_t max_generation() const { return max_generation_; }
    const std::unordered_map<std::string, ClassEntry>& entries() const { return entries_; }
    const std::unordered_map<std::string, void*>& thunks() const { return thunks_; }

private:
    std::unordered_map<std::string, ClassEntry> entries_;
    std::unordered_map<std::string, void*> thunks_;
    uint32_t max_generation_ = 0;
};

// Loads generation modules. A module exports: extern "C" void script_register(Registry&).
//
// The first module goes through load(). A later module goes through
// request_reload(), which only validates (no side effect on live objects),
// and then commit_pending(), which the World loop calls at a safe point.
// The first record of a class stays the record of the class for ever. A
// commit copies the slots and the layout of the new record into it.
// A thunk keeps the address of the first registration (append-only).
class Loader {
public:
    Loader();
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    bool load(const std::string& path, std::string& error);

    // Validation phase. Returns false and leaves the live generation unchanged
    // when the library is not acceptable.
    bool request_reload(const std::string& path, std::string& error);
    bool reload_pending() const { return pending_ != nullptr; }
    // Commit phase. Call it only at a frame boundary. Returns true when a
    // reload was committed. On false, "error" tells why (empty: nothing pending).
    bool commit_pending(std::string& error);

    const ClassEntry* find(const std::string& name) const { return registry_.find(name); }
    void* thunk(const std::string& name) const { return registry_.thunk(name); }

private:
    struct Pending;
    Registry registry_;
    std::vector<void*> handles_;
    std::unique_ptr<Pending> pending_;
};

}  // namespace script
