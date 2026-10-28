// The argument "ref e" or "out e" of the script.
#pragma once

#include <memory>
#include <type_traits>

#include "spp/runtime/handles.h"

namespace spp {

// The argument "ref e" or "out e" of the script. It holds a reference to the operand.
// Clang picks the form from the native signature: a "T&" parameter gets the object,
// and a "T*" parameter (or a "T*" field) gets its address.
template <class T>
struct RefArg {
    T& ref;
    constexpr operator T&() const noexcept { return ref; }
    constexpr operator T*() const noexcept { return &ref; }
    // A handle converts also to the raw pointer of its pointee. A native "Actor*" parameter then
    // takes "ref h" where "h" is a handle to Actor (design 7.10.6). The callee must not keep it.
    template <class P>
        requires(is_handle_v<T> && std::is_pointer_v<P> && !std::is_same_v<P, T*> &&
                 std::is_convertible_v<decltype(std::to_address(std::declval<T&>())), P>)
    operator P() const
    {
        return std::to_address(ref);
    }
};
template <class T>
constexpr RefArg<T> ref_arg(T& v) noexcept
{
    return RefArg<T>{v};
}
// A temporary has no address that the callee can keep.
template <class T>
void ref_arg(const T&&) = delete;

}  // namespace spp
