// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::Value<T>: a script class held with value semantics (the [value] attribute).
#pragma once
#include "hot/runtime/errors.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace hot
{

template <class T> class Value;

namespace detail
{
// The operand of a script operator: the object of a Value, or any other operand as it is. The
// operators of a Value (below) use it, so "a + b" on two boxes calls the operator of the class.
template <class T> HOT_HIDDEN T& unbox(const Value<T>& v)
{
    return *v;
}
template <class X, class = void> struct is_value : std::false_type
{
};
template <class X> HOT_HIDDEN const X& unbox(const X& x, std::enable_if_t<!is_value<X>::value, int> = 0)
{
    return x;
}

// The generation of the running module (0: none). A copy of a Value makes an object with
// clone_shell. The control block of that copy has code of the module that runs the copy. So the
// copy must name the generation of this module (note_creator). The loader then keeps the module
// loaded while the copy lives. The function is hidden, so each module has its own variable.
// The generated script_register sets it. The host never sets it: a host copy has no creator.
HOT_HIDDEN uint32_t& value_caller() noexcept
{
    static uint32_t generation = 0;
    return generation;
}
} // namespace detail

// Sets the generation of this module (see detail::value_caller). The generated script_register
// of a script that uses a Value calls it with GEN. A module with no script class has no record,
// so it names the generation and not a record.
HOT_HIDDEN void set_value_generation(uint32_t generation) noexcept
{
    detail::value_caller() = generation;
}

// The same, with the record of a class of this module.
HOT_HIDDEN void set_value_caller(const ClassRecord* rec) noexcept
{
    detail::value_caller() = rec ? rec->generation : 0u;
}

// An owning box of a script object. It holds a std::shared_ptr and the pointer is never null,
// except in a moved-from Value. A copy of the Value makes a copy of the object with its real
// class (clone_shell), so a Value<Base> that holds a derived object copies the derived object.
// "T" is a shell class. The copy does not run the script constructor.
template <class T> class Value
{
public:
    // Takes the object of the handle (no copy). A null handle throws hot::NullReference. The
    // constructor is explicit: a handle that converts to a Value without a copy would share the object.
    HOT_HIDDEN explicit Value(std::shared_ptr<T> p) : p_(checked(std::move(p))) {}

    // Makes a Value with a copy of the object of the handle. A null handle throws
    // hot::NullReference.
    HOT_HIDDEN static Value copy_of(const std::shared_ptr<T>& p)
    {
        return Value(clone_shell_in(*checked(p), detail::value_caller()));
    }

    HOT_HIDDEN Value(const Value& o) : p_(clone_shell_in(*checked(o.p_), detail::value_caller())) {}
    HOT_HIDDEN Value(Value&& o) noexcept : p_(std::move(o.p_)) {}

    // A Value of a derived class converts to a Value of its base class. The copy keeps the real
    // class. The move shares the object: the source is an rvalue and dies after the move.
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>, int> = 0>
    HOT_HIDDEN Value(const Value<U>& o) : p_(clone_shell_in(*checked(o.handle()), detail::value_caller()))
    {
    }
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>, int> = 0>
    HOT_HIDDEN Value(Value<U>&& o) noexcept : p_(o.handle())
    {
    }

    // The copy comes first. When it throws, this Value does not change.
    HOT_HIDDEN Value& operator=(const Value& o)
    {
        Value tmp(o);
        p_.swap(tmp.p_);
        return *this;
    }
    HOT_HIDDEN Value& operator=(Value&& o) noexcept
    {
        p_ = std::move(o.p_);
        return *this;
    }

    HOT_HIDDEN T* operator->() const { return p_.get(); }
    // The raw pointer. It makes hot::shell_of work on a Value like on a handle.
    HOT_HIDDEN T* get() const { return p_.get(); }
    HOT_HIDDEN T& operator*() const { return *p_; }
    HOT_HIDDEN const std::shared_ptr<T>& handle() const { return p_; }

    // "v[i]": the "operator[]" of the class. An index that is a Value gives its object.
    template <class I>
    HOT_HIDDEN auto operator[](const I& i) const -> decltype(std::declval<T&>()[detail::unbox(i)])
    {
        return (*p_)[detail::unbox(i)];
    }

private:
    template <class P> HOT_HIDDEN static P&& checked(P&& p)
    {
        if (!p)
            detail::throw_null_reference("null handle used as a value");
        return std::forward<P>(p);
    }

    std::shared_ptr<T> p_;
};

// True for a Value (the primary template is in state.h).
template <class T> struct is_value_box<Value<T>> : std::true_type
{
};

namespace detail
{
template <class T> struct is_value<Value<T>, void> : std::true_type
{
};
} // namespace detail

// The operators of a box. "a + b" with a Value on one side or on both sides calls the operator of
// the script class on the objects. A Value with no such operator does not match (SFINAE). A
// compare operator and "!=" work the same way. The result is the result of the class operator.
#define HOT_VALUE_BINARY_OP(OP)                                                                              \
    template <class A, class B, std::enable_if_t<detail::is_value<A>::value || detail::is_value<B>::value, int> = 0> \
    HOT_HIDDEN auto operator OP(const A& a, const B& b)->decltype(detail::unbox(a) OP detail::unbox(b))       \
    {                                                                                                        \
        return detail::unbox(a) OP detail::unbox(b);                                                          \
    }
HOT_VALUE_BINARY_OP(+)
HOT_VALUE_BINARY_OP(-)
HOT_VALUE_BINARY_OP(*)
HOT_VALUE_BINARY_OP(/)
HOT_VALUE_BINARY_OP(%)
HOT_VALUE_BINARY_OP(&)
HOT_VALUE_BINARY_OP(|)
HOT_VALUE_BINARY_OP(^)
HOT_VALUE_BINARY_OP(<<)
HOT_VALUE_BINARY_OP(>>)
HOT_VALUE_BINARY_OP(==)
HOT_VALUE_BINARY_OP(!=)
HOT_VALUE_BINARY_OP(<)
HOT_VALUE_BINARY_OP(>)
HOT_VALUE_BINARY_OP(<=)
HOT_VALUE_BINARY_OP(>=)
#undef HOT_VALUE_BINARY_OP

#define HOT_VALUE_UNARY_OP(OP)                                                                               \
    template <class T> HOT_HIDDEN auto operator OP(const Value<T>& a)->decltype(OP std::declval<T&>())       \
    {                                                                                                        \
        return OP * a;                                                                                       \
    }
HOT_VALUE_UNARY_OP(+)
HOT_VALUE_UNARY_OP(-)
HOT_VALUE_UNARY_OP(!)
HOT_VALUE_UNARY_OP(~)
#undef HOT_VALUE_UNARY_OP

// "a += b" on a box calls the compound operator of the shell class. The shell generates it for a
// binary operator that returns the class. It gives the object of "a" the state of "a + b"
// (assign_value), so the object keeps its identity. The box "a" does not get a new object.
#define HOT_VALUE_COMPOUND_OP(OP)                                                                            \
    template <class T, class B>                                                                              \
    HOT_HIDDEN auto operator OP(Value<T>& a, const B& b)->decltype(std::declval<T&>() OP detail::unbox(b), a) \
    {                                                                                                        \
        *a OP detail::unbox(b);                                                                              \
        return a;                                                                                            \
    }
HOT_VALUE_COMPOUND_OP(+=)
HOT_VALUE_COMPOUND_OP(-=)
HOT_VALUE_COMPOUND_OP(*=)
HOT_VALUE_COMPOUND_OP(/=)
HOT_VALUE_COMPOUND_OP(%=)
HOT_VALUE_COMPOUND_OP(&=)
HOT_VALUE_COMPOUND_OP(|=)
HOT_VALUE_COMPOUND_OP(^=)
HOT_VALUE_COMPOUND_OP(<<=)
HOT_VALUE_COMPOUND_OP(>>=)
#undef HOT_VALUE_COMPOUND_OP

// The conversion of an expression to "Value<T>" in generated code. A Value copies or moves
// (the copy makes a new object). A handle gives a copy of its object, and a null handle throws.
// "new C()" does not use this function: the generated code builds the Value from the new object.
template <class T, class V, std::enable_if_t<is_value_box<std::decay_t<V>>::value, int> = 0>
HOT_HIDDEN Value<T> as_value(V&& v)
{
    return Value<T>(std::forward<V>(v));
}
template <class T, class U> HOT_HIDDEN Value<T> as_value(const std::shared_ptr<U>& p)
{
    return Value<T>::copy_of(p);
}

// A Value that views the object "o" and does not own it. The destructor frees nothing. A copy of
// the view makes a real object (clone_shell). The shell member of an operator uses it to pass a
// "const C&" to a script function that takes "in C". The view must not outlive "o".
template <class T> HOT_HIDDEN Value<T> borrow_value(const T& o)
{
    return Value<T>(std::shared_ptr<T>(std::shared_ptr<T>(), const_cast<T*>(&o)));
}

// Gives the object "dst" the state of the object that "v" owns. The two objects swap their state
// blocks, so the old state of "dst" dies with "v". Both objects must have the same class record.
// The shell member of a compound operator ("a += b") uses it with the result of "a + b".
template <class S> HOT_NODEBUG void assign_value(S& dst, Value<S>&& v)
{
    S& src = *v;
    if (&dst == &src)
        return;
    if (dst.cls != src.cls)
        throw ScriptError(std::string("class ") + dst.cls->name + ": cannot assign an object of class "
            + src.cls->name);
    ScriptEntry entry; // a commit must not migrate the objects during the swap
    std::swap(dst.state, src.state);
    set_owner(dst.cls->layout, static_cast<char*>(dst.state), &dst);
    set_owner(src.cls->layout, static_cast<char*>(src.state), &src);
}

} // namespace hot
