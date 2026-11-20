// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The helper of the script operator "x ?? y".
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/hidden.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace hot
{

// An optional-like type is "O<V>" with one type argument. It has "value_type", "has_value()" and
// "operator*". It is std::optional or the optional of a host (see the stub attribute "optional").
// A pointer, a handle, std::variant and std::expected are not optional-like.
namespace detail
{
template <class T> struct one_arg_template : std::false_type
{
};
template <template <class> class O, class V> struct one_arg_template<O<V>> : std::true_type
{
};
template <class T, class = void> struct has_optional_api : std::false_type
{
};
template <class T>
struct has_optional_api<T,
    std::void_t<typename T::value_type, decltype(std::declval<const T&>().has_value()), decltype(*std::declval<T&>())>>
    : std::true_type
{
};
} // namespace detail
template <class T>
struct is_optional_like
    : std::bool_constant<detail::one_arg_template<T>::value && detail::has_optional_api<T>::value>
{
};
template <class T> inline constexpr bool is_optional_like_v = is_optional_like<detail::remove_cvref_t<T>>::value;

// Gives "x" when "x" is true in a bool context (a handle, a pointer or an optional). Else it gives
// the result of "y". The result has the common type of the two sides. The helper reads "x" one
// time. "y" is a callable: it runs only when "x" is false.
// When "x" is an optional and the result of "y" is not, the result has the common type of the
// value type of "x" and the type of "y". Then the helper gives the value in "x".
template <class X, class F> HOT_HIDDEN constexpr auto coalesce(X&& x, F&& y)
{
    using Y = std::invoke_result_t<F&>;
    using DX = std::decay_t<X>;
    using DY = std::decay_t<Y>;
    if constexpr (is_optional_like<DX>::value && !is_optional_like<DY>::value)
    {
        using R = std::common_type_t<typename DX::value_type, DY>;
        if (x)
            return R(*std::forward<X>(x));
        return R(y());
    }
    else
    {
        using R = std::common_type_t<DX, DY>;
        if (static_cast<bool>(x))
            return R(std::forward<X>(x));
        return R(y());
    }
}

} // namespace hot
