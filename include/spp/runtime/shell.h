// The base of a generated shell, the slot call and the checked access to a shell.
#pragma once
#include <cstring>
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
struct PlainObject {
    PlainObject() = default;
    PlainObject(const PlainObject&) = delete;
    PlainObject& operator=(const PlainObject&) = delete;
    virtual ~PlainObject() = default;
};

// Calls slot "slot" of the class record. "Sig" is the signature of the slot.
template <class Sig, class... A>
inline auto call(const ClassRecord* cls, unsigned slot, A&&... a)
{
    return reinterpret_cast<Sig*>(cls->slots[slot])(std::forward<A>(a)...);
}

// Builds the shell of a script object in the module that made the class (design 7.3).
// "rec" is the record of the running generation. "key" is typeid(std::tuple<...>).name()
// of the base arguments in "args". The first record of the class holds the constructor of
// the shell, so every object has the vtable of that module. The caller runs the script
// constructor on the shell.
inline ShellBase* construct_shell(const ClassRecord* rec, const char* key, const void* args)
{
    const ClassRecord* canon = rec->canonical ? rec->canonical : rec;
    if (!canon->construct)
        throw std::runtime_error(std::string("class ") + rec->name +
                                 " has no constructor entry in its first generation; restart the program");
    if (std::strcmp(canon->construct_key, key) != 0)
        throw std::runtime_error(std::string("class ") + rec->name +
                                 ": the types of the 'super(...)' arguments changed since the first generation");
    return canon->construct(args);
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

}  // namespace spp
