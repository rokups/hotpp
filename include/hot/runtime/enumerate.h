// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The helpers of the script ranges "enumerate(xs)" and "range(n)".
#pragma once
#include "hot/runtime/hidden.h"

#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

namespace hot
{

// The range of "enumerate(xs)". An item is a pair: the index as size_t and the element. The
// element is a reference when the iterator of "xs" gives a reference. So the binding
// "var (i, x)" gives the index by value and "x" as a reference to the element. A container that
// is an lvalue is not copied. A container that is an rvalue moves into the range.
template <class C> class Enumerated
{
public:
    using Iter = decltype(std::begin(std::declval<C&>()));
    using Ref = decltype(*std::declval<Iter&>());
    using Item = std::pair<std::size_t, Ref>;

    class Cursor
    {
    public:
        HOT_HIDDEN Cursor(Iter it, std::size_t index)
            : it_(it)
            , index_(index)
        {
        }
        HOT_HIDDEN Item operator*() const { return Item(index_, *it_); }
        HOT_HIDDEN Cursor& operator++()
        {
            ++it_;
            ++index_;
            return *this;
        }
        HOT_HIDDEN bool operator!=(const Cursor& o) const { return it_ != o.it_; }
        HOT_HIDDEN bool operator==(const Cursor& o) const { return it_ == o.it_; }

    private:
        Iter it_;
        std::size_t index_;
    };

    HOT_HIDDEN explicit Enumerated(C&& c)
        : c_(std::forward<C>(c))
    {
    }
    HOT_HIDDEN Cursor begin() { return Cursor(std::begin(c_), 0); }
    HOT_HIDDEN Cursor end() { return Cursor(std::end(c_), 0); }

private:
    C c_; // a reference type for an lvalue, a value type for an rvalue
};

template <class C> HOT_HIDDEN Enumerated<C> enumerate(C&& c)
{
    return Enumerated<C>(std::forward<C>(c));
}

// The range of "range(a, b)": the values a, a + 1, ... b - 1.
template <class T> class IntRange
{
public:
    class Cursor
    {
    public:
        HOT_HIDDEN explicit Cursor(T v)
            : v_(v)
        {
        }
        HOT_HIDDEN T operator*() const { return v_; }
        HOT_HIDDEN Cursor& operator++()
        {
            ++v_;
            return *this;
        }
        HOT_HIDDEN bool operator!=(const Cursor& o) const { return v_ < o.v_; }

    private:
        T v_;
    };

    HOT_HIDDEN IntRange(T a, T b)
        : a_(a)
        , b_(b < a ? a : b)
    {
    }
    HOT_HIDDEN Cursor begin() const { return Cursor(a_); }
    HOT_HIDDEN Cursor end() const { return Cursor(b_); }

private:
    T a_, b_;
};

template <class A, class B> HOT_HIDDEN IntRange<std::common_type_t<A, B>> range(A a, B b)
{
    return IntRange<std::common_type_t<A, B>>(a, b);
}

// The range of "range(n)": the values 0 ... n - 1.
template <class N> HOT_HIDDEN IntRange<N> range(N n)
{
    return IntRange<N>(N(0), n);
}

} // namespace hot
