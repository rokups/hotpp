// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A second standard-like library "mocklib" for the tests of the module attribute "lib". Its types
// are other types than the std types. So the runtime has no default bounds check for them.
#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mocklib
{
template <class T, class A = std::allocator<T>> class vector : public std::vector<T, A>
{
public:
    using std::vector<T, A>::vector;
};

template <class T, std::size_t N> struct array : std::array<T, N>
{
};

template <class K, class V, class C = std::less<K>> class map : public std::map<K, V, C>
{
};

template <class C, class Tr = std::char_traits<C>> class basic_string : public std::basic_string<C, Tr>
{
public:
    basic_string() = default;
    basic_string(const C* s)
        : std::basic_string<C, Tr>(s)
    {
    }
};
using string = basic_string<char>;
} // namespace mocklib
