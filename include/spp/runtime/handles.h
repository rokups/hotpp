// The trait of a handle and the intrusive reference-counted handle.
#pragma once
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "spp/runtime/hidden.h"

namespace spp {

// A handle is a smart pointer. "is_handle<T>" is true for std::shared_ptr. A user can
// specialize it for a custom smart pointer.
template <class T>
struct is_handle : std::false_type {};
template <class T>
struct is_handle<std::shared_ptr<T>> : std::true_type {};
template <class T>
inline constexpr bool is_handle_v = is_handle<std::remove_cvref_t<T>>::value;

// An intrusive reference-counted handle. The object keeps its own count.
// "Add" and "Release" are member function pointers of the OWNER class, for example
// "&Object::AddRef". T can derive from the owner class. Then "(p->*Add)()" still works.
// A new object starts with zero references (the boost "intrusive_ptr" convention).
// The constructor from "T*" calls Add. So a new object has the count 1 in the handle.
// A move leaves the source null. A null handle never calls Add or Release.
template <class T, auto Add, auto Release>
class Intrusive {
public:
    SPP_HIDDEN Intrusive() noexcept {}
    SPP_HIDDEN Intrusive(std::nullptr_t) noexcept {}
    SPP_HIDDEN Intrusive(T* p) : p_(p)
    {
        if (p_)
            (p_->*Add)();
    }
    SPP_HIDDEN Intrusive(const Intrusive& o) : p_(o.p_)
    {
        if (p_)
            (p_->*Add)();
    }
    SPP_HIDDEN Intrusive(Intrusive&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    // Conversion from a handle of a derived type. Add and Release stay the same.
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    SPP_HIDDEN Intrusive(const Intrusive<U, Add, Release>& o) : p_(o.get())
    {
        if (p_)
            (p_->*Add)();
    }
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    SPP_HIDDEN Intrusive(Intrusive<U, Add, Release>&& o) noexcept : p_(o.get())
    {
        o.detach_();
    }

    SPP_HIDDEN Intrusive& operator=(const Intrusive& o)
    {
        Intrusive(o).swap(*this);
        return *this;
    }
    SPP_HIDDEN Intrusive& operator=(Intrusive&& o) noexcept
    {
        Intrusive(std::move(o)).swap(*this);
        return *this;
    }

    SPP_HIDDEN ~Intrusive()
    {
        if (p_)
            (p_->*Release)();
    }

    SPP_HIDDEN T* get() const noexcept { return p_; }
    SPP_HIDDEN T* operator->() const noexcept { return p_; }
    SPP_HIDDEN T& operator*() const noexcept { return *p_; }
    SPP_HIDDEN explicit operator bool() const noexcept { return p_ != nullptr; }
    SPP_HIDDEN operator T*() const noexcept { return p_; }

    SPP_HIDDEN void reset() noexcept { Intrusive().swap(*this); }
    SPP_HIDDEN void swap(Intrusive& o) noexcept { std::swap(p_, o.p_); }

    SPP_HIDDEN friend bool operator==(const Intrusive& a, const Intrusive& b) noexcept { return a.p_ == b.p_; }
    SPP_HIDDEN friend bool operator==(const Intrusive& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }
    // A comparison with a raw pointer compares the addresses. It makes no temporary wrapper,
    // so the reference count does not change. C++20 gives "!=" and the reversed forms. The
    // exact "T*" form wins over the built-in comparison through "operator T*".
    SPP_HIDDEN friend bool operator==(const Intrusive& a, T* b) noexcept { return a.p_ == b; }
    SPP_HIDDEN friend bool operator==(const Intrusive& a, const T* b) noexcept { return a.p_ == b; }

private:
    template <class, auto, auto>
    friend class Intrusive;
    // Gives up the pointer without a Release call. The converting move uses it.
    SPP_HIDDEN void detach_() noexcept { p_ = nullptr; }

    T* p_ = nullptr;
};
template <class T, auto Add, auto Release>
struct is_handle<Intrusive<T, Add, Release>> : std::true_type {};

}  // namespace spp
