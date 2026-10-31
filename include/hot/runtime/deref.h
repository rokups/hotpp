// The deref helper: the translator writes it for the member access "a.b".
#pragma once
#include "hot/config.h"
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/handles.h"
#include "hot/runtime/hidden.h"

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace hot
{

// The script writes "a.b" for a pointer and for a reference. The translator writes
// "hot::deref(a).b". The helper gives an object for both.
// A pointer gives the object that it points to. This includes an lvalue pointer ("T*&"):
// the pointer overload is more specialized than the reference overloads.
// An lvalue passes through as a reference, without a copy.
// An rvalue returns BY VALUE (moved). A reference to the argument would dangle at the end
// of the full expression. A member of the returned prvalue keeps its lifetime in a
// range-for and in a reference binding: "for (auto x : deref(make()).list)".
// A member of a temporary handle lives to the end of the full expression. A range-for
// over a member of a temporary handle needs a local variable for the handle.
// A null pointer or a null handle throws hot::NullReference, unless HOT_RUNTIME_CHECKS is 0.
template <class T> HOT_HIDDEN T& deref(T* p)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!p))
        detail::throw_null_reference("null pointer dereference");
    return *p;
}
// The two overloads below exclude a handle. Else they win over the handle overload.
template <class T, std::enable_if_t<!is_handle_v<T>, int> = 0> HOT_HIDDEN T& deref(T& v)
{
    return v;
}
template <class T, std::enable_if_t<!std::is_lvalue_reference_v<T> && !is_handle_v<T>, int> = 0>
HOT_HIDDEN detail::remove_cvref_t<T> deref(T&& v)
{
    return static_cast<T&&>(v);
}
// The handle overload binds an lvalue and a temporary. It covers std::shared_ptr.
template <class H, std::enable_if_t<is_handle_v<H>, int> = 0> HOT_HIDDEN decltype(auto) deref(H&& h)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!h))
        detail::throw_null_reference("null handle dereference");
    return *h;
}
// An array is not an object that has members. The script cannot use "arr.x" on an array.
// Without this overload, the array would decay to a pointer and "arr.x" would compile.
template <class T, std::size_t N> void deref(T (&)[N]) = delete;


// The script writes "*p" for a pointer and for a handle. The translator writes "hot::star(p)".
// The helper has the same null test as hot::deref. Any other type uses its own operator*.
template <class T> HOT_HIDDEN T& star(T* p)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!p))
        detail::throw_null_reference("null pointer dereference");
    return *p;
}
template <class H, std::enable_if_t<is_handle_v<H>, int> = 0> HOT_HIDDEN decltype(auto) star(H&& h)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!h))
        detail::throw_null_reference("null handle dereference");
    return *h;
}
template <class T, std::enable_if_t<!is_handle_v<T>, int> = 0> HOT_HIDDEN decltype(auto) star(T&& v)
{
    return *static_cast<T&&>(v);
}

// The script passes a handle "h" to a native parameter "T*" or "const T*". The translator
// writes "hot::as_ptr(h)". A null handle gives nullptr. Else the result is "&*h". The pointer
// does not own the object. A pointer of a temporary handle is valid to the end of the full
// expression only. A raw pointer passes through.
template <class H, std::enable_if_t<is_handle_v<H>, int> = 0> HOT_HIDDEN auto as_ptr(const H& h)
{
    using P = decltype(&*h);
    return h ? &*h : static_cast<P>(nullptr);
}
template <class T> HOT_HIDDEN T* as_ptr(T* p)
{
    return p;
}

// The script passes a handle "h" to a native parameter "T&" or "const T&". The translator
// writes "hot::as_ref(h)". A null handle throws hot::NullReference, unless HOT_RUNTIME_CHECKS
// is 0. Else the result is "*h".
template <class H, std::enable_if_t<is_handle_v<H>, int> = 0>
HOT_HIDDEN decltype(auto) as_ref(const H& h)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!h))
        detail::throw_null_reference("null handle passed to a reference parameter");
    return *h;
}
template <class T> HOT_HIDDEN T& as_ref(T* p)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!p))
        detail::throw_null_reference("null handle passed to a reference parameter");
    return *p;
}

// The script writes "c[i]". The translator writes "hot::index(c, i)". An array has a bounds
// test: an index that is negative or not less than N throws hot::IndexOutOfRange, unless
// HOT_RUNTIME_CHECKS is 0. A negative index converts to a large std::size_t, so one test
// covers both. An rvalue std::array returns the element BY VALUE (moved), as hot::deref does.
// Each array overload is more specialized than the generic overload below it.
template <class T, std::size_t N, class I> HOT_HIDDEN T& index(std::array<T, N>& c, I i)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(static_cast<std::size_t>(i) >= N))
        detail::throw_index_out_of_range(static_cast<std::size_t>(i), N);
    return c[static_cast<std::size_t>(i)];
}
template <class T, std::size_t N, class I> HOT_HIDDEN const T& index(const std::array<T, N>& c, I i)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(static_cast<std::size_t>(i) >= N))
        detail::throw_index_out_of_range(static_cast<std::size_t>(i), N);
    return c[static_cast<std::size_t>(i)];
}
template <class T, std::size_t N, class I> HOT_HIDDEN T index(std::array<T, N>&& c, I i)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(static_cast<std::size_t>(i) >= N))
        detail::throw_index_out_of_range(static_cast<std::size_t>(i), N);
    return std::move(c[static_cast<std::size_t>(i)]);
}
template <class T, std::size_t N, class I> HOT_HIDDEN T& index(T (&c)[N], I i)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(static_cast<std::size_t>(i) >= N))
        detail::throw_index_out_of_range(static_cast<std::size_t>(i), N);
    return c[static_cast<std::size_t>(i)];
}
template <class T, std::size_t N, class I> HOT_HIDDEN const T& index(const T (&c)[N], I i)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(static_cast<std::size_t>(i) >= N))
        detail::throw_index_out_of_range(static_cast<std::size_t>(i), N);
    return c[static_cast<std::size_t>(i)];
}
// Any other container (std::vector, std::map, a native type) uses its own operator[].
template <class C, class I> HOT_HIDDEN decltype(auto) index(C&& c, I&& i)
{
    return static_cast<C&&>(c)[static_cast<I&&>(i)];
}

} // namespace hot
