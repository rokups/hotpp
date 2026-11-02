// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::Value<T>: a script class held with value semantics (the [value] attribute).
#pragma once
#include "hot/runtime/errors.h"
#include "hot/runtime/shell.h"

#include <memory>
#include <utility>

namespace hot
{

// An owning box of a script object. It holds a std::shared_ptr and the pointer is never null,
// except in a moved-from Value. A copy of the Value makes a copy of the object with its real
// class (clone_shell), so a Value<Base> that holds a derived object copies the derived object.
// "T" is a shell class. The copy does not run the script constructor.
template <class T> class Value
{
public:
    // Takes the object of the handle. A null handle throws hot::NullReference.
    HOT_HIDDEN Value(std::shared_ptr<T> p) : p_(checked(std::move(p))) {}

    // Makes a Value with a copy of the object of the handle. A null handle throws
    // hot::NullReference.
    HOT_HIDDEN static Value copy_of(const std::shared_ptr<T>& p)
    {
        return Value(clone_shell(*checked(p)));
    }

    HOT_HIDDEN Value(const Value& o) : p_(clone_shell(*checked(o.p_))) {}
    HOT_HIDDEN Value(Value&& o) noexcept : p_(std::move(o.p_)) {}

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

} // namespace hot
