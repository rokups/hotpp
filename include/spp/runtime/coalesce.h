// The helper of the script operator "x ?? y".
#pragma once
#include "spp/runtime/hidden.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace spp
{

template <class T> struct is_optional : std::false_type
{
};
template <class T> struct is_optional<std::optional<T>> : std::true_type
{
};

// Gives "x" when "x" is true in a bool context (a handle, a pointer or an optional). Else it gives
// the result of "y". The result has the common type of the two sides. The helper reads "x" one
// time. "y" is a callable: it runs only when "x" is false.
// When "x" is an optional and the result of "y" is not, the result has the common type of the
// value type of "x" and the type of "y". Then the helper gives the value in "x".
template <class X, class F> SPP_HIDDEN constexpr auto coalesce(X&& x, F&& y)
{
    using Y = std::invoke_result_t<F&>;
    using DX = std::decay_t<X>;
    using DY = std::decay_t<Y>;
    if constexpr (is_optional<DX>::value && !is_optional<DY>::value)
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

} // namespace spp
