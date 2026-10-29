// The deref helper: the translator writes it for the member access "a.b".
#pragma once
#include <cstddef>
#include <type_traits>

#include "spp/runtime/handles.h"
#include "spp/runtime/hidden.h"

namespace spp {

// The script writes "a.b" for a pointer and for a reference. The translator writes
// "spp::deref(a).b". The helper gives an object for both.
// A pointer gives the object that it points to. This includes an lvalue pointer ("T*&"):
// the pointer overload is more specialized than the reference overloads.
// An lvalue passes through as a reference, without a copy.
// An rvalue returns BY VALUE (moved). A reference to the argument would dangle at the end
// of the full expression. A member of the returned prvalue keeps its lifetime in a
// range-for and in a reference binding: "for (auto x : deref(make()).list)".
// A member of a temporary handle lives to the end of the full expression. A range-for
// over a member of a temporary handle needs a local variable for the handle.
template <class T>
SPP_HIDDEN T& deref(T* p)
{
    return *p;
}
// The two overloads below exclude a handle. Else they win over the handle overload.
template <class T>
SPP_HIDDEN T& deref(T& v)
    requires(!is_handle_v<T>)
{
    return v;
}
template <class T>
SPP_HIDDEN std::remove_cvref_t<T> deref(T&& v)
    requires(!std::is_lvalue_reference_v<T> && !is_handle_v<T>)
{
    return static_cast<T&&>(v);
}
// The handle overload binds an lvalue and a temporary. It covers std::shared_ptr.
template <class H>
SPP_HIDDEN decltype(auto) deref(H&& h)
    requires(is_handle_v<H>)
{
    return *h;
}
// An array is not an object that has members. The script cannot use "arr.x" on an array.
// Without this overload, the array would decay to a pointer and "arr.x" would compile.
template <class T, std::size_t N>
void deref(T (&)[N]) = delete;

}  // namespace spp
