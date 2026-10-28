// Script runtime (design sections 4.4, 4.6 and 4.7).
// The header has the types and the inline hot path.
// The file script_runtime.cpp has the globals and the generation loader.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "spp/module.h"

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

// Threads (design sections 6.2 and 6.3, experiment R5).
//
// The engine can run script code on worker threads.
// A reload commit changes the class records, the state blocks and active_generation.
// So the commit must run when no thread is in script code.
// The engine gives this guarantee. The commit runs only in the frame boundary hook.
// The hook runs on the main thread, after all workers reached the barrier.
// The workers restart after the hook. The barrier also orders the memory:
// the workers see the writes of the commit after they restart.
//
// The counter below is a debug check of that guarantee.
// The engine wraps each entry into script code in a ScriptTickScope.
// commit_pending() reads the counter. A value other than zero is an engine bug.
// The commit counts it (unsafe_commit_count) and then asserts.
//
// Which thread may touch which data:
//   - Worker threads read class records, state blocks and active_generation,
//     only inside a ScriptTickScope. They write the state block of their own actors.
//     They do not create or destroy script objects. They do not call the Loader.
//   - The main thread creates and destroys script objects. It runs the frame
//     boundary hook (commit_pending, frame_boundary, stats) and calls Loader::load.
//   - Other threads (for example a compile thread) call only
//     Loader::request_reload and Loader::reload_pending. These take the Loader mutex.
//   - Pins and the stale call counter are atomic. Any thread can use them.
int in_script_tick();
uint64_t unsafe_commit_count();

// RAII: the thread is inside script code while the object lives.
class ScriptTickScope {
public:
    ScriptTickScope();
    ~ScriptTickScope();
    ScriptTickScope(const ScriptTickScope&) = delete;
    ScriptTickScope& operator=(const ScriptTickScope&) = delete;
};

// Called by a generation function when its generation is not live.
void stale_call(const char* what);

// The number of stale calls that the guard ignored since the start.
uint64_t stale_call_count();

// Generation pins (section 4.11). A pin tells the runtime that something outside
// script code (for example a closure in an engine timer) still holds code of the
// generation. The counts are atomic: any thread can pin or unpin. A generation
// number maps to one of 65536 counters; two numbers that are 65536 apart share a
// counter. This can only delay an unload.
void pin(uint32_t generation);
void unpin(uint32_t generation);
uint32_t pin_count(uint32_t generation);

// A closure that leaves script code (section 4.11). It holds one pin of its
// generation for its whole life. Each call checks the generation guard: when the
// generation is not live, the call logs a stale call and returns R(). The lambda
// body does not run. A copy of the closure holds its own pin.
//
// The closure converts to std::function (the constructor of std::function takes
// any callable) and to an engine delegate type D that has Bind(std::function<Sig>).
// The closure code lives in the generation module. Destroy the last copy on the
// main thread, before the frame boundary at which the generation can unload.
template <class Sig>
class Closure;

template <class R, class... A>
class Closure<R(A...)> {
public:
    using Function = std::function<R(A...)>;

    Closure() = default;

    template <class F>
    Closure(uint32_t generation, const char* what, F fn)
        : fn_(std::move(fn)), gen_(generation), what_(what)
    {
        pin(gen_);
    }

    Closure(const Closure& o) : fn_(o.fn_), gen_(o.gen_), what_(o.what_)
    {
        if (gen_) pin(gen_);
    }

    Closure(Closure&& o) noexcept : fn_(std::move(o.fn_)), gen_(o.gen_), what_(o.what_)
    {
        o.fn_ = nullptr;
        o.gen_ = 0;
    }

    Closure& operator=(Closure o) noexcept
    {
        std::swap(fn_, o.fn_);
        std::swap(gen_, o.gen_);
        std::swap(what_, o.what_);
        return *this;
    }

    // The lambda goes first (its captures can use the generation code), then the pin.
    ~Closure()
    {
        fn_ = nullptr;
        if (gen_) unpin(gen_);
    }

    R operator()(A... a) const
    {
        if (SCRIPT_UNLIKELY(!fn_ || gen_ != active_generation)) {
            stale_call(what_ ? what_ : "closure");
            return R();
        }
        return fn_(std::forward<A>(a)...);
    }

    explicit operator bool() const { return gen_ != 0; }
    uint32_t generation() const { return gen_; }

    // A std::function that holds a copy of this closure (and so one more pin).
    Function to_function() const
    {
        Closure copy(*this);
        return Function(std::move(copy));
    }

    // Conversion to an engine delegate type that binds a std::function.
    template <class D>
        requires requires(D& d, Function f) { d.Bind(std::move(f)); }
    operator D() const
    {
        D d;
        d.Bind(to_function());
        return d;
    }

private:
    Function fn_;
    uint32_t gen_ = 0;
    const char* what_ = nullptr;
};

// The guard of a script "finally" block. The translator writes
//   { script::Finally __fin_N([&] { body }); try { ... } catch ... }
// The destructor runs the body on each path out of the outer block: the normal end,
// a return, a break, a continue and an exception.
//
// Rule: the body must not let an exception out of the destructor. During unwinding, such
// an exception calls std::terminate. So the destructor catches and drops each exception
// from the body. The exception from the try block (if any) continues.
template <class F>
class Finally {
public:
    explicit Finally(F f) : f_(std::move(f)) {}
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
    ~Finally()
    {
        try {
            f_();
        } catch (...) {
            // Dropped. See the rule above.
        }
    }

private:
    F f_;
};

namespace detail {
template <class M>
struct SigOf;
template <class C, class R, class... A>
struct SigOf<R (C::*)(A...) const> {
    using type = R(A...);
};
template <class C, class R, class... A>
struct SigOf<R (C::*)(A...)> {
    using type = R(A...);
};
}  // namespace detail

// The translator wraps each arrow lambda in this call. "what" names the lambda
// in the stale call log.
template <class F>
auto make_closure(uint32_t generation, const char* what, F fn)
{
    using Sig = typename detail::SigOf<decltype(&F::operator())>::type;
    return Closure<Sig>(generation, what, std::move(fn));
}

// The script writes "a.b" for a pointer and for a reference. The translator writes
// "script::deref(a).b". The helper gives an object for both.
// A pointer gives the object that it points to. This includes an lvalue pointer ("T*&"):
// the pointer overload is more specialized than the reference overloads.
// An lvalue passes through as a reference, without a copy.
// An rvalue returns BY VALUE (moved). A reference to the argument would dangle at the end
// of the full expression. A member of the returned prvalue keeps its lifetime in a
// range-for and in a reference binding: "for (auto x : deref(make()).list)".
// A handle is a smart pointer. "is_handle<T>" is true for std::shared_ptr. A user can
// specialize it for a custom smart pointer. A handle gives the object that it points to.
// A member of a temporary handle lives to the end of the full expression. A range-for
// over a member of a temporary handle needs a local variable for the handle.
template <class T>
struct is_handle : std::false_type {};
template <class T>
struct is_handle<std::shared_ptr<T>> : std::true_type {};
template <class T>
inline constexpr bool is_handle_v = is_handle<std::remove_cvref_t<T>>::value;

// An intrusive reference-counted handle. The object keeps its own count.
// "Add" and "Release" are member function pointers of the OWNER class, for example
// "&Object::AddRef". T can derive from the owner class. Then "(p->*Add)()" still works.
// A new object starts with zero references (the boost "intrusive_ptr" convention).
// The constructor from "T*" calls Add. So a new object has the count 1 in the handle.
// A move leaves the source null. A null handle never calls Add or Release.
template <class T, auto Add, auto Release>
class Intrusive {
public:
    Intrusive() noexcept = default;
    Intrusive(std::nullptr_t) noexcept {}
    Intrusive(T* p) : p_(p)
    {
        if (p_)
            (p_->*Add)();
    }
    Intrusive(const Intrusive& o) : p_(o.p_)
    {
        if (p_)
            (p_->*Add)();
    }
    Intrusive(Intrusive&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    // Conversion from a handle of a derived type. Add and Release stay the same.
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    Intrusive(const Intrusive<U, Add, Release>& o) : p_(o.get())
    {
        if (p_)
            (p_->*Add)();
    }
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    Intrusive(Intrusive<U, Add, Release>&& o) noexcept : p_(o.get())
    {
        o.detach_();
    }

    Intrusive& operator=(const Intrusive& o)
    {
        Intrusive(o).swap(*this);
        return *this;
    }
    Intrusive& operator=(Intrusive&& o) noexcept
    {
        Intrusive(std::move(o)).swap(*this);
        return *this;
    }

    ~Intrusive()
    {
        if (p_)
            (p_->*Release)();
    }

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    operator T*() const noexcept { return p_; }

    void reset() noexcept { Intrusive().swap(*this); }
    void swap(Intrusive& o) noexcept { std::swap(p_, o.p_); }

    friend bool operator==(const Intrusive& a, const Intrusive& b) noexcept { return a.p_ == b.p_; }
    friend bool operator==(const Intrusive& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }

private:
    template <class, auto, auto>
    friend class Intrusive;
    // Gives up the pointer without a Release call. The converting move uses it.
    void detach_() noexcept { p_ = nullptr; }

    T* p_ = nullptr;
};
template <class T, auto Add, auto Release>
struct is_handle<Intrusive<T, Add, Release>> : std::true_type {};

template <class T>
T& deref(T* p)
{
    return *p;
}
// The two overloads below exclude a handle. Else they win over the handle overload.
template <class T>
T& deref(T& v)
    requires(!is_handle_v<T>)
{
    return v;
}
template <class T>
std::remove_cvref_t<T> deref(T&& v)
    requires(!std::is_lvalue_reference_v<T> && !is_handle_v<T>)
{
    return static_cast<T&&>(v);
}
// The handle overload binds an lvalue and a temporary. It covers std::shared_ptr.
template <class H>
decltype(auto) deref(H&& h)
    requires(is_handle_v<H>)
{
    return *h;
}
// An array is not an object that has members. The script cannot use "arr.x" on an array.
// Without this overload, the array would decay to a pointer and "arr.x" would compile.
template <class T, std::size_t N>
void deref(T (&)[N]) = delete;

// The argument "ref e" or "out e" of the script. It holds a reference to the operand.
// Clang picks the form from the native signature: a "T&" parameter gets the object,
// and a "T*" parameter (or a "T*" field) gets its address.
template <class T>
struct RefArg {
    T& ref;
    constexpr operator T&() const noexcept { return ref; }
    constexpr operator T*() const noexcept { return &ref; }
};
template <class T>
constexpr RefArg<T> ref_arg(T& v) noexcept
{
    return RefArg<T>{v};
}
// A temporary has no address that the callee can keep.
template <class T>
void ref_arg(const T&&) = delete;

// Calls slot "slot" of the class record. "Sig" is the signature of the slot.
template <class Sig, class... A>
inline auto call(const ClassRecord* cls, unsigned slot, A&&... a)
{
    return reinterpret_cast<Sig*>(cls->slots[slot])(std::forward<A>(a)...);
}

// Property access on an object other than "this" (lower.h). "p.P" becomes
// "Cls__get_P(script::shell_of<Cls_Shell>(p, "Cls"))". The helper accepts a pointer and
// a reference. The object must be a live script object of the class "cls". Else the
// helper throws std::runtime_error. A pointer to the shell itself needs no check.
template <class Shell, class T>
Shell* shell_of(T* p, const char* cls)
{
    if constexpr (std::is_same_v<T, Shell>) {
        return p;
    } else {
        static_assert(std::is_base_of_v<T, Shell>,
                      "the object of a property access is not of the native base of the script class");
        auto* s = dynamic_cast<ShellBase*>(p);
        if (!s || !s->cls || std::strcmp(s->cls->name, cls) != 0)
            throw std::runtime_error(std::string("the object is not a script object of class ") + cls);
        return static_cast<Shell*>(p);
    }
}
template <class Shell, class T>
Shell* shell_of(T& v, const char* cls)
{
    return shell_of<Shell>(&v, cls);
}

// The operators of a compound property write: "o.P += e" uses op_add.
#define SCRIPT_PROP_OP(name, op)                         \
    struct name {                                        \
        template <class A, class B>                      \
        auto operator()(A&& a, B&& b) const              \
        {                                                \
            return std::forward<A>(a) op std::forward<B>(b); \
        }                                                \
    };
SCRIPT_PROP_OP(op_add, +)
SCRIPT_PROP_OP(op_sub, -)
SCRIPT_PROP_OP(op_mul, *)
SCRIPT_PROP_OP(op_div, /)
SCRIPT_PROP_OP(op_mod, %)
SCRIPT_PROP_OP(op_and, &)
SCRIPT_PROP_OP(op_or, |)
SCRIPT_PROP_OP(op_xor, ^)
SCRIPT_PROP_OP(op_shl, <<)
SCRIPT_PROP_OP(op_shr, >>)
#undef SCRIPT_PROP_OP

// A property write that gives a value (lower.h). Each helper evaluates the object once.
// "o.P = v" gives v. "o.P op= e" and "++o.P" give the new value. "o.P++" gives the old value.
template <class O, class T>
T prop_set(void (*set)(O*, T), std::type_identity_t<O>* o, std::type_identity_t<T> v)
{
    set(o, v);
    return v;
}
template <class Op, class O, class T, class E>
T prop_update(T (*get)(O*), void (*set)(O*, T), std::type_identity_t<O>* o, E&& e)
{
    T v = Op{}(get(o), std::forward<E>(e));
    set(o, v);
    return v;
}
template <class Op, class O, class T, class E>
T prop_post(T (*get)(O*), void (*set)(O*, T), std::type_identity_t<O>* o, E&& e)
{
    T old = get(o);
    set(o, Op{}(old, std::forward<E>(e)));
    return old;
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
    Move,  // copy or move the old field "old_index"
    Init,  // run the initializer
};

struct FieldStep {
    FieldAction action;
    uint32_t old_index;  // valid for Move
};

struct MigrationPlan {
    std::vector<FieldStep> steps;        // one step for each new field, in order
    std::vector<uint32_t> removed;       // old fields that nothing receives
    std::string error;                   // not empty: the reload is rejected (type change)
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

// The state of a generation module in the loader (section 4.9).
enum class GenState : uint8_t {
    Active,    // the live generation
    Retired,   // a reload replaced it; the code stays loaded
    Unloaded,  // the loader released the code (dlclose or ORC tracker removal)
};

struct GenerationInfo {
    uint32_t generation;
    GenState state;
    uint32_t pins;       // pin_count(generation)
    uint32_t code_refs;  // registry entries, thunks and live objects that use the module code
    std::string origin;
};

struct LoaderStats {
    uint32_t active_generation = 0;
    std::vector<GenerationInfo> generations;  // in load order
    size_t live_objects = 0;
};

// Registers generation modules. A module has the function: extern "C" void script_register(Registry&).
//
// Threads. The Loader has one mutex.
// It guards "pending_", "registry_", the class records in "registry_",
// and the write of active_generation.
//   - request_reload and reload_pending can run on any thread.
//     They can run while workers run script code. They take the mutex.
//     They read only data that changes in commit_pending (under the same mutex)
//     and the data of the modules, which does not change.
//     They write only the staged data of the new Pending.
//   - commit_pending runs only at the frame boundary, on the main thread,
//     when no worker is in script code. It takes the mutex.
//     So it waits for a validation that runs at that time.
//     It also checks in_script_tick() (debug check).
//   - load, frame_boundary, stats, find and thunk run on the main thread only.
//     The workers do not call them.
//
// The first module goes through load(). A later module goes through
// request_reload(), which only validates (no side effect on live objects),
// and then commit_pending(), which the World loop calls at a safe point.
// The first record of a class stays the record of the class for ever. A
// commit copies the slots and the layout of the new record into it.
// A thunk keeps the address of the first registration (append-only).
//
// The loader owns every Module. A generation that a reload replaced is
// retired. frame_boundary() unloads a retired generation when all of these hold:
//   1. its pin count is zero;
//   2. at least one frame boundary passed after the commit that retired it;
//   3. no code of the module is in use (code_refs is zero).
// The record, the factory and the thunks of a class stay in the module that
// registered the class first. The vtable of an object stays in the module that
// made the object. Condition 3 keeps those modules loaded.
class Loader {
public:
    Loader();
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Registers the first module.
    bool load(Module module, std::string& error);

    // Validation phase. Returns false and leaves the live generation unchanged
    // when the module is not acceptable. A Backend or module_from_library makes the Module.
    bool request_reload(Module module, std::string& error);
    bool reload_pending() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_ != nullptr;
    }
    // Commit phase. Call it only at a frame boundary. Returns true when a
    // reload was committed. On false, "error" tells why (empty: nothing pending).
    bool commit_pending(std::string& error);

    // Call it at each frame boundary, after commit_pending. It unloads the
    // retired generations that may unload, then counts the boundary.
    // Returns the number of generations that it unloaded.
    unsigned frame_boundary();

    LoaderStats stats() const;

    const ClassEntry* find(const std::string& name) const { return registry_.find(name); }
    void* thunk(const std::string& name) const { return registry_.thunk(name); }

private:
    struct Pending;
    struct Gen {
        uint32_t generation = 0;
        GenState state = GenState::Active;
        Module module;               // empty after the unload
        uint64_t retired_frame = 0;  // the frame counter at the retirement
        uint32_t registry_refs = 0;  // classes and thunks that this module registered
    };
    void add_generation(Module module, uint32_t generation);
    void note_records(const Registry& reg, uint32_t generation);
    // The number of live objects for each module (the module that made the object).
    std::unordered_map<uint32_t, uint32_t> live_by_module() const;
    uint32_t code_refs(const Gen& g, const std::unordered_map<uint32_t, uint32_t>& live) const;

    mutable std::mutex mutex_;  // see "Threads" above
    Registry registry_;
    std::vector<Gen> gens_;  // the loader owns all modules; a retired one stays until it unloads
    std::unordered_map<const ClassRecord*, uint32_t> record_gen_;  // the module that holds a record
    uint64_t frame_ = 0;
    std::unique_ptr<Pending> pending_;
};

}  // namespace script
