// A host that has its own fixed-size array template. The module attribute
// "[array(\"hostns::FixedArray<$T, $N>\")];" uses it.
#pragma once

#include <array>
#include <cstddef>

namespace hostns
{
template <class T, std::size_t N> using FixedArray = std::array<T, N>;
}
