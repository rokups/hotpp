// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The checked operations on a container. The checks are always on. They do not depend on
// HOT_RUNTIME_CHECKS.
//
// The translator writes hot::front(c), hot::back(c), hot::top(c), hot::pop(c), hot::pop_back(c)
// and hot::pop_front(c) for the call without arguments of the member of the same name. The call
// of such a member of an empty container throws hot::IndexOutOfRange (index 0, size 0). This
// holds for each type that has empty(): the standard containers, the adapters (queue, stack,
// priority_queue), std::string and a view. A type without empty() gets the plain member call. A
// null pointer or a null handle throws hot::NullReference, as hot::deref does.
//
// The translator writes hot::at(c, k) for the call "c.at(k)". std::out_of_range from the member
// becomes hot::IndexOutOfRange for an integer key. For a key of another type (std::map) it
// becomes hot::ScriptError("key not found: <what>").
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/deref.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/handles.h"
#include "hot/runtime/hidden.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace hot
{

namespace detail
{

template <class C, class = void> constexpr bool has_empty_v = false;
template <class C> constexpr bool has_empty_v<C, std::void_t<decltype(std::declval<const C&>().empty())>> = true;

template <class C, class = void> constexpr bool has_size_v = false;
template <class C> constexpr bool has_size_v<C, std::void_t<decltype(std::declval<const C&>().size())>> = true;

// Throws when the container is empty.
template <class C> HOT_HIDDEN void require_nonempty(const C& c)
{
    if constexpr (has_empty_v<C>)
        if (c.empty())
            throw_index_out_of_range(0, 0);
}

template <class C> struct is_string_view : std::false_type
{
};
template <class Ch, class Tr> struct is_string_view<std::basic_string_view<Ch, Tr>> : std::true_type
{
};

// True when the argument of a helper is a temporary that owns its elements. A reference to an
// element would dangle then, so the helper returns a copy of the element.
template <class C> constexpr bool owns_temporary_v = !std::is_lvalue_reference_v<C> && std::is_class_v<remove_cvref_t<C>>
    && !is_handle_v<remove_cvref_t<C>> && !is_checked_ptr_v<C> && !is_span<remove_cvref_t<C>>::value
    && !is_string_view<remove_cvref_t<C>>::value;

// Gives the element that "r" returns. A reference to an element of a temporary becomes a value.
template <class C, class R> HOT_HIDDEN decltype(auto) element_result(R&& r)
{
    if constexpr (std::is_lvalue_reference_v<R> && !owns_temporary_v<C>)
        return static_cast<R>(r);
    else
        return remove_cvref_t<R>(static_cast<R&&>(r));
}

} // namespace detail

#define HOT_CHECKED_ELEMENT(name) \
    template <class C> HOT_HIDDEN decltype(auto) name(C&& c) \
    { \
        auto& o = hot::deref(c); \
        detail::require_nonempty(o); \
        return detail::element_result<C>(o.name()); \
    }
HOT_CHECKED_ELEMENT(front)
HOT_CHECKED_ELEMENT(back)
HOT_CHECKED_ELEMENT(top)
#undef HOT_CHECKED_ELEMENT

#define HOT_CHECKED_POP(name) \
    template <class C> HOT_HIDDEN decltype(auto) name(C&& c) \
    { \
        auto& o = hot::deref(c); \
        detail::require_nonempty(o); \
        return o.name(); \
    }
HOT_CHECKED_POP(pop)
HOT_CHECKED_POP(pop_back)
HOT_CHECKED_POP(pop_front)
#undef HOT_CHECKED_POP

// "c.at(k)".
template <class C, class K> HOT_HIDDEN decltype(auto) at(C&& c, K&& k)
{
    auto& o = hot::deref(c);
    try
    {
        return detail::element_result<C>(o.at(static_cast<K&&>(k)));
    }
    catch (const std::out_of_range& e)
    {
        if constexpr (std::is_integral<detail::remove_cvref_t<K>>::value
            && detail::has_size_v<detail::remove_cvref_t<decltype(o)>>)
            detail::throw_index_out_of_range(static_cast<std::size_t>(k), static_cast<std::size_t>(o.size()));
        else
            throw ScriptError(std::string("key not found: ") + e.what());
    }
}

} // namespace hot
