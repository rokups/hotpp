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
#include <vector>

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

// The trait "checked_index<C>" tells whether "c[i]" has a bounds test for the container type C.
// The default is false, so a container uses its own operator[]. std::array and std::vector are
// checked by default (the private trait default_checked_index below). The stub attribute
// "checked_index" of a native type makes the translator write a specialization in the module
// header (see doc/native-interop-checks.md). A host can also write one by hand:
//     template <class T> struct hot::checked_index<my::Ring<T>> : std::true_type {};
// A checked type needs "size()" and "operator[]". A specialization that derives from
// std::false_type removes the default check of a type. The trait is separate from
// hot::holds_no_code.
namespace detail
{
// True when "R" (the result of operator[] of C) is a proxy: a prvalue that is not a "value_type".
template <class C, class R, class = void> struct index_proxy : std::false_type
{
};
template <class C, class R>
struct index_proxy<C, R, std::void_t<typename C::value_type>>
    : std::bool_constant<!std::is_reference_v<R> && !std::is_same_v<R, typename C::value_type>>
{
    using type = typename C::value_type;
};
template <class C> struct default_checked_index : std::false_type
{
};
template <class T, std::size_t N> struct default_checked_index<std::array<T, N>> : std::true_type
{
};
template <class T, class A> struct default_checked_index<std::vector<T, A>> : std::true_type
{
};
} // namespace detail

template <class C> struct checked_index : detail::default_checked_index<C>
{
};

// The script writes "c[i]". The translator writes "hot::index(c, i)". A container with
// checked_index<C> and a C array have a bounds test: an index that is negative or not less than
// the size throws hot::IndexOutOfRange, unless HOT_RUNTIME_CHECKS is 0. A negative index
// converts to a large std::size_t, so one test covers both. An rvalue checked container returns
// a non-const element BY VALUE (moved), as hot::deref does: a reference would dangle. A proxy
// (the element of an rvalue std::vector<bool>) becomes a "value_type" value for the same reason.
// Any other container (std::map, a native type) uses its own operator[].
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
template <class C, class I> HOT_HIDDEN decltype(auto) index(C&& c, I&& i)
{
    if constexpr (checked_index<detail::remove_cvref_t<C>>::value)
    {
        if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(static_cast<std::size_t>(i) >= static_cast<std::size_t>(c.size())))
            detail::throw_index_out_of_range(static_cast<std::size_t>(i), static_cast<std::size_t>(c.size()));
        using R = decltype(static_cast<C&&>(c)[static_cast<I&&>(i)]);
        if constexpr (!std::is_lvalue_reference_v<C> && std::is_lvalue_reference_v<R>
            && !std::is_const_v<std::remove_reference_t<R>>)
            return std::remove_reference_t<R>(std::move(c[static_cast<I&&>(i)]));
        else if constexpr (!std::is_lvalue_reference_v<C> && detail::index_proxy<detail::remove_cvref_t<C>, R>::value)
            // A proxy (std::vector<bool>) would keep a pointer to the dead temporary.
            return static_cast<typename detail::index_proxy<detail::remove_cvref_t<C>, R>::type>(
                c[static_cast<I&&>(i)]);
        else
            return static_cast<C&&>(c)[static_cast<I&&>(i)];
    }
    else
        return static_cast<C&&>(c)[static_cast<I&&>(i)];
}

} // namespace hot
