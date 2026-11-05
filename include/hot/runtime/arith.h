// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The integer divide and modulo of the script. The translator writes hot::div and hot::mod for
// "/" and "%", and hot::div_assign and hot::mod_assign for "/=" and "%=". The checks are always on.
// They do not depend on HOT_RUNTIME_CHECKS.
//
// A zero divisor throws hot::ArithmeticError. The case INT_MIN / -1 wraps: the quotient is INT_MIN
// and the remainder is 0. The result type is the type of the C++ operator. A pair of types that are
// not both integral (a float, a class with an operator) gives the plain C++ operator.
#pragma once
#include "hot/runtime/errors.h"
#include "hot/runtime/hidden.h"

#include <type_traits>
#include <utility>

namespace hot
{

namespace detail
{

template <class A, class B>
constexpr bool both_integral_v = std::is_integral<std::remove_cv_t<std::remove_reference_t<A>>>::value
    && std::is_integral<std::remove_cv_t<std::remove_reference_t<B>>>::value;

// The signed quotient and remainder of x and y in the type R. The divisor is not zero.
template <class R>
HOT_HIDDEN constexpr R wrap_div(R x, R y)
{
    if (std::is_signed<R>::value && y == static_cast<R>(-1))
    {
        using U = std::make_unsigned_t<R>;
        return static_cast<R>(static_cast<U>(U(0) - static_cast<U>(x))); // the negation wraps
    }
    return static_cast<R>(x / y);
}

template <class R>
HOT_HIDDEN constexpr R wrap_mod(R x, R y)
{
    if (std::is_signed<R>::value && y == static_cast<R>(-1))
        return R(0);
    return static_cast<R>(x % y);
}

} // namespace detail

// "a / b" for two integral types.
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto div(A a, B b) -> decltype(a / b)
{
    using R = decltype(a / b);
    if (b == 0)
        throw ArithmeticError("integer divide by zero");
    return detail::wrap_div<R>(static_cast<R>(a), static_cast<R>(b));
}

// "a / b" for any other types.
template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto div(A&& a, B&& b) -> decltype(std::forward<A>(a) / std::forward<B>(b))
{
    return std::forward<A>(a) / std::forward<B>(b);
}

// "a % b" for two integral types.
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto mod(A a, B b) -> decltype(a % b)
{
    using R = decltype(a % b);
    if (b == 0)
        throw ArithmeticError("integer modulo by zero");
    return detail::wrap_mod<R>(static_cast<R>(a), static_cast<R>(b));
}

// "a % b" for any other types.
template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto mod(A&& a, B&& b) -> decltype(std::forward<A>(a) % std::forward<B>(b))
{
    return std::forward<A>(a) % std::forward<B>(b);
}

// "a /= b". The target "a" is a reference, so the caller evaluates it one time.
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr A& div_assign(A& a, B b)
{
    a = static_cast<A>(hot::div(a, b));
    return a;
}

template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr decltype(auto) div_assign(A& a, B&& b)
{
    return a /= std::forward<B>(b);
}

// "a %= b".
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr A& mod_assign(A& a, B b)
{
    a = static_cast<A>(hot::mod(a, b));
    return a;
}

template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr decltype(auto) mod_assign(A& a, B&& b)
{
    return a %= std::forward<B>(b);
}

} // namespace hot
