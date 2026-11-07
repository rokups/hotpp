// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The run-time guard of a "foreach" loop (doc/statements-errors.md, "Change of a container in a
// foreach"). The translator writes
//     for (auto __i = hot::iter(c); __i.next();) { auto& x = __i.ref(); ... }
// A change of the shape of the container during the loop gives hot::ContainerChanged. It never
// gives undefined behavior. The guard has three kinds:
//  - An indexed container (an array, std::vector, std::string, a span): the guard walks an index.
//    "next()" and "ref()" compare the address and the size with the values at the loop start.
//  - A container with unique keys (std::map, std::unordered_map, std::set): the guard keeps a copy
//    of the current key. "next()" and "ref()" find the key again. They do not use an old iterator.
//  - A temporary range (the result of a call): the body cannot name it, so the guard walks it
//    with a plain iterator.
// A named range of a different kind (std::list, std::multimap) is a compile error.
// "hot::iter_path(getter)" evaluates the range again at each step: the range is a checked path
// (an element of a container, a field behind a handle). "Enum" is the form of "enumerate(c)".
#pragma once
#include "hot/runtime/errors.h"
#include "hot/runtime/hidden.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace hot
{
namespace detail
{

template <class C, class = void> struct has_size : std::false_type
{
};
template <class C> struct has_size<C, std::void_t<decltype(std::size(std::declval<C&>()))>> : std::true_type
{
};

template <class C> using begin_t = decltype(std::begin(std::declval<C&>()));

template <class C, class = void> struct is_indexed : std::false_type
{
};
template <class C>
struct is_indexed<C,
    std::void_t<decltype(std::size(std::declval<C&>())), decltype(std::declval<C&>()[std::size_t{}]),
        typename std::iterator_traits<begin_t<C>>::iterator_category>>
    : std::is_base_of<std::random_access_iterator_tag, typename std::iterator_traits<begin_t<C>>::iterator_category>
{
};

// A container with unique keys: "insert(value)" gives a pair (iterator, bool).
template <class C, class = void> struct is_unique_keyed : std::false_type
{
};
template <class C>
struct is_unique_keyed<C,
    std::void_t<typename C::key_type, decltype(std::declval<C&>().find(std::declval<const typename C::key_type&>())),
        decltype(std::declval<C&>().insert(std::declval<const typename C::value_type&>()).second)>> : std::true_type
{
};

template <class C, class = void> struct has_buckets : std::false_type
{
};
template <class C>
struct has_buckets<C, std::void_t<decltype(std::declval<const C&>().bucket_count())>> : std::true_type
{
};

template <class C, class V> HOT_HIDDEN const auto& key_of(const V& v)
{
    if constexpr (std::is_same_v<std::remove_cv_t<typename C::key_type>, std::remove_cv_t<V>>)
        return v;
    else
        return v.first;
}

template <class R> struct RefSource
{
    R& r;
    HOT_HIDDEN R& get() { return r; }
};
template <class V> struct OwnSource
{
    V v;
    HOT_HIDDEN V& get() { return v; }
};
template <class G> struct PathSource
{
    G g;
    HOT_HIDDEN decltype(auto) get() { return g(); }
};

template <class S> struct is_own : std::false_type
{
};
template <class V> struct is_own<OwnSource<V>> : std::true_type
{
};

template <class T> struct dependent_false : std::false_type
{
};

} // namespace detail

template <class S, bool Enum = false> class Iter
{
    using C = std::remove_reference_t<decltype(std::declval<S&>().get())>;
    using P = std::remove_const_t<C>;
    using It = detail::begin_t<C>;
    static constexpr bool kOwned = detail::is_own<S>::value;
    static constexpr bool kIndexed = detail::is_indexed<C>::value;
    static constexpr bool kKeyed = !kIndexed && detail::is_unique_keyed<P>::value;
    static constexpr bool kChecked = kIndexed || kKeyed; // the address and the size are compared
    static_assert(kOwned || kChecked,
        "foreach over this container needs an index or a unique key; copy the elements to a std.vector first");

    struct NoKey
    {
    };
    template <class Q, bool K> struct KeyOf
    {
        using type = NoKey;
    };
    template <class Q> struct KeyOf<Q, true>
    {
        using type = std::remove_cv_t<typename Q::key_type>;
    };
    using Key = typename KeyOf<P, kKeyed>::type;

public:
    HOT_HIDDEN explicit Iter(S s)
        : s_(std::move(s))
    {
        C& c = s_.get();
        addr_ = std::addressof(c);
        if constexpr (kChecked)
            size_ = std::size(c);
        if constexpr (kKeyed && detail::has_buckets<P>::value)
            buckets_ = c.bucket_count();
    }

    // Goes to the next element. False at the end.
    HOT_HIDDEN bool next()
    {
        C& c = checked();
        if (started_)
            ++n_;
        if constexpr (kIndexed)
        {
            started_ = true;
            return n_ < size_;
        }
        else if constexpr (kKeyed)
        {
            if (!started_)
            {
                it_ = std::begin(c);
            }
            else
            {
                it_ = c.find(*key_);
                if (it_ == std::end(c))
                    throw ContainerChanged();
                ++it_;
            }
            started_ = true;
            if (it_ == std::end(c))
                return false;
            key_.emplace(detail::key_of<P>(*it_));
            return true;
        }
        else
        {
            if (!started_)
                it_ = std::begin(c);
            else
                ++it_;
            started_ = true;
            return it_ != std::end(c);
        }
    }

    // The current element, after the same test as "next()".
    HOT_HIDDEN decltype(auto) ref()
    {
        C& c = checked();
        if constexpr (kIndexed)
            return item(c[n_]);
        else if constexpr (kKeyed)
        {
            auto it = c.find(*key_);
            if (it == std::end(c))
                throw ContainerChanged();
            return item(*it);
        }
        else
            return item(*it_);
    }

private:
    template <class T> HOT_HIDDEN decltype(auto) item(T&& x)
    {
        if constexpr (Enum)
            return std::pair<std::size_t, T&&>(n_, std::forward<T>(x));
        else
            return std::forward<T>(x);
    }

    HOT_HIDDEN C& checked()
    {
        C& c = s_.get();
        if constexpr (kChecked)
        {
            if (std::addressof(c) != addr_ || std::size(c) != size_)
                throw ContainerChanged();
            if constexpr (kKeyed && detail::has_buckets<P>::value)
                if (c.bucket_count() != buckets_)
                    throw ContainerChanged();
        }
        return c;
    }

    S s_;
    const void* addr_ = nullptr;
    std::size_t size_ = 0;
    std::size_t buckets_ = 0;
    std::size_t n_ = 0; // the index of the current element
    bool started_ = false;
    std::conditional_t<kIndexed, char, It> it_{};
    std::conditional_t<kKeyed, std::optional<Key>, char> key_{};
};

// A range with a simple root (a local, a parameter, a field of "this"): it cannot move during the
// loop. An lvalue binds by reference. A temporary is kept in the guard.
template <bool Enum = false, class R> HOT_HIDDEN auto iter(R&& r)
{
    if constexpr (std::is_lvalue_reference_v<R>)
        return Iter<detail::RefSource<std::remove_reference_t<R>>, Enum>(
            detail::RefSource<std::remove_reference_t<R>>{r});
    else
        return Iter<detail::OwnSource<std::remove_cv_t<R>>, Enum>(detail::OwnSource<std::remove_cv_t<R>>{std::move(r)});
}

// A range that is a checked path: "get()" evaluates the path again at each step.
template <bool Enum = false, class G> HOT_HIDDEN auto iter_path(G g)
{
    using R = decltype(g());
    if constexpr (std::is_lvalue_reference_v<R>)
        return Iter<detail::PathSource<G>, Enum>(detail::PathSource<G>{std::move(g)});
    else
        return Iter<detail::OwnSource<std::remove_cv_t<std::remove_reference_t<R>>>, Enum>(
            detail::OwnSource<std::remove_cv_t<std::remove_reference_t<R>>>{g()});
}

} // namespace hot
