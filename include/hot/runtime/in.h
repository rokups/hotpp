// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::In<T>: the C++ type of a plain parameter of a script function (a read only input).
#pragma once
#include <type_traits>

namespace hot
{

// The parameter is a copy of T when T is trivially copyable and has 16 bytes or less. Else it is
// "const T&". The translator never computes sizeof: the C++ compiler picks the form.
template <class T>
using In = std::conditional_t<std::is_trivially_copyable_v<T> && sizeof(T) <= 16, T, const T&>;

} // namespace hot
