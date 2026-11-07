// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::span<T>: a view of a contiguous range, for C++17. It works as std::span<T> with a dynamic
// extent. A script type "T[]" can use it when the std::span of C++20 is not available.
// The member operator[] has no check. The function hot::index (see deref.h) has the check.
#pragma once

#include <array>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace hot
{

inline constexpr std::size_t dynamic_extent = static_cast<std::size_t>(-1);

template <class T> class span;

namespace detail
{
// True when a pointer to "U" converts to a pointer to "T" by a qualification conversion only.
template <class U, class T> inline constexpr bool span_qual_convertible_v = std::is_convertible_v<U (*)[], T (*)[]>;

template <class C> struct is_hot_span : std::false_type
{
};
template <class T> struct is_hot_span<span<T>> : std::true_type
{
};
template <class C> struct is_std_array : std::false_type
{
};
template <class T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type
{
};

// The element type of a range "R" (an lvalue of R): the result of std::data without the pointer.
// The substitution fails when R has no std::data or no std::size.
template <class R>
using span_range_element_t = std::remove_pointer_t<decltype(std::data(std::declval<R&>()))>;
template <class R, class = void> struct span_range_size_ok : std::false_type
{
};
template <class R>
struct span_range_size_ok<R, std::void_t<decltype(std::size(std::declval<R&>()))>> : std::true_type
{
};
} // namespace detail

template <class T> class span
{
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using reverse_iterator = std::reverse_iterator<iterator>;

    static constexpr std::size_t extent = dynamic_extent;

    constexpr span() noexcept = default;
    constexpr span(T* ptr, std::size_t count) noexcept : data_(ptr), size_(count) {}
    // The end is a pointer, as in std::span. An integer end (for example 0) selects (ptr, count).
    template <class End, std::enable_if_t<std::is_pointer_v<End> && std::is_convertible_v<End, T*>, int> = 0>
    constexpr span(T* first, End last) noexcept : data_(first), size_(static_cast<std::size_t>(last - first))
    {
    }

    template <class U, std::size_t N, std::enable_if_t<detail::span_qual_convertible_v<U, T>, int> = 0>
    constexpr span(U (&arr)[N]) noexcept : data_(arr), size_(N)
    {
    }
    template <class U, std::size_t N, std::enable_if_t<detail::span_qual_convertible_v<U, T>, int> = 0>
    constexpr span(std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N)
    {
    }
    template <class U, std::size_t N, std::enable_if_t<detail::span_qual_convertible_v<const U, T>, int> = 0>
    constexpr span(const std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N)
    {
    }

    // A range with std::data and std::size. An rvalue range is allowed only when T is const, as
    // std::span does: a view of a temporary range is safe only for the full expression.
    template <class R,
        std::enable_if_t<!detail::is_hot_span<std::remove_cv_t<std::remove_reference_t<R>>>::value
                && !detail::is_std_array<std::remove_cv_t<std::remove_reference_t<R>>>::value
                && !std::is_array_v<std::remove_cv_t<std::remove_reference_t<R>>>
                && (std::is_lvalue_reference_v<R> || std::is_const_v<T>)
                && detail::span_range_size_ok<R>::value
                && detail::span_qual_convertible_v<detail::span_range_element_t<R>, T>,
            int> = 0>
    constexpr span(R&& range) : data_(std::data(range)), size_(static_cast<std::size_t>(std::size(range)))
    {
    }

    // The conversion from span<U>, for example span<int> to span<const int>.
    template <class U, std::enable_if_t<detail::span_qual_convertible_v<U, T>, int> = 0>
    constexpr span(const span<U>& other) noexcept : data_(other.data()), size_(other.size())
    {
    }

    constexpr iterator begin() const noexcept { return data_; }
    constexpr iterator end() const noexcept { return data_ + size_; }
    constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
    constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

    constexpr reference front() const { return data_[0]; }
    constexpr reference back() const { return data_[size_ - 1]; }
    // No check here. Use hot::index for a checked access.
    constexpr reference operator[](std::size_t i) const { return data_[i]; }
    constexpr pointer data() const noexcept { return data_; }

    constexpr std::size_t size() const noexcept { return size_; }
    constexpr std::size_t size_bytes() const noexcept { return size_ * sizeof(T); }
    constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr span first(std::size_t count) const { return span(data_, count); }
    constexpr span last(std::size_t count) const { return span(data_ + (size_ - count), count); }
    constexpr span subspan(std::size_t offset, std::size_t count = dynamic_extent) const
    {
        return span(data_ + offset, count == dynamic_extent ? size_ - offset : count);
    }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

template <class T, std::size_t N> span(T (&)[N]) -> span<T>;
template <class T, std::size_t N> span(std::array<T, N>&) -> span<T>;
template <class T, std::size_t N> span(const std::array<T, N>&) -> span<const T>;
template <class R> span(R&&) -> span<detail::span_range_element_t<R>>;

// A view as the argument of a native span parameter (gap G22). It converts to each type S that
// has the constructor S(T*, size_t): eastl::span<T>, std::span<T> and span<const T>. The translator
// writes "hot::span_as(v)" when the native parameter is an eastl::span or a std::span.
template <class T> struct span_arg
{
    T* data_;
    std::size_t size_;
    template <class S, std::enable_if_t<!detail::is_hot_span<S>::value && std::is_constructible_v<S, T*, std::size_t>, int> = 0>
    constexpr operator S() const
    {
        return S(data_, size_);
    }
};
template <class V> constexpr auto span_as(const V& v) noexcept
{
    return span_arg<std::remove_reference_t<decltype(*v.data())>>{v.data(), v.size()};
}

} // namespace hot
