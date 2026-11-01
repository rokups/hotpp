// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Helpers for the property access of the script (lower.h).
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/hidden.h"

#include <type_traits>
#include <utility>

namespace hot
{

// The operators of a compound property write: "o.P += e" uses op_add.
#define HOT_PROP_OP(name, op) \
    struct name \
    { \
        template <class A, class B> HOT_HIDDEN auto operator()(A&& a, B&& b) const \
        { \
            return std::forward<A>(a) op std::forward<B>(b); \
        } \
    };
HOT_PROP_OP(op_add, +)
HOT_PROP_OP(op_sub, -)
HOT_PROP_OP(op_mul, *)
HOT_PROP_OP(op_div, /)
HOT_PROP_OP(op_mod, %)
HOT_PROP_OP(op_and, &)
HOT_PROP_OP(op_or, |)
HOT_PROP_OP(op_xor, ^)
HOT_PROP_OP(op_shl, <<)
HOT_PROP_OP(op_shr, >>)
#undef HOT_PROP_OP

// A property write that gives a value (lower.h). Each helper evaluates the object once.
// "o.P = v" gives v. "o.P op= e" and "++o.P" give the new value. "o.P++" gives the old value.
template <class O, class T>
HOT_HIDDEN T prop_set(void (*set)(O*, T), detail::type_identity_t<O>* o, detail::type_identity_t<T> v)
{
    set(o, v);
    return v;
}
template <class Op, class O, class T, class E>
HOT_HIDDEN T prop_update(T (*get)(O*), void (*set)(O*, T), detail::type_identity_t<O>* o, E&& e)
{
    T v = Op{}(get(o), std::forward<E>(e));
    set(o, v);
    return v;
}
template <class Op, class O, class T, class E>
HOT_HIDDEN T prop_post(T (*get)(O*), void (*set)(O*, T), detail::type_identity_t<O>* o, E&& e)
{
    T old = get(o);
    set(o, Op{}(old, std::forward<E>(e)));
    return old;
}

} // namespace hot
