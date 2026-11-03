// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The deref helper: the translator writes it for the member access "a.b".
#pragma once
#include "hot/config.h"
#include "hot/runtime/coalesce.h"
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/handles.h"
#include "hot/runtime/hidden.h"

#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
#if __has_include(<span>) && (__cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L))
#include <span> // the view "T[]" of a script is a std::span<T>
#endif

namespace hot
{

// A pointer field of a native class is a CheckedPtr (see checked_ptr.h). It has the form of a
// pointer for the script, so deref, star, as_ptr and as_ref treat it as a pointer.
template <class T> class CheckedPtr;
template <class T> struct is_checked_ptr : std::false_type
{
};
template <class T> struct is_checked_ptr<CheckedPtr<T>> : std::true_type
{
};
template <class T> inline constexpr bool is_checked_ptr_v = is_checked_ptr<detail::remove_cvref_t<T>>::value;

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
template <class T, std::enable_if_t<!is_handle_v<T> && !is_checked_ptr_v<T>, int> = 0> HOT_HIDDEN T& deref(T& v)
{
    return v;
}
template <class T,
    std::enable_if_t<!std::is_lvalue_reference_v<T> && !is_handle_v<T> && !is_checked_ptr_v<T>, int> = 0>
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
// An std::optional gives its value. An empty one throws hot::NullReference.
template <class T> HOT_HIDDEN T& deref(std::optional<T>& o)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!o))
        detail::throw_null_reference("empty optional dereference");
    return *o;
}
template <class T> HOT_HIDDEN const T& deref(const std::optional<T>& o)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!o))
        detail::throw_null_reference("empty optional dereference");
    return *o;
}
template <class T> HOT_HIDDEN T deref(std::optional<T>&& o)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!o))
        detail::throw_null_reference("empty optional dereference");
    return std::move(*o);
}
// A CheckedPtr gives the object like a pointer. A null or an expired target throws
// hot::NullReference. The reference is borrowed: the strong handle lives only inside the call.
template <class P, std::enable_if_t<is_checked_ptr_v<P>, int> = 0> HOT_HIDDEN decltype(auto) deref(P&& p)
{
    return *p;
}
// An array is not an object that has members. The script cannot use "arr.x" on an array.
// Without this overload, the array would decay to a pointer and "arr.x" would compile.
template <class T, std::size_t N> void deref(T (&)[N]) = delete;


// The checked unwrap. The script writes "(T*)p" for a pointer "p" of the type "T*", and "($C)h" for a
// handle "h" of the type "$C". The translator writes "hot::nonnull(p)". The helper gives the
// argument when it is not null. A null pointer, a null handle and an empty optional throw
// hot::NullReference. The test is always on, also when HOT_RUNTIME_CHECKS is 0, because the
// script asks for it. A pointer gives the pointer. A handle and a CheckedPtr give a reference
// for an lvalue and a value for an rvalue. An optional gives its value in the same way.
template <class T> HOT_HIDDEN T* nonnull(T* p)
{
    if (HOT_UNLIKELY(!p))
        detail::throw_null_reference("null pointer in a checked unwrap");
    return p;
}
template <class H, std::enable_if_t<is_handle_v<H> || is_checked_ptr_v<H>, int> = 0>
HOT_HIDDEN decltype(auto) nonnull(H&& h)
{
    if (HOT_UNLIKELY(!static_cast<bool>(h)))
        detail::throw_null_reference("null handle in a checked unwrap");
    if constexpr (std::is_lvalue_reference_v<H>)
        return (h);
    else
        return detail::remove_cvref_t<H>(std::move(h));
}
template <class T> HOT_HIDDEN T& nonnull(std::optional<T>& o)
{
    if (HOT_UNLIKELY(!o))
        detail::throw_null_reference("empty optional in a checked unwrap");
    return *o;
}
template <class T> HOT_HIDDEN const T& nonnull(const std::optional<T>& o)
{
    if (HOT_UNLIKELY(!o))
        detail::throw_null_reference("empty optional in a checked unwrap");
    return *o;
}
template <class T> HOT_HIDDEN T nonnull(std::optional<T>&& o)
{
    if (HOT_UNLIKELY(!o))
        detail::throw_null_reference("empty optional in a checked unwrap");
    return std::move(*o);
}

// The script writes the cast "(T*)p". The translator writes "hot::cast_ptr<T*>(p)". When the type
// of "p" is already "T*", or "p" is a CheckedPtr or a handle, the cast is a checked unwrap: the
// null test (hot::nonnull) comes before the C cast. Else it is the C cast.
// The C++ compiler tests the type, so the translator needs no type of "p".
template <class To, class P> HOT_HIDDEN constexpr decltype(auto) cast_ptr(P&& p)
{
    if constexpr (std::is_same_v<detail::remove_cvref_t<P>, To>)
        return nonnull(static_cast<P&&>(p));
    else if constexpr (is_checked_ptr_v<P> || is_handle_v<P>)
        return (To)nonnull(static_cast<P&&>(p)); // the analyzer treats this cast as a checked unwrap
    else
        return (To)(static_cast<P&&>(p));
}

namespace detail
{
template <class T> struct holds_null : std::bool_constant<std::is_pointer_v<T> || is_handle_v<T> || is_checked_ptr_v<T>>
{
};
template <class T> struct holds_null<std::optional<T>> : std::true_type
{
};
} // namespace detail

// The script writes "a?.b" and "a?.f(x)" in an expression. The translator writes
// "hot::safe_access(a, [&](auto&& s) { return hot::deref(s).b; })". The helper reads "a" one
// time. When "a" is null (or an empty optional), the result is empty. Else the result is "f(a)":
// a function that gives a pointer, a handle or an optional keeps its type (null when "a" is null).
// Any other result type "R" becomes "std::optional<R>". A function that gives void runs only
// when "a" is not null. A value "a" (not a pointer, a handle or an optional) is never null.
template <class P, class F> HOT_HIDDEN auto safe_access(P&& p, F&& f)
{
    using D = detail::remove_cvref_t<P>;
    using R = std::invoke_result_t<F&, D&>;
    using RD = detail::remove_cvref_t<R>;
    bool present = true;
    if constexpr (detail::holds_null<D>::value)
        present = static_cast<bool>(p);
    if constexpr (std::is_void_v<R>)
    {
        if (present)
            f(p);
    }
    else if constexpr (detail::holds_null<RD>::value)
    {
        return present ? RD(f(p)) : RD();
    }
    else
    {
        return present ? std::optional<RD>(f(p)) : std::optional<RD>();
    }
}

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
// A CheckedPtr gives its borrowed pointer: nullptr when the pointer is null or expired.
template <class P, std::enable_if_t<is_checked_ptr_v<P>, int> = 0> HOT_HIDDEN auto as_ptr(const P& p)
{
    return p.get();
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
// A CheckedPtr throws hot::NullReference on null or on an expired target (its operator*).
template <class P, std::enable_if_t<is_checked_ptr_v<P>, int> = 0> HOT_HIDDEN decltype(auto) as_ref(const P& p)
{
    return *p;
}
template <class T> HOT_HIDDEN T& as_ref(T* p)
{
    if (HOT_RUNTIME_CHECKS && HOT_UNLIKELY(!p))
        detail::throw_null_reference("null handle passed to a reference parameter");
    return *p;
}

// The trait "checked_index<C>" tells whether "c[i]" has a bounds test for the container type C.
// The default is false, so a container uses its own operator[]. std::array and std::vector are
// checked by default, and so is std::span (the private trait default_checked_index below). The stub attribute
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
#ifdef __cpp_lib_span
// A view "T[]" is checked as an array is. Its index never copies an element: a view does not own.
template <class T, std::size_t E> struct default_checked_index<std::span<T, E>> : std::true_type
{
};
template <class C> struct is_span : std::false_type
{
};
template <class T, std::size_t E> struct is_span<std::span<T, E>> : std::true_type
{
};
#else
template <class C> struct is_span : std::false_type
{
};
#endif
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
        if constexpr (detail::is_span<detail::remove_cvref_t<C>>::value)
            return static_cast<C&&>(c)[static_cast<I&&>(i)]; // the element lives in the owner
        else if constexpr (!std::is_lvalue_reference_v<C> && std::is_lvalue_reference_v<R>
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

// The script compares "x == null" and "x != null". The translator writes "x == nullptr". For a
// handle and a pointer this is the C++ comparison. A "T?" that comes from "a?.b" can have a type
// that the translator does not know ("var d = a?.b; if (d == null)"). These operators make the
// comparison of an std::optional with nullptr work. They are in the global namespace, because the
// generated functions are there.
template <class T> HOT_HIDDEN constexpr bool operator==(const std::optional<T>& o, std::nullptr_t) noexcept
{
    return !o.has_value();
}
template <class T> HOT_HIDDEN constexpr bool operator==(std::nullptr_t, const std::optional<T>& o) noexcept
{
    return !o.has_value();
}
template <class T> HOT_HIDDEN constexpr bool operator!=(const std::optional<T>& o, std::nullptr_t) noexcept
{
    return o.has_value();
}
template <class T> HOT_HIDDEN constexpr bool operator!=(std::nullptr_t, const std::optional<T>& o) noexcept
{
    return o.has_value();
}
