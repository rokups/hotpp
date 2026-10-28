// The base of a generated shell, the slot call and the checked access to a shell.
#pragma once
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "spp/runtime/records.h"

namespace spp {

// Base of each generated shell. The shell also derives from the native class.
struct ShellBase {
    ClassRecord* cls = nullptr;
    void* state = nullptr;
};

// The base of the shell of a script class that has no native base (design 7.10.1). It gives
// the shell a virtual destructor, so a handle can destroy it through the base.
struct PlainObject : std::enable_shared_from_this<PlainObject> {
    PlainObject() = default;
    PlainObject(const PlainObject&) = delete;
    PlainObject& operator=(const PlainObject&) = delete;
    virtual ~PlainObject() = default;
};

// True when the record "r" is the class "name" or a script class that derives from it. The
// walk uses the names, so a record of any generation gives the same answer (design 7.7).
inline bool record_is(const ClassRecord* r, const char* name)
{
    for (int depth = 0; r && depth < 64; r = r->super, ++depth)
        if (std::strcmp(r->name, name) == 0) return true;
    return false;
}

// Calls slot "slot" of the class record. "Sig" is the signature of the slot.
template <class Sig, class... A>
inline auto call(const ClassRecord* cls, unsigned slot, A&&... a)
{
    return reinterpret_cast<Sig*>(cls->slots[slot])(std::forward<A>(a)...);
}

// Records that the code of "generation" made the object "self" (state.cpp). The loader keeps
// that generation loaded while the object lives, because the handle of the object (for example
// the control block of a std::shared_ptr) has code of that generation.
void note_creator(ShellBase* self, uint32_t generation);

// Builds the shell of a script object (design 7.3). "rec" is the record of the running
// generation. "key" is typeid(std::tuple<...>).name() of the base arguments in "args". The
// canonical record of the class holds the constructor of the shell. It is in the core module
// (module.h), or in the first generation when there is no core. So every object has the vtable
// of that module. The caller runs the script constructor on the shell. "note" false skips
// note_creator: the engine factory uses it, because the engine owns that object.
inline ShellBase* construct_shell(const ClassRecord* rec, const char* key, const void* args, bool note = true)
{
    const ClassRecord* canon = rec->canonical ? rec->canonical : rec;
    if (!canon->construct)
        throw std::runtime_error(std::string("class ") + rec->name +
                                 " has no constructor entry in its first generation; restart the program");
    if (std::strcmp(canon->construct_key, key) != 0)
        throw std::runtime_error(std::string("class ") + rec->name +
                                 ": the types of the 'super(...)' arguments changed since the first generation");
    ShellBase* s = canon->construct(args);
    if (note) note_creator(s, rec->generation);
    return s;
}

// A handle of the script object that owns the shell "self": the lowering of a bare "this"
// where a handle type is expected (design 7.10.1). "H" is the handle type of the class.
// A std::shared_ptr handle uses shared_from_this. The object must be owned by a handle.
template <class H, class S>
H handle_from_this(S* self)
{
    if constexpr (requires { typename H::element_type; } && requires { self->shared_from_this(); }) {
        try {
            return std::static_pointer_cast<typename H::element_type>(self->shared_from_this());
        } catch (const std::bad_weak_ptr&) {
            throw std::runtime_error("'this' is not owned by a handle");
        }
    } else {
        return H(self);
    }
}

// The address of a [value] script object, for a property access on another object.
template <class T>
T* value_ptr(T& v)
{
    return &v;
}
template <class T>
T* value_ptr(T* p)
{
    return p;
}

// Property access on an object other than "this" (lower.h). "p.P" becomes
// "Cls__get_P(spp::shell_of<Cls_Shell>(p, "Cls"))". The helper accepts a pointer and
// a reference. The object must be a live script object of the class "cls". Else the
// helper throws std::runtime_error. A pointer to the shell itself needs no check.
template <class Shell, class T>
Shell* shell_of(T* p, const char* cls)
{
    if (!p) throw std::runtime_error(std::string("the handle is null: expected a script object of class ") + cls);
    if constexpr (std::is_same_v<T, Shell> || std::is_base_of_v<Shell, T>) {
        return p;  // the shell of the class or of a derived script class
    } else {
        static_assert(std::is_base_of_v<T, Shell>,
                      "the object of a property access is not of the native base of the script class");
        auto* s = dynamic_cast<ShellBase*>(p);
        if (!s || !s->cls || !record_is(s->cls, cls))
            throw std::runtime_error(std::string("the object is not a script object of class ") + cls);
        return static_cast<Shell*>(p);
    }
}
template <class Shell, class T>
    requires(!requires(const T& h) { h.get(); })
Shell* shell_of(T& v, const char* cls)
{
    return shell_of<Shell>(&v, cls);
}
// A handle of a script object (std::shared_ptr or Intrusive): check the pointee.
template <class Shell, class H>
    requires requires(const H& h) { h.get(); }
Shell* shell_of(const H& h, const char* cls)
{
    return shell_of<Shell>(h.get(), cls);
}

// ---- "is" and "as" (design 7.7)
// The ShellBase of the object behind "p", or null: a null pointer or a native object.
template <class T>
const ShellBase* shell_base_of(T* p)
{
    if (!p) return nullptr;
    if constexpr (std::is_base_of_v<ShellBase, T>)
        return p;
    else if constexpr (std::is_polymorphic_v<T>)
        return dynamic_cast<const ShellBase*>(p);
    else
        return nullptr;
}

// "obj is C" of a script class C: the class records of the object, not RTTI. A pointer,
// a reference and a handle (a type with get()) are accepted. Null gives false.
template <class T>
bool is_script(T* p, const char* cls)
{
    const ShellBase* s = shell_base_of(p);
    return s && s->cls && record_is(s->cls, cls);
}
template <class H>
    requires requires(const H& h) { h.get(); }
bool is_script(const H& h, const char* cls)
{
    return is_script(h.get(), cls);
}
template <class T>
    requires(!std::is_pointer_v<T> && !requires(const T& h) { h.get(); })
bool is_script(T& v, const char* cls)
{
    return is_script(&v, cls);
}

// "obj as C" of a script class C. A pointer or a reference gives the shell pointer or null.
// A std::shared_ptr gives a std::shared_ptr of the shell that shares the ownership. Another
// handle gives the shell pointer, so the handle type of C can take it.
template <class Shell, class T>
Shell* as_script(T* p, const char* cls)
{
    if (!is_script(p, cls)) return nullptr;
    return static_cast<Shell*>(const_cast<ShellBase*>(shell_base_of(p)));
}
template <class Shell, class U>
std::shared_ptr<Shell> as_script(const std::shared_ptr<U>& h, const char* cls)
{
    Shell* s = as_script<Shell>(h.get(), cls);
    return s ? std::shared_ptr<Shell>(h, s) : nullptr;
}
template <class Shell, class H>
    requires requires(const H& h) { h.get(); }
Shell* as_script(const H& h, const char* cls)
{
    return as_script<Shell>(h.get(), cls);
}
template <class Shell, class T>
    requires(!std::is_pointer_v<T> && !requires(const T& h) { h.get(); })
Shell* as_script(T& v, const char* cls)
{
    return as_script<Shell>(&v, cls);
}

// "obj is T" and "obj as T" of a native class T: dynamic_cast, as in C++.
template <class T, class U>
bool is_native(U* p)
{
    return dynamic_cast<const T*>(p) != nullptr;
}
template <class T, class H>
    requires requires(const H& h) { h.get(); }
bool is_native(const H& h)
{
    return is_native<T>(h.get());
}
template <class T, class U>
    requires(!std::is_pointer_v<U> && !requires(const U& h) { h.get(); })
bool is_native(U& v)
{
    return is_native<T>(&v);
}
template <class T, class U>
T* as_native(U* p)
{
    return dynamic_cast<T*>(p);
}
template <class T, class U>
std::shared_ptr<T> as_native(const std::shared_ptr<U>& h)
{
    return std::dynamic_pointer_cast<T>(h);
}
template <class T, class H>
    requires requires(const H& h) { h.get(); }
T* as_native(const H& h)
{
    return as_native<T>(h.get());
}
template <class T, class U>
    requires(!std::is_pointer_v<U> && !requires(const U& h) { h.get(); })
T* as_native(U& v)
{
    return as_native<T>(&v);
}

}  // namespace spp
