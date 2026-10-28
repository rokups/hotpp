// The base of a generated shell, the slot call and the checked access to a shell.
#pragma once
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "spp/runtime/records.h"

namespace script {

// Base of each generated shell. The shell also derives from the native class.
struct ShellBase {
    ClassRecord* cls = nullptr;
    void* state = nullptr;
};

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

}  // namespace script
