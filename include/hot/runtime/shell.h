// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The base of a generated shell, the slot call and the checked access to a shell.
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/hidden.h"
#include "hot/runtime/records.h"
#include "hot/runtime/threads.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace hot
{

// The minimal base of each generated shell: only the record of the class. The shell of
// optimized AOT code derives from it directly. The shell also derives from the native class.
struct ShellCore
{
    ClassRecord* cls = nullptr;
    HOT_HIDDEN ShellCore() noexcept {}
};

// Base of each generated shell of the other modes. It adds the state block and the list of
// live objects.
struct ShellBase : ShellCore
{
    void* state = nullptr;
    // The list of live objects (state.cpp). Only the runtime uses these members. A list in the
    // object needs no allocation and no hash lookup, so "new" and free stay cheap.
    ShellBase* live_prev = nullptr;
    ShellBase* live_next = nullptr;
    // The generation whose "new" made the object and its handle, or 0 (see note_creator).
    uint32_t creator = 0;
    HOT_HIDDEN ShellBase() noexcept {}
    // A copy is not in the list of live objects, so it does not copy the list members.
    HOT_HIDDEN ShellBase(const ShellBase& o) noexcept : ShellCore(o), state(o.state) {}
    HOT_HIDDEN ShellBase& operator=(const ShellBase& o) noexcept
    {
        cls = o.cls;
        state = o.state;
        return *this;
    }
};

// The base of the shell of a script class that has no native base (design 7.10.1). It gives
// the shell a virtual destructor, so a handle can destroy it through the base.
struct PlainObject : std::enable_shared_from_this<PlainObject>
{
    HOT_HIDDEN PlainObject() {}
    PlainObject(const PlainObject&) = delete;
    PlainObject& operator=(const PlainObject&) = delete;
    HOT_NODEBUG virtual ~PlainObject() = default;
};

// True when the record "r" is the class "name" or a script class that derives from it. The
// walk uses the names, so a record of any generation gives the same answer (design 7.7).
HOT_HIDDEN bool record_is(const ClassRecord* r, const char* name)
{
    for (int depth = 0; r && depth < 64; r = r->super, ++depth)
        if (std::strcmp(r->name, name) == 0)
            return true;
    return false;
}

// A call through a slot with an old signature (design 5.3): a native delegate on the thunk of a
// signature that a reload changed. The runtime logs the first call of each slot and counts the
// calls (loader.cpp). The call does not run.
HOT_NODEBUG HOT_EXPORT void detached_call(const ClassRecord* cls, unsigned slot);
uint64_t detached_call_count();
// The call of a slot of a removed class (key kRemovedSlotKey): it counts the call and logs the first one.
void note_removed_call();
uint64_t removed_call_count();
// The call of a defunct slot of a live class (key kDefunctSlotKey): the function is removed from the
// source. The call counts, logs the first call of the slot and throws ScriptError "call of removed
// script function Cls::fn". The name comes from ClassRecord::slot_names, else the text has the slot number.
[[noreturn]] HOT_NODEBUG HOT_EXPORT void removed_function_called(const ClassRecord* cls, unsigned slot);
// True when the slot is a slot of a removed class.
HOT_HIDDEN bool slot_is_removed(const ClassRecord* cls, unsigned slot)
{
    const char* const* keys = cls->slot_keys;
    return keys != nullptr && keys[slot] != nullptr && std::strcmp(keys[slot], kRemovedSlotKey) == 0;
}
// True when the slot is a defunct slot of a live class: its function is removed from the source.
HOT_HIDDEN bool slot_is_defunct(const ClassRecord* cls, unsigned slot)
{
    const char* const* keys = cls->slot_keys;
    return keys != nullptr && keys[slot] != nullptr && std::strcmp(keys[slot], kDefunctSlotKey) == 0;
}

namespace detail
{
template <class Sig> struct sig_result;
template <class R, class... P> struct sig_result<R(P...)>
{
    using type = R;
};
// The pointer-to-member type of a slot with the signature "Sig". The state is the object.
template <class Sig> struct sig_pmf;
template <class R, class B, class... P> struct sig_pmf<R(B*, P...)>
{
    using type = R (StateBase::*)(P...);
};
// The result of a detached call: nothing, a value-initialized value, or an exception when the
// result type has no default value (for example a reference).
template <class R> HOT_HIDDEN R detached_result()
{
    if constexpr (std::is_void_v<R>)
        return;
    else if constexpr (std::is_default_constructible_v<R>)
        return R();
    else
        throw ScriptError("a detached delegate cannot make a result value");
}
} // namespace detail

// Calls slot "slot" of the class of the shell "self". The slot is a member of the state of "self". "Sig" is the
// signature of the slot. When the slot has a signature key (ClassRecord::slot_keys) and "Sig" is another signature, the
// call detaches. A slot of a removed class has a key that no signature matches, so its call detaches too: it
// gives a default result and does not run the slot. A defunct slot of a live class (a removed function)
// throws ScriptError (removed_function_called). The call is a script entry (threads.h). In StopTheWorld mode the entry is one flag check. The entry
// starts before the read of the slot, so a commit cannot change the slot under it.
// This form lets an exception pass to the caller. A call from one script function to another
// script virtual function uses it, because the script caller can catch the exception.
// The result type is the result type of "Sig", also a reference ("auto" would copy the value).
template <class Sig, class... A>
HOT_HIDDEN typename detail::sig_result<Sig>::type call_script(ShellBase* self, unsigned slot, A&&... a)
{
    ScriptEntry entry;
    const ClassRecord* cls = self->cls;
    if (const char* const* keys = cls->slot_keys; HOT_UNLIKELY(keys != nullptr))
    {
        if (keys[slot] && std::strcmp(keys[slot], typeid(Sig).name()) != 0)
        {
            if (std::strcmp(keys[slot], kDefunctSlotKey) == 0)
                removed_function_called(cls, slot);
            detached_call(cls, slot);
            return detail::detached_result<typename detail::sig_result<Sig>::type>();
        }
    }
    using Pmf = typename detail::sig_pmf<Sig>::type;
    return (static_cast<StateBase*>(self->state)->*reinterpret_cast<Pmf>(cls->slots[slot]))(std::forward<A>(a)...);
}

// Result of a noexcept shell override after an unhandled script error, when the result type has no
// default value (a reference, for example). The host specializes it with "static R get()".
template <class R> struct noexcept_result;

namespace detail
{
template <class R, class = void> struct has_noexcept_result : std::false_type
{
};
template <class R> struct has_noexcept_result<R, std::void_t<decltype(noexcept_result<R>::get())>> : std::true_type
{
};
} // namespace detail

// The call from the host into the script: a shell override of a native virtual function, and a
// thunk of a delegate. This is the end of the script stack. A hot::ScriptError that the script does
// not handle goes to hot::detail::report_unhandled (errors.h). Then the call returns a value-initialized
// result. A result type with no default value (for example a reference) throws the exception again,
// unless the call is "NoExcept" and the host specialized hot::noexcept_result<R>: then the call
// returns the value of that specialization.
// Any other exception passes to the host, which can catch it. A call inside another call (the
// native code calls a script override while a script call runs) lets the error pass: only the
// outermost call reports.
// The fast path reads the call depth (errors.h), the slot and the state, and calls the slot. The
// rare paths (the counted entry, the signature key, the catch) are out of line.
// "NoExcept" is true for the shell override of a noexcept native virtual function. An exception
// must not leave it, so it always reports and returns the result, also inside another call. Without
// a default value and without hot::noexcept_result<R>, the exception leaves it and ends the program.
namespace detail
{
// The number of hot::call frames on the thread (errors.h). A call inside another call
// (script -> native -> script) is not the end of the script stack. The error passes to the
// script code that called, which can catch it.
struct CallDepth
{
    uint32_t& d;
    HOT_HIDDEN CallDepth() noexcept : d(call_depth()) { ++d; }
    HOT_HIDDEN ~CallDepth() { --d; }
    CallDepth(const CallDepth&) = delete;
    CallDepth& operator=(const CallDepth&) = delete;
};

// Counts the script function frames of the thread in the high bits of the call depth (errors.h).
// A frame above the limit throws ScriptError "stack overflow" (frame_overflow). The check is always
// on: it does not depend on HOT_RUNTIME_CHECKS. The check comes before the add, so the field never
// wraps and a failed constructor leaves nothing to undo. The destructor removes the frame also when
// an exception passes.
struct FrameGuard
{
    uint32_t& d;
    HOT_HIDDEN FrameGuard() : d(call_depth())
    {
        if (HOT_UNLIKELY((d >> 16) >= max_call_depth_value()))
            frame_overflow(d >> 16);
        d += kScriptFrame;
    }
    HOT_HIDDEN ~FrameGuard() { d -= kScriptFrame; }
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
};

// The catch path of hot::call. It runs inside the catch handler, so "throw;" and
// std::current_exception() see the exception. It is out of line: then the fast path has no
// std::exception_ptr on its stack, and so no stack protector and fewer saved registers.
template <class R, bool NoExcept> HOT_NOINLINE R call_unhandled(ShellCore* self, uint32_t depth)
{
    if constexpr (!NoExcept)
        if ((depth & kHostFrameMask) > 1)
            throw;
    report_unhandled(self, std::current_exception());
    if constexpr (std::is_void_v<R> || std::is_default_constructible_v<R>)
        return detached_result<R>();
    else if constexpr (NoExcept && has_noexcept_result<R>::value)
        return noexcept_result<R>::get();
    else
        throw;
}

// The full form of hot::call: a counted script entry (EntryCounters mode) and a slot with a
// signature key. It is out of line, because both cases are rare. It takes the parameter types
// of the slot, as the slot call does. A reference to an argument of the fast path would give that
// argument a stack slot, and so a stack protector.
template <class Sig, bool NoExcept> struct CallSlow;
template <class R, class B, class... P, bool NoExcept> struct CallSlow<R(B*, P...), NoExcept>
{
    HOT_NOINLINE static R run(ShellBase* self, unsigned slot, P... p)
    {
        CallDepth depth;
        try
        {
            return call_script<R(B*, P...)>(self, slot, std::forward<P>(p)...);
        }
        catch (const ScriptError&)
        {
            return call_unhandled<R, NoExcept>(self, depth.d);
        }
    }
};
} // namespace detail

template <class Sig, bool NoExcept = false, class... A>
HOT_HIDDEN typename detail::sig_result<Sig>::type call(ShellBase* self, unsigned slot, A&&... a)
{
    // The fast path is for StopTheWorld mode and a slot with no signature key. There a script
    // entry is only the flag check, so the slot read needs no ScriptEntry. Else call_slow runs
    // the entry and the key check.
    const ClassRecord* cls = self->cls;
    if (HOT_UNLIKELY(entry_counters_enabled() || cls->slot_keys != nullptr))
        return detail::CallSlow<Sig, NoExcept>::run(self, slot, std::forward<A>(a)...);
    detail::CallDepth depth;
    try
    {
        using Pmf = typename detail::sig_pmf<Sig>::type;
        return (static_cast<StateBase*>(self->state)->*reinterpret_cast<Pmf>(cls->slots[slot]))(std::forward<A>(a)...);
    }
    catch (const ScriptError&)
    {
        return detail::call_unhandled<typename detail::sig_result<Sig>::type, NoExcept>(self, depth.d);
    }
}

// The call from the host into the script in optimized AOT code (HOT_AOT_OPTIMIZED). "f" is a
// callable with no argument. It calls the script function directly: no slot, no signature key,
// no script entry. The rule for an error is the rule of hot::call: only the outermost call
// reports a hot::ScriptError, an inner call lets it pass, and "NoExcept" always reports.
// "R" is the result type, also a reference or void.
template <class R, bool NoExcept = false, class F>
HOT_HIDDEN R call_direct(ShellCore* self, F&& f)
{
    detail::CallDepth depth;
    try
    {
        return std::forward<F>(f)();
    }
    catch (const ScriptError&)
    {
        return detail::call_unhandled<R, NoExcept>(self, depth.d);
    }
}

// Records that the code of "generation" made the object "self" (ShellBase::creator). The loader keeps
// that generation loaded while the object lives, because the handle of the object (for example
// the control block of a std::shared_ptr) has code of that generation.
HOT_HIDDEN void note_creator(ShellBase* self, uint32_t generation)
{
    // The shell is in the list already, and a walk of another thread can read "creator".
#if defined(_MSC_VER) && !defined(__clang__)
    // An interlocked exchange of 32 bits is a full barrier, so it is stronger than relaxed.
    _InterlockedExchange(reinterpret_cast<volatile long*>(&self->creator), static_cast<long>(generation));
#else
    __atomic_store_n(&self->creator, generation, __ATOMIC_RELAXED);
#endif
}

// Builds the shell of a script object (design 7.3). "rec" is the record of the running
// generation. "key" is typeid(std::tuple<...>).name() of the base arguments in "args". The
// canonical record of the class holds the constructor of the shell. It is in the core module
// (module.h), or in the first generation when there is no core. So every object has the vtable
// of that module. The caller runs the script constructor on the shell. "note" false skips
// note_creator: the engine factory uses it, because the engine owns that object.
// "mem" null gives a shell from "new". Else the shell is made in "mem" (make_shared_shell).
HOT_NODEBUG inline ShellBase* construct_shell(
    const ClassRecord* rec, const char* key, const void* args, bool note = true, void* mem = nullptr)
{
    const ClassRecord* canon = rec->canonical ? rec->canonical : rec;
    if (!canon->construct)
        throw std::runtime_error(std::string("class ") + rec->name
            + " has no constructor entry in its first generation; restart the program");
    ShellBase* s = canon->construct(mem, key, args);
    if (!s)
        throw std::runtime_error(std::string("class ") + rec->name
            + ": the types of the 'base(...)' arguments changed since the first generation");
    if (note)
        note_creator(s, rec->generation);
    return s;
}

namespace detail
{
// True when the class T has its own operator new (in T or in a base class).
template <class T, class = void> struct has_class_new : std::false_type
{
};
template <class T>
struct has_class_new<T, std::void_t<decltype(T::operator new(std::size_t{}))>> : std::true_type
{
};

// The allocator of make_shared_shell. std::allocate_shared calls "construct" for the shell, in
// the block of the control block. "construct" makes the shell through the canonical record, so
// the shell has the vtable of the canonical module. "destroy" runs the destructor of the shell.
template <class T> struct ShellAllocator
{
    using value_type = T;
    const ClassRecord* rec;
    const char* key;
    const void* args;
    HOT_HIDDEN ShellAllocator(const ClassRecord* r, const char* k, const void* a) noexcept : rec(r), key(k), args(a) {}
    template <class U>
    HOT_HIDDEN ShellAllocator(const ShellAllocator<U>& o) noexcept : rec(o.rec), key(o.key), args(o.args)
    {
    }
    HOT_HIDDEN T* allocate(std::size_t n) { return std::allocator<T>().allocate(n); }
    HOT_HIDDEN void deallocate(T* p, std::size_t n) noexcept { std::allocator<T>().deallocate(p, n); }
    template <class U> HOT_HIDDEN void construct(U* p) { construct_shell(rec, key, args, true, p); }
    template <class U> HOT_HIDDEN void destroy(U* p) noexcept { p->~U(); }
    template <class U> HOT_HIDDEN bool operator==(const ShellAllocator<U>&) const noexcept { return true; }
    template <class U> HOT_HIDDEN bool operator!=(const ShellAllocator<U>&) const noexcept { return false; }
};
} // namespace detail

// The "new" of a script class with a std::shared_ptr handle. It is construct_shell with "note"
// true, and the result has an owner. One block of memory holds the shell and the control block,
// so "new" makes two blocks, not three. The block stays until the last std::weak_ptr goes; the
// destructor and the state block go with the last std::shared_ptr. A class with its own
// operator new keeps two blocks for the shell and the control block, so that operator new runs.
template <class Shell>
HOT_NODEBUG inline std::shared_ptr<Shell> make_shared_shell(const ClassRecord* rec, const char* key, const void* args)
{
    if constexpr (detail::has_class_new<Shell>::value)
        return std::shared_ptr<Shell>(static_cast<Shell*>(construct_shell(rec, key, args)));
    else
        return std::allocate_shared<Shell>(detail::ShellAllocator<Shell>(rec, key, args));
}

// Makes a copy of the script object "src" with a std::shared_ptr handle. The copy has the real
// class of "src", also when "S" is a base class (no slicing). The script constructor does not
// run. The script destructor runs when the copy dies. The shell comes from "new", because the
// shell has the size of its real class.
// "caller" is the record of the generation whose code calls clone_shell. The control block of the
// handle has code of that generation, so the loader keeps it loaded while the copy lives (see
// note_creator). Host code passes nothing: then the copy has no creator generation.
// A class with no clone constructor throws std::runtime_error. This happens for a class with a
// native base, and for a module built before kCloneKey. A restart makes the new module load.
// "S" must derive from ShellBase.
template <class S>
HOT_NODEBUG inline std::shared_ptr<S> clone_shell(const S& src, const ClassRecord* caller = nullptr)
{
    const ShellBase* p = &src;
    const ClassRecord* canon = src.cls->canonical ? src.cls->canonical : src.cls;
    ShellBase* s = canon->construct ? canon->construct(nullptr, kCloneKey, &p) : nullptr;
    if (!s)
        throw std::runtime_error(std::string("class ") + src.cls->name
            + " is not copyable: it has no clone constructor; restart the program");
    std::shared_ptr<S> h(static_cast<S*>(s)); // an exception here deletes the new object
    if (caller)
        note_creator(s, caller->generation);
    return h;
}

// The "new" of a script class with a std::shared_ptr handle in optimized AOT code. The code has
// one generation, so the shell is a plain object of "Shell". One block holds the shell and the
// control block. A class with its own operator new keeps two blocks, so that operator new runs.
template <class Shell, class... A>
HOT_HIDDEN std::shared_ptr<Shell> make_shared_plain(A&... a)
{
    if constexpr (detail::has_class_new<Shell>::value)
        return std::shared_ptr<Shell>(new Shell(Construct{}, a...));
    else
        return std::make_shared<Shell>(Construct{}, a...);
}

// A handle of the script object that owns the shell "self": the lowering of a bare "this"
// where a handle type is expected (design 7.10.1). "H" is the handle type of the class.
// A std::shared_ptr handle uses shared_from_this. The object must be owned by a handle.
template <class H, class S> HOT_HIDDEN H handle_from_this(S* self)
{
    if constexpr (detail::has_element_type<H>::value && detail::has_shared_from_this<S>::value)
    {
        try
        {
            return std::static_pointer_cast<typename H::element_type>(self->shared_from_this());
        }
        catch (const std::bad_weak_ptr&)
        {
            throw std::runtime_error("'this' is not owned by a handle");
        }
    }
    else
    {
        return H(self);
    }
}

// A handle "H" of an object that a raw pointer of a native call gives, for example
// "$C h = node.GetComponent<C>()" or "p as $C". A value that converts to "H" passes through.
// A null pointer gives a null handle. A raw pointer uses handle_from_this: shared_from_this for a
// std::shared_ptr handle, else "H(p)" (an intrusive handle takes a new reference).
template <class H, class P> HOT_HIDDEN H handle_from_raw(P&& p)
{
    using D = std::decay_t<P>;
    if constexpr (std::is_convertible_v<P&&, H>)
        return H(std::forward<P>(p));
    else if constexpr (std::is_pointer_v<D>)
    {
        if (!p)
            return H();
        return handle_from_this<H>(p);
    }
    else
    {
        static_assert(detail::has_get_v<D>, "hot::handle_from_raw needs a pointer or a handle");
        return handle_from_raw<H>(detail::get_raw(p));
    }
}

// The address of a [value] script object, for a property access on another object.
template <class T> HOT_HIDDEN T* value_ptr(T& v)
{
    return &v;
}
template <class T> HOT_HIDDEN T* value_ptr(T* p)
{
    return p;
}

// Property access on an object other than "this" (lower.h). "p.P" becomes
// "Cls__get_P(hot::shell_of<Cls_Shell>(p, "Cls"))". The helper accepts a pointer and
// a reference. The object must be a live script object of the class "cls". Else the
// helper throws std::runtime_error. A null object throws hot::NullReference. A pointer to the shell itself needs no check.
template <class Shell, class T> HOT_HIDDEN Shell* shell_of(T* p, const char* cls)
{
    if (!p)
        detail::throw_null_reference(
            (std::string("the handle is null: expected a script object of class ") + cls).c_str());
    if constexpr (std::is_const_v<T>)
    {
        // A ".cpp" script casts a const object. The script code is const-correct C++, so it
        // does not write through the result.
        return shell_of<Shell>(const_cast<std::remove_const_t<T>*>(p), cls);
    }
    else if constexpr (std::is_same_v<T, Shell> || std::is_base_of_v<Shell, T>)
    {
        return p; // the shell of the class or of a derived script class
    }
    else
    {
        static_assert(std::is_base_of_v<T, Shell>,
            "the object of a property access is not of the native base of the script class");
        auto* s = dynamic_cast<ShellCore*>(p);
        if (!s || !s->cls || !record_is(s->cls, cls))
            throw std::runtime_error(std::string("the object is not a script object of class ") + cls);
        return static_cast<Shell*>(p);
    }
}
template <class Shell, class T, std::enable_if_t<!detail::has_get_v<T>, int> = 0>
HOT_HIDDEN Shell* shell_of(T& v, const char* cls)
{
    return shell_of<Shell>(&v, cls);
}
// A handle of a script object (std::shared_ptr or Intrusive): check the pointee.
template <class Shell, class H, std::enable_if_t<detail::has_get_v<H>, int> = 0>
HOT_HIDDEN Shell* shell_of(const H& h, const char* cls)
{
    return shell_of<Shell>(detail::get_raw(h), cls);
}

// ---- "is" and "as" (design 7.7)
// The ShellCore of the object behind "p", or null: a null pointer or a native object.
template <class T> HOT_HIDDEN const ShellCore* shell_core_of(T* p)
{
    if (!p)
        return nullptr;
    if constexpr (std::is_base_of_v<ShellCore, T>)
        return p;
    else if constexpr (std::is_polymorphic_v<T>)
        return dynamic_cast<const ShellCore*>(p);
    else
        return nullptr;
}

// "obj is C" of a script class C: the class records of the object, not RTTI. A pointer,
// a reference and a handle (a type with get()) are accepted. Null gives false.
template <class T> HOT_HIDDEN bool is_script(T* p, const char* cls)
{
    const ShellCore* s = shell_core_of(p);
    return s && s->cls && record_is(s->cls, cls);
}
template <class H, std::enable_if_t<detail::has_get_v<H>, int> = 0>
HOT_HIDDEN bool is_script(const H& h, const char* cls)
{
    return is_script(detail::get_raw(h), cls);
}
template <class T, std::enable_if_t<!std::is_pointer_v<T> && !detail::has_get_v<T>, int> = 0>
HOT_HIDDEN bool is_script(T& v, const char* cls)
{
    return is_script(&v, cls);
}

// "obj as C" of a script class C. A pointer or a reference gives the shell pointer or null.
// A std::shared_ptr gives a std::shared_ptr of the shell that shares the ownership. Another
// handle gives the shell pointer, so the handle type of C can take it.
template <class Shell, class T> HOT_HIDDEN Shell* as_script(T* p, const char* cls)
{
    if (!is_script(p, cls))
        return nullptr;
    return static_cast<Shell*>(const_cast<ShellCore*>(shell_core_of(p)));
}
template <class Shell, class U>
HOT_HIDDEN std::shared_ptr<Shell> as_script(const std::shared_ptr<U>& h, const char* cls)
{
    Shell* s = as_script<Shell>(detail::get_raw(h), cls);
    return s ? std::shared_ptr<Shell>(h, s) : nullptr;
}
template <class Shell, class H, std::enable_if_t<detail::has_get_v<H>, int> = 0>
HOT_HIDDEN Shell* as_script(const H& h, const char* cls)
{
    return as_script<Shell>(detail::get_raw(h), cls);
}
template <class Shell, class T, std::enable_if_t<!std::is_pointer_v<T> && !detail::has_get_v<T>, int> = 0>
HOT_HIDDEN Shell* as_script(T& v, const char* cls)
{
    return as_script<Shell>(&v, cls);
}

// "obj is T" and "obj as T" of a native class T: dynamic_cast, as in C++.
template <class T, class U> HOT_HIDDEN bool is_native(U* p)
{
    return dynamic_cast<const T*>(p) != nullptr;
}
template <class T, class H, std::enable_if_t<detail::has_get_v<H>, int> = 0> HOT_HIDDEN bool is_native(const H& h)
{
    return is_native<T>(detail::get_raw(h));
}
template <class T, class U, std::enable_if_t<!std::is_pointer_v<U> && !detail::has_get_v<U>, int> = 0>
HOT_HIDDEN bool is_native(U& v)
{
    return is_native<T>(&v);
}
template <class T, class U> HOT_HIDDEN T* as_native(U* p)
{
    return dynamic_cast<T*>(p);
}
template <class T, class U> HOT_HIDDEN std::shared_ptr<T> as_native(const std::shared_ptr<U>& h)
{
    return std::dynamic_pointer_cast<T>(h);
}
template <class T, class H, std::enable_if_t<detail::has_get_v<H>, int> = 0> HOT_HIDDEN T* as_native(const H& h)
{
    return as_native<T>(detail::get_raw(h));
}
template <class T, class U, std::enable_if_t<!std::is_pointer_v<U> && !detail::has_get_v<U>, int> = 0>
HOT_HIDDEN T* as_native(U& v)
{
    return as_native<T>(&v);
}

} // namespace hot
