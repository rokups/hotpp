// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Calls of script interface methods by name. The call survives a hot reload.
//
// An interface handle (Iface<Tag>) holds a script object. A call finds the slot through the
// class record of the object: the method has a session id (iface_method_id), and the record has a
// table from the id to the slot (ClassRecord::iface_slots). The loader rebuilds the table at each
// reload, so a call always uses the current slots.
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/export.h"
#include "hot/runtime/hidden.h"
#include "hot/runtime/records.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"
#include "hot/runtime/threads.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace hot
{

// The session id of the interface method "key" ("I.M") with the signature "sig"
// (typeid(R(P...)).name()). The first call for a pair gives a new id, starting at 1. Later calls
// give the same id. An id is never reused in the session. The function is thread safe.
HOT_NODEBUG HOT_EXPORT uint32_t iface_method_id(const char* key, const char* sig);

// The table from the id of an interface method to the slot, built from the rows of a class
// record (ClassRecord::iface_rows, null: none). The index is the id, the value is the slot or
// kNoIfaceSlot. The result is empty when the class has no rows. The loader and the AOT registry
// use this function, so both give the same table.
HOT_NODEBUG HOT_EXPORT std::vector<uint32_t> build_iface_table(const IfaceRow* rows);

// A handle of a script object, typed by the interface "Tag". It shares the ownership of the object.
template <class Tag> class Iface
{
public:
    HOT_HIDDEN Iface() noexcept {}
    // "null" of the script: an empty handle.
    HOT_HIDDEN Iface(std::nullptr_t) noexcept {}
    HOT_HIDDEN explicit Iface(std::shared_ptr<ShellCore> p) noexcept : p_(std::move(p)) {}
    HOT_HIDDEN ShellCore* get() const noexcept { return p_.get(); }
    HOT_HIDDEN explicit operator bool() const noexcept { return static_cast<bool>(p_); }
    HOT_HIDDEN void reset() noexcept { p_.reset(); }
    // The shared pointer of the object (it shares the count of the handle).
    HOT_HIDDEN const std::shared_ptr<ShellCore>& shared() const noexcept { return p_; }

private:
    std::shared_ptr<ShellCore> p_;
};

// A field of type Iface<Tag> holds a reference count with a control block and a deleter. The
// deleter can be code of a module (to_iface with a handle), so the field may hold code.
template <class Tag> struct holds_no_code<Iface<Tag>> : std::false_type
{
};

namespace detail
{
template <class Sig> struct iface_pmf;
template <class R, class... P> struct iface_pmf<R(P...)>
{
    using type = R (StateBase::*)(P...);
};
template <class T> struct is_std_shared : std::false_type
{
};
template <class T> struct is_std_shared<std::shared_ptr<T>> : std::true_type
{
};
// The interface name of a key "I.M": the text before the last dot.
HOT_HIDDEN std::string iface_of_key(const char* key)
{
    const char* dot = std::strrchr(key, '.');
    return dot ? std::string(key, dot) : std::string(key);
}
[[noreturn]] HOT_NODEBUG HOT_EXPORT void iface_null_handle(const char* key);
[[noreturn]] HOT_NODEBUG HOT_EXPORT void iface_not_implemented(const ClassRecord* cls, const char* key);
} // namespace detail

// True when the class record "r" or a script parent of it lists the interface "name". The walk
// uses the names, as record_is does, so a record of any generation gives the same answer.
HOT_HIDDEN bool record_is_iface(const ClassRecord* r, const char* name)
{
    for (int depth = 0; r && depth < 64; r = r->super, ++depth)
        if (const char* const* l = r->ifaces)
            for (; *l; ++l)
                if (std::strcmp(*l, name) == 0)
                    return true;
    return false;
}

// Calls the method "key" ("I.M") through the handle "h". "Sig" is R(P...), the signature of the
// method without the base pointer. "site" is the cache of the id of this call site: it is 0 at
// the start, and the call sets it. The call makes the signature text typeid(Sig).name(), as
// call_script does for the key of a slot. The call is a script entry
// (threads.h). The entry starts before the read of any table, so a commit cannot change the table
// under the call (the same rule as call_script in shell.h).
// A null handle and a class without the method throw ScriptError.
template <class Sig, class Tag, class... A>
HOT_HIDDEN typename detail::sig_result<Sig>::type call_iface(
    const Iface<Tag>& h, std::atomic<uint32_t>& site, const char* key, A&&... a)
{
    ScriptEntry entry;
    ShellCore* core = h.get();
    if (HOT_UNLIKELY(core == nullptr))
        detail::iface_null_handle(key);
    uint32_t id = site.load(std::memory_order_relaxed);
    if (HOT_UNLIKELY(id == 0))
    {
        id = iface_method_id(key, typeid(Sig).name());
        site.store(id, std::memory_order_relaxed);
    }
    const ClassRecord* cls = core->cls;
    const uint32_t slot = id < cls->iface_slot_count ? cls->iface_slots[id] : kNoIfaceSlot;
    if (HOT_UNLIKELY(slot == kNoIfaceSlot))
        detail::iface_not_implemented(cls, key);
    if (HOT_UNLIKELY(cls->slot_keys != nullptr))
        if (slot_is_defunct(cls, slot))
            removed_function_called(cls, slot);
    if (HOT_UNLIKELY(slot_is_removed(cls, slot)))
    {
        detached_call(cls, slot);
        return detail::detached_result<typename detail::sig_result<Sig>::type>();
    }
    using Pmf = typename detail::iface_pmf<Sig>::type;
    auto* self = static_cast<ShellBase*>(core);
    return (static_cast<StateBase*>(self->state)->*reinterpret_cast<Pmf>(cls->slots[slot]))(std::forward<A>(a)...);
}

// ---- Conversions
// A script object behind the shared pointer "p" gives an interface handle that shares the count.
// An empty pointer and a native object (no ShellCore) give an empty handle.
template <class Tag, class X> HOT_HIDDEN Iface<Tag> to_iface(const std::shared_ptr<X>& p)
{
    ShellCore* core = const_cast<ShellCore*>(shell_core_of(p.get()));
    if (!core)
        return Iface<Tag>();
    return Iface<Tag>(std::shared_ptr<ShellCore>(p, core)); // the aliasing constructor
}
// A handle of the same interface stays as it is. This keeps a conversion of an unknown type safe.
template <class Tag> HOT_HIDDEN Iface<Tag> to_iface(const Iface<Tag>& i) noexcept
{
    return i;
}
// Another handle (a type with get() or Get()): the handle gives a shared pointer with a deleter
// that holds a copy of the handle. The copy lives until the last interface handle goes.
template <class Tag, class H, std::enable_if_t<detail::has_get_v<H> && !detail::is_std_shared<H>::value, int> = 0>
HOT_HIDDEN Iface<Tag> to_iface(const H& h)
{
    ShellCore* core = const_cast<ShellCore*>(shell_core_of(detail::get_raw(h)));
    if (!core)
        return Iface<Tag>();
    return Iface<Tag>(std::shared_ptr<ShellCore>(core, [keep = h](ShellCore*) mutable { (void)keep; }));
}

// ---- Identity (D9)
// The script object behind a handle or an interface handle, or null. "a == b" of the script
// compares these values when one side is an interface. A native object has no script object, so
// it has the same identity as null.
HOT_HIDDEN const ShellCore* iface_id(std::nullptr_t) noexcept
{
    return nullptr;
}
template <class H, std::enable_if_t<detail::has_get_v<H>, int> = 0> HOT_HIDDEN const ShellCore* iface_id(const H& h)
{
    return shell_core_of(detail::get_raw(h));
}
template <class A, class B> HOT_HIDDEN bool iface_same(const A& a, const B& b)
{
    return iface_id(a) == iface_id(b);
}

// "obj is I": the object is a script object, and its class or a script parent lists "I".
template <class T> HOT_HIDDEN bool is_iface(T* p, const char* name)
{
    const ShellCore* s = shell_core_of(p);
    return s && s->cls && record_is_iface(s->cls, name);
}
template <class H, std::enable_if_t<detail::has_get_v<H>, int> = 0>
HOT_HIDDEN bool is_iface(const H& h, const char* name)
{
    return is_iface(detail::get_raw(h), name);
}
template <class T, std::enable_if_t<!std::is_pointer_v<T> && !detail::has_get_v<T>, int> = 0>
HOT_HIDDEN bool is_iface(T& v, const char* name)
{
    return is_iface(&v, name);
}

// "obj as I": the interface handle, or an empty handle when "obj is I" is false.
template <class Tag, class H, std::enable_if_t<detail::has_get_v<H>, int> = 0>
HOT_HIDDEN Iface<Tag> as_iface(const H& h, const char* name)
{
    return is_iface(h, name) ? to_iface<Tag>(h) : Iface<Tag>();
}

// The class handle of the object behind the interface handle "i". "H" is the handle type, "Shell"
// is the shell type of the class "cls". The result is null when "i" is empty or the object is not
// of the class "cls".
template <class H, class Shell, class Tag> HOT_HIDDEN H iface_as(const Iface<Tag>& i, const char* cls)
{
    ShellCore* core = i.get();
    if (!core || !core->cls || !record_is(core->cls, cls))
        return H();
    if constexpr (detail::is_std_shared<H>::value)
        return H(i.shared(), static_cast<Shell*>(core)); // shares the count of the interface handle
    else
        return H(static_cast<Shell*>(core));
}

} // namespace hot
