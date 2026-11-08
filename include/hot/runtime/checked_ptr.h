// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::CheckedPtr<T>: a weak pointer to a native object.
// The pointer never dangles. A read of an expired object gives null.
// The operators "->" and "*" throw hot::NullReference on a null or an expired object.
// The traits weak_ref_traits<T> tell how to make and to read the weak reference of T.
// Declare (or include) the specialization of weak_ref_traits<T> before the first use of
// CheckedPtr<T>. A later specialization is an ODR violation.
// A CheckedPtr belongs to one thread, like the raw pointer that it replaces: get(), "*" and "->"
// lock the weak reference, but the object that they give is not safe against a release on
// another thread.
// Note: "operator T*()" lets "delete p" compile for a CheckedPtr. The delete frees an object that
// a handle owns. Do not write it. A native call that takes ownership of a raw pointer is not valid.
#pragma once
#include "hot/runtime/errors.h"
#include "hot/runtime/handles.h"
#include "hot/runtime/hidden.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace hot
{

// The weak reference traits of T. The primary template has no definition.
// A specialization has these members:
//   strong_type                   the strong handle of T
//   weak_type                     the weak handle of T (default constructible)
//   to_weak(const strong_type&)   makes the weak handle
//   lock(const weak_type&)        gives the strong handle, or a null handle when the object is gone
// A specialization can also have "from_raw(T*)". It gives the strong handle of a raw pointer, or a
// null handle when no handle owns the object. Without "from_raw", CheckedPtr accepts no raw pointer.
template <class T, class = void> struct weak_ref_traits;

// A helper base for a weak reference with custom handle types. It uses hot::lock to read the weak handle.
// It has no "from_raw": the program can assign a handle only.
template <class T, class Strong, class Weak> struct custom_weak_ref
{
    using strong_type = Strong;
    using weak_type = Weak;
    static weak_type to_weak(const strong_type& s)
    {
        return weak_type(s);
    }
    static strong_type lock(const weak_type& w)
    {
        return hot::lock(w);
    }
};

// A helper base for std::shared_ptr and std::weak_ptr. The program can assign a handle only.
template <class T> struct shared_weak_ref : custom_weak_ref<T, std::shared_ptr<T>, std::weak_ptr<T>>
{
};

namespace detail
{
template <class T, class = void> struct has_weak_from_this : std::false_type
{
};
template <class T>
struct has_weak_from_this<T, std::void_t<decltype(std::declval<T&>().weak_from_this())>> : std::true_type
{
};
} // namespace detail

// A type with "weak_from_this()", such as a class with a std::enable_shared_from_this base.
// "T" can be a derived class: the aliasing constructor keeps the pointer of the derived class.
template <class T>
struct weak_ref_traits<T, std::void_t<decltype(std::declval<T&>().weak_from_this())>>
    : shared_weak_ref<T>
{
    static std::shared_ptr<T> from_raw(T* p)
    {
        // An empty owner with the aliasing constructor still gives a non-null pointer: test the owner first.
        auto owner = p->weak_from_this().lock();
        if (!owner)
            return std::shared_ptr<T>();
        return std::shared_ptr<T>(std::move(owner), p);
    }
};

namespace detail
{
template <class T, class = void> struct has_weak_ref : std::false_type
{
};
template <class T> struct has_weak_ref<T, std::void_t<typename weak_ref_traits<T>::weak_type>> : std::true_type
{
};
template <class Traits, class T, class = void> struct has_from_raw : std::false_type
{
};
template <class Traits, class T>
struct has_from_raw<Traits, T, std::void_t<decltype(Traits::from_raw(std::declval<T*>()))>> : std::true_type
{
};
} // namespace detail

// True when weak_ref_traits exists for T. The const of T does not count.
template <class T> inline constexpr bool has_weak_ref_v = detail::has_weak_ref<std::remove_const_t<T>>::value;

// A weak pointer to a native object of type T.
template <class T> class CheckedPtr
{
    template <class V> friend class CheckedPtr;

    static_assert(has_weak_ref_v<T>,
        "CheckedPtr<T> needs a weak source: a std::enable_shared_from_this base or a weak_ref_traits specialization");

    using U = std::remove_const_t<T>;
    using Traits = weak_ref_traits<U>;
    using Strong = typename Traits::strong_type;
    using Weak = typename Traits::weak_type;

    // The pointer inside a strong handle, or nullptr for a null handle.
    HOT_HIDDEN static T* pointer_of(const Strong& s)
    {
        return s ? std::addressof(*s) : nullptr;
    }

    HOT_HIDDEN static Strong strong_of(T* p)
    {
        if (!p)
            return Strong();
        if constexpr (detail::has_from_raw<Traits, U>::value)
        {
            Strong s = Traits::from_raw(const_cast<U*>(p));
            if (!s)
                detail::throw_null_reference(
                    "the object has no owner: a pointer field needs an object that a handle owns");
            return s;
        }
        else
        {
            static_assert(detail::always_false_v<T>,
                "assignment from 'T*' needs std::enable_shared_from_this or a weak_handle stub");
            return Strong();
        }
    }

    HOT_HIDDEN Strong lock_strong() const
    {
        return Traits::lock(weak_);
    }

public:
    // The result of "->". It holds the strong handle to the end of the statement.
    class Locked
    {
    public:
        HOT_HIDDEN explicit Locked(Strong s) : strong_(std::move(s)) {}
        Locked(const Locked&) = delete;
        Locked& operator=(const Locked&) = delete;
        HOT_HIDDEN Locked(Locked&&) = default;
        HOT_HIDDEN T* operator->() const
        {
            return pointer_of(strong_);
        }

    private:
        Strong strong_;
    };

    HOT_HIDDEN CheckedPtr() = default;
    HOT_HIDDEN CheckedPtr(std::nullptr_t) {}
    // A raw pointer. Null gives an empty pointer. An object that no handle owns throws hot::NullReference.
    HOT_HIDDEN CheckedPtr(T* p) : weak_(Traits::to_weak(strong_of(p))) {}
    HOT_HIDDEN CheckedPtr(const Strong& s) : weak_(Traits::to_weak(s)) {}
    // A handle of a const object. The pointer of const T never gives a non-const access.
    template <class V = Strong,
        class = std::enable_if_t<std::is_const_v<T> && std::is_same_v<V, std::shared_ptr<U>>>>
    HOT_HIDDEN CheckedPtr(const std::shared_ptr<const U>& s) : weak_(Traits::to_weak(std::const_pointer_cast<U>(s)))
    {
    }
    // A pointer to T converts to a pointer to const T (never the other way).
    template <class V,
        class = std::enable_if_t<std::is_const_v<T> && !std::is_const_v<V> && std::is_same_v<std::remove_const_t<T>, V>>>
    HOT_HIDDEN CheckedPtr(const CheckedPtr<V>& o) : weak_(o.weak_)
    {
    }
    template <class V,
        class = std::enable_if_t<std::is_const_v<T> && !std::is_const_v<V> && std::is_same_v<std::remove_const_t<T>, V>>>
    HOT_HIDDEN CheckedPtr& operator=(const CheckedPtr<V>& o)
    {
        weak_ = o.weak_;
        return *this;
    }
    HOT_HIDDEN CheckedPtr(const CheckedPtr&) = default;
    HOT_HIDDEN CheckedPtr(CheckedPtr&&) = default;
    HOT_HIDDEN CheckedPtr& operator=(const CheckedPtr&) = default;
    HOT_HIDDEN CheckedPtr& operator=(CheckedPtr&&) = default;

    HOT_HIDDEN CheckedPtr& operator=(std::nullptr_t)
    {
        weak_ = Weak();
        return *this;
    }
    HOT_HIDDEN CheckedPtr& operator=(T* p)
    {
        weak_ = Traits::to_weak(strong_of(p));
        return *this;
    }
    HOT_HIDDEN CheckedPtr& operator=(const Strong& s)
    {
        weak_ = Traits::to_weak(s);
        return *this;
    }

    // The borrowed pointer, or nullptr when the pointer is empty or the object is gone.
    // The object can die after the call: do not keep the result. Use it in one thread only.
    HOT_HIDDEN T* get() const
    {
        return pointer_of(lock_strong());
    }

    // The strong handle, or a null handle when the object is gone.
    HOT_HIDDEN Strong lock() const
    {
        return lock_strong();
    }

    HOT_HIDDEN explicit operator bool() const
    {
        return get() != nullptr;
    }
    // For a native call that takes a raw pointer. The pointer is borrowed, as in get().
    HOT_HIDDEN operator T*() const
    {
        return get();
    }

    // Throws hot::NullReference on a null or an expired object.
    HOT_HIDDEN Locked operator->() const
    {
        Strong s = lock_strong();
        if (!s)
            detail::throw_null_reference("null pointer dereference");
        return Locked(std::move(s));
    }
    // The reference is borrowed, as the result of get(). Throws hot::NullReference on null or expired.
    HOT_HIDDEN T& operator*() const
    {
        Strong s = lock_strong();
        if (!s)
            detail::throw_null_reference("null pointer dereference");
        return *pointer_of(s);
    }

    HOT_HIDDEN friend bool operator==(const CheckedPtr& a, T* b)
    {
        return a.get() == b;
    }
    HOT_HIDDEN friend bool operator==(T* a, const CheckedPtr& b)
    {
        return a == b.get();
    }
    HOT_HIDDEN friend bool operator!=(const CheckedPtr& a, T* b)
    {
        return a.get() != b;
    }
    HOT_HIDDEN friend bool operator!=(T* a, const CheckedPtr& b)
    {
        return a != b.get();
    }
    HOT_HIDDEN friend bool operator==(const CheckedPtr& a, std::nullptr_t)
    {
        return a.get() == nullptr;
    }
    HOT_HIDDEN friend bool operator==(std::nullptr_t, const CheckedPtr& b)
    {
        return b.get() == nullptr;
    }
    HOT_HIDDEN friend bool operator!=(const CheckedPtr& a, std::nullptr_t)
    {
        return a.get() != nullptr;
    }
    HOT_HIDDEN friend bool operator!=(std::nullptr_t, const CheckedPtr& b)
    {
        return b.get() != nullptr;
    }
    HOT_HIDDEN friend bool operator==(const CheckedPtr& a, const CheckedPtr& b)
    {
        return a.get() == b.get();
    }
    HOT_HIDDEN friend bool operator!=(const CheckedPtr& a, const CheckedPtr& b)
    {
        return a.get() != b.get();
    }

private:
    Weak weak_{};
};

} // namespace hot
