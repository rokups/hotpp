// The trait of a handle and the intrusive reference-counted handle.
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/hidden.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace hot
{

// A handle is a smart pointer. "is_handle<T>" is true for std::shared_ptr. A user can
// specialize it for a custom smart pointer.
template <class T> struct is_handle : std::false_type
{
};
template <class T> struct is_handle<std::shared_ptr<T>> : std::true_type
{
};
template <class T> inline constexpr bool is_handle_v = is_handle<detail::remove_cvref_t<T>>::value;

// An intrusive reference-counted handle. The object keeps its own count.
// "Add" and "Release" are member function pointers of the OWNER class, for example
// "&Object::AddRef". T can derive from the owner class. Then "(p->*Add)()" still works.
// A new object starts with zero references (the boost "intrusive_ptr" convention).
// The constructor from "T*" calls Add. So a new object has the count 1 in the handle.
// A move leaves the source null. A null handle never calls Add or Release.
template <class T, auto Add, auto Release> class Intrusive
{
public:
    HOT_HIDDEN Intrusive() noexcept {}
    HOT_HIDDEN Intrusive(std::nullptr_t) noexcept {}
    HOT_HIDDEN Intrusive(T* p)
        : p_(p)
    {
        if (p_)
            (p_->*Add)();
    }
    HOT_HIDDEN Intrusive(const Intrusive& o)
        : p_(o.p_)
    {
        if (p_)
            (p_->*Add)();
    }
    HOT_HIDDEN Intrusive(Intrusive&& o) noexcept
        : p_(o.p_)
    {
        o.p_ = nullptr;
    }

    // Conversion from a handle of a derived type. Add and Release stay the same.
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>, int> = 0>
    HOT_HIDDEN Intrusive(const Intrusive<U, Add, Release>& o)
        : p_(o.get())
    {
        if (p_)
            (p_->*Add)();
    }
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>, int> = 0>
    HOT_HIDDEN Intrusive(Intrusive<U, Add, Release>&& o) noexcept
        : p_(o.get())
    {
        o.detach_();
    }

    HOT_HIDDEN Intrusive& operator=(const Intrusive& o)
    {
        Intrusive(o).swap(*this);
        return *this;
    }
    HOT_HIDDEN Intrusive& operator=(Intrusive&& o) noexcept
    {
        Intrusive(std::move(o)).swap(*this);
        return *this;
    }

    HOT_HIDDEN ~Intrusive()
    {
        if (p_)
            (p_->*Release)();
    }

    HOT_HIDDEN T* get() const noexcept { return p_; }
    HOT_HIDDEN T* operator->() const noexcept { return p_; }
    HOT_HIDDEN T& operator*() const noexcept { return *p_; }
    HOT_HIDDEN explicit operator bool() const noexcept { return p_ != nullptr; }
    HOT_HIDDEN operator T*() const noexcept { return p_; }

    HOT_HIDDEN void reset() noexcept { Intrusive().swap(*this); }
    HOT_HIDDEN void swap(Intrusive& o) noexcept { std::swap(p_, o.p_); }

    HOT_HIDDEN friend bool operator==(const Intrusive& a, const Intrusive& b) noexcept { return a.p_ == b.p_; }
    HOT_HIDDEN friend bool operator==(const Intrusive& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }
    HOT_HIDDEN friend bool operator!=(const Intrusive& a, const Intrusive& b) noexcept { return a.p_ != b.p_; }
    HOT_HIDDEN friend bool operator!=(const Intrusive& a, std::nullptr_t) noexcept { return a.p_ != nullptr; }
    // A comparison with a raw pointer compares the addresses. It makes no temporary wrapper,
    // so the reference count does not change. The forms for "!=" and the reversed order are
    // explicit, because C++17 does not rewrite them. The exact "T*" form wins over the
    // built-in comparison through "operator T*".
    HOT_HIDDEN friend bool operator==(const Intrusive& a, T* b) noexcept { return a.p_ == b; }
    HOT_HIDDEN friend bool operator==(const Intrusive& a, const T* b) noexcept { return a.p_ == b; }
    HOT_HIDDEN friend bool operator==(T* a, const Intrusive& b) noexcept { return a == b.p_; }
    HOT_HIDDEN friend bool operator==(const T* a, const Intrusive& b) noexcept { return a == b.p_; }
    HOT_HIDDEN friend bool operator==(std::nullptr_t, const Intrusive& b) noexcept { return b.p_ == nullptr; }
    HOT_HIDDEN friend bool operator!=(const Intrusive& a, T* b) noexcept { return a.p_ != b; }
    HOT_HIDDEN friend bool operator!=(const Intrusive& a, const T* b) noexcept { return a.p_ != b; }
    HOT_HIDDEN friend bool operator!=(T* a, const Intrusive& b) noexcept { return a != b.p_; }
    HOT_HIDDEN friend bool operator!=(const T* a, const Intrusive& b) noexcept { return a != b.p_; }
    HOT_HIDDEN friend bool operator!=(std::nullptr_t, const Intrusive& b) noexcept { return b.p_ != nullptr; }

private:
    template <class, auto, auto> friend class Intrusive;
    // Gives up the pointer without a Release call. The converting move uses it.
    HOT_HIDDEN void detach_() noexcept { p_ = nullptr; }

    T* p_ = nullptr;
};
template <class T, auto Add, auto Release> struct is_handle<Intrusive<T, Add, Release>> : std::true_type
{
};

namespace detail
{
template <class H> struct is_shared_ptr : std::false_type
{
};
template <class T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type
{
};
template <class H> struct is_intrusive : std::false_type
{
};
template <class T, auto Add, auto Release> struct is_intrusive<Intrusive<T, Add, Release>> : std::true_type
{
};
template <class...> inline constexpr bool always_false_v = false;
} // namespace detail

// Makes the handle H from the argument of a native virtual method. A script override that takes
// the plain handle type "Weapon w" gets it from the native parameter:
//  - a handle ("H", "const H&", "H&&") or anything that converts to H: H itself;
//  - an intrusive H from "T*": the handle adds a reference (Intrusive(T*));
//  - a std::shared_ptr<T> from "T*" or "T&": "shared_from_this()" of the object. A null pointer
//    gives an empty handle. An object that no shared_ptr owns throws hot::NullReference.
// Else the call does not compile: the native class needs a shared_from_this base.
template <class H, class Src> HOT_HIDDEN H bind(Src&& s)
{
    if constexpr (std::is_convertible_v<Src&&, H>)
    {
        return H(std::forward<Src>(s));
    }
    else
    {
        using S = detail::remove_cvref_t<Src>;
        auto* raw = [&]
        {
            if constexpr (std::is_pointer_v<S>)
                return s;
            else
                return std::addressof(s);
        }();
        using E = std::remove_const_t<std::remove_pointer_t<decltype(raw)>>;
        E* object = const_cast<E*>(raw);
        if constexpr (detail::is_intrusive<H>::value)
        {
            return H(object);
        }
        else if constexpr (detail::is_shared_ptr<H>::value && detail::has_shared_from_this<E>::value)
        {
            if (!object)
                return H();
            try
            {
                return std::static_pointer_cast<typename H::element_type>(object->shared_from_this());
            }
            catch (const std::bad_weak_ptr&)
            {
                detail::throw_null_reference("the object has no owning handle: shared_from_this failed");
            }
        }
        else
        {
            static_assert(detail::always_false_v<H, Src>,
                "the native method passes a raw pointer or a reference to a handle type: give the class a "
                "std::enable_shared_from_this base or write the exact native parameter type in the override");
            return H();
        }
    }
}

// Reads a weak handle ("[weak] T x"). The result is the strong handle, or a null handle when
// the object is gone. The template takes any weak pointer type with "lock()", so a custom
// standard namespace (the module attribute "std") works too.
template <class W> HOT_HIDDEN auto lock(const W& w) -> decltype(w.lock())
{
    return w.lock();
}

} // namespace hot
