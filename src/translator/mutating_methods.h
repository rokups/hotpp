// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The names of the container methods that can change the shape of a container. A call of such a
// method can move or remove the elements. A "ref" to an element can then dangle.
#pragma once

#include <array>
#include <string_view>

namespace hotc
{

inline constexpr std::array<std::string_view, 26> kShapeChangeMethods = {"push_back", "emplace_back", "pop_back",
    "push_front", "pop_front", "emplace_front", "emplace", "emplace_hint", "insert", "insert_or_assign", "try_emplace",
    "erase", "clear", "resize", "reserve", "shrink_to_fit", "assign", "swap", "rehash", "merge", "extract", "append",
    "replace", "remove", "remove_if", "splice"};

constexpr bool is_mutating_method(std::string_view name)
{
    for (std::string_view m : kShapeChangeMethods)
        if (m == name)
            return true;
    return false;
}

} // namespace hotc
