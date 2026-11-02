// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::Value<T>: a script class held with value semantics (the [value] attribute).
#pragma once
#include "hot/runtime/errors.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace hot
{

template <class T> class Value;

namespace detail
{
// The record of the generation of the running module. A copy of a Value makes an object with
// clone_shell. The control block of that copy has code of the module that runs the copy. So the
// copy must name the generation of this module (note_creator). The loader then keeps the module
// loaded while the copy lives. The function is hidden, so each module has its own variable.
// The generated script_register sets it. The host never sets it: a host copy has no creator.
HOT_HIDDEN const ClassRecord*& value_caller() noexcept
{
    static const ClassRecord* rec = nullptr;
    return rec;
}
} // namespace detail

// Sets the record of the generation of this module (see detail::value_caller). The generated
// script_register of a script that uses [value] calls it.
HOT_HIDDEN void set_value_caller(const ClassRecord* rec) noexcept
{
    detail::value_caller() = rec;
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
        return Value(clone_shell(*checked(p), detail::value_caller()));
    }

    HOT_HIDDEN Value(const Value& o) : p_(clone_shell(*checked(o.p_), detail::value_caller())) {}
    HOT_HIDDEN Value(Value&& o) noexcept : p_(std::move(o.p_)) {}

    // A Value of a derived class converts to a Value of its base class. The copy keeps the real
    // class. The move shares the object: the source is an rvalue and dies after the move.
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>, int> = 0>
    HOT_HIDDEN Value(const Value<U>& o) : p_(clone_shell(*checked(o.handle()), detail::value_caller()))
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

} // namespace hot
