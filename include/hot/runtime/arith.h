// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The integer arithmetic of the script. The checks are always on. They do not depend on
// HOT_RUNTIME_CHECKS.
//
// Divide and modulo: the translator writes hot::div and hot::mod for "/" and "%", and
// hot::div_assign and hot::mod_assign for "/=" and "%=". A zero divisor throws
// hot::ArithmeticError. The case INT_MIN / -1 wraps: the quotient is INT_MIN and the remainder is 0.
//
// Shifts: the translator writes hot::shl and hot::shr for "<<" and ">>", and hot::shl_assign and
// hot::shr_assign for "<<=" and ">>=". A count that is negative or not less than the bit width of
// the promoted left operand throws hot::ArithmeticError("shift count out of range"). A left shift
// wraps: it keeps the low bits, also for a negative value and for a shift into the sign bit. A right
// shift of a negative value is an arithmetic shift.
//
// The result type is the type of the C++ operator. A pair of types that are not both integral (a
// float, a class with an operator, a stream) gives the plain C++ operator.
//
// hot::float_to_int converts a float to an integer. It saturates, and NaN gives 0.
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/hidden.h"

#include <climits>
#include <limits>
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

// The count of a shift of the promoted type R. A negative count or a count that is not less
// than the width throws.
template <class R, class B>
HOT_HIDDEN constexpr void check_shift_count(B b)
{
    bool bad = false;
    if constexpr (std::is_signed<B>::value)
        bad = b < 0;
    if (bad || static_cast<unsigned long long>(b) >= sizeof(R) * CHAR_BIT)
        throw ArithmeticError("shift count out of range");
}

// 2 to the power n as a floating point value. The value is exact for the range of the type.
template <class F>
HOT_HIDDEN constexpr F pow2(int n)
{
    F r = 1;
    for (int i = 0; i < n; ++i)
        r *= 2;
    return r;
}

// True when static_cast<To>(declval<From>()) is valid.
template <class From, class To, class = void>
constexpr bool can_static_cast_v = false;
template <class From, class To>
constexpr bool can_static_cast_v<From, To, std::void_t<decltype(static_cast<To>(std::declval<From>()))>> = true;

// True for a cast from a pointer to an integer type (not bool) that can hold the address.
template <class From, class To>
constexpr bool pointer_to_int_v = std::is_pointer<From>::value && std::is_integral<To>::value
    && !std::is_same<std::remove_cv_t<To>, bool>::value && sizeof(To) >= sizeof(From);

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

// "a << b" for two integral types. The value keeps the low bits of the promoted type.
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto shl(A a, B b) -> decltype(a << b)
{
    using R = decltype(a << b);
    using U = std::make_unsigned_t<R>;
    detail::check_shift_count<R>(b);
    return static_cast<R>(static_cast<U>(static_cast<U>(static_cast<R>(a)) << b)); // the shift of an unsigned value wraps
}

// "a << b" for any other types.
template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto shl(A&& a, B&& b) -> decltype(std::forward<A>(a) << std::forward<B>(b))
{
    return std::forward<A>(a) << std::forward<B>(b);
}

// "a >> b" for two integral types. A negative value shifts with the sign.
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto shr(A a, B b) -> decltype(a >> b)
{
    using R = decltype(a >> b);
    detail::check_shift_count<R>(b);
    return static_cast<R>(static_cast<R>(a) >> b);
}

// "a >> b" for any other types.
template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr auto shr(A&& a, B&& b) -> decltype(std::forward<A>(a) >> std::forward<B>(b))
{
    return std::forward<A>(a) >> std::forward<B>(b);
}

// "a <<= b" and "a >>= b". The target "a" is a reference, so the caller evaluates it one time.
template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr A& shl_assign(A& a, B b)
{
    a = static_cast<A>(hot::shl(a, b));
    return a;
}

template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr decltype(auto) shl_assign(A& a, B&& b)
{
    return a <<= std::forward<B>(b);
}

template <class A, class B, std::enable_if_t<detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr A& shr_assign(A& a, B b)
{
    a = static_cast<A>(hot::shr(a, b));
    return a;
}

template <class A, class B, std::enable_if_t<!detail::both_integral_v<A, B>, int> = 0>
HOT_HIDDEN constexpr decltype(auto) shr_assign(A& a, B&& b)
{
    return a >>= std::forward<B>(b);
}

// "(To)f" for a floating point value f and an integer type To. A value out of the range of To gives
// the nearest limit of To. NaN gives 0. The fraction is cut off, as in the C++ cast. The cast of any
// other type (an integer, an enum, a class with a conversion operator, bool) is the static_cast. A
// pointer to an integer type that can hold the address is the reinterpret_cast. A type that these
// casts do not accept (an unrelated class, an integer to a pointer) does not compile. The argument
// is a copy, so a bit-field can be the argument.
template <class To, class From,
    std::enable_if_t<detail::can_static_cast_v<From, To> || detail::pointer_to_int_v<From, To>, int> = 0>
HOT_HIDDEN constexpr To float_to_int(From f)
{
    using F = From;
    if constexpr (detail::pointer_to_int_v<From, To>)
    {
        return reinterpret_cast<To>(f);
    }
    else if constexpr (std::is_floating_point<F>::value && std::is_integral<To>::value && !std::is_same<To, bool>::value)
    {
        using L = std::numeric_limits<To>;
        if (f != f)
            return To(0);
        const F top = detail::pow2<F>(L::digits); // the first value above the range
        if (f >= top)
            return L::max();
        if (L::is_signed)
        {
            if (f <= -top)
                return L::min();
        }
        else if (f <= F(-1))
        {
            return To(0);
        }
        return static_cast<To>(f);
    }
    else
    {
        return static_cast<To>(f);
    }
}

} // namespace hot
