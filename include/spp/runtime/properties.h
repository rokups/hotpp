// Helpers for the property access of the script (lower.h).
#pragma once
#include <type_traits>
#include <utility>

namespace spp {

// The operators of a compound property write: "o.P += e" uses op_add.
#define SPP_PROP_OP(name, op)                         \
    struct name {                                        \
        template <class A, class B>                      \
        auto operator()(A&& a, B&& b) const              \
        {                                                \
            return std::forward<A>(a) op std::forward<B>(b); \
        }                                                \
    };
SPP_PROP_OP(op_add, +)
SPP_PROP_OP(op_sub, -)
SPP_PROP_OP(op_mul, *)
SPP_PROP_OP(op_div, /)
SPP_PROP_OP(op_mod, %)
SPP_PROP_OP(op_and, &)
SPP_PROP_OP(op_or, |)
SPP_PROP_OP(op_xor, ^)
SPP_PROP_OP(op_shl, <<)
SPP_PROP_OP(op_shr, >>)
#undef SPP_PROP_OP

// A property write that gives a value (lower.h). Each helper evaluates the object once.
// "o.P = v" gives v. "o.P op= e" and "++o.P" give the new value. "o.P++" gives the old value.
template <class O, class T>
T prop_set(void (*set)(O*, T), std::type_identity_t<O>* o, std::type_identity_t<T> v)
{
    set(o, v);
    return v;
}
template <class Op, class O, class T, class E>
T prop_update(T (*get)(O*), void (*set)(O*, T), std::type_identity_t<O>* o, E&& e)
{
    T v = Op{}(get(o), std::forward<E>(e));
    set(o, v);
    return v;
}
template <class Op, class O, class T, class E>
T prop_post(T (*get)(O*), void (*set)(O*, T), std::type_identity_t<O>* o, E&& e)
{
    T old = get(o);
    set(o, Op{}(old, std::forward<E>(e)));
    return old;
}

}  // namespace spp
