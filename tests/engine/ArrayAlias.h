// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A host that has its own fixed-size array template. The module attribute
// "[array(\"hostns::FixedArray<$T, $N>\")];" uses it.
#pragma once

#include <array>
#include <cstddef>

namespace hostns
{
template <class T, std::size_t N> using FixedArray = std::array<T, N>;
}
