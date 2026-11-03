// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::In. It is the C++ type of a plain parameter of a script function.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace
{

struct Vec3
{
    float x, y, z;
};
struct Vec4
{
    float x, y, z, w;
};
struct Big
{
    float v[5];
};
struct Handle
{
    Handle() = default;
    Handle(const Handle&) {}
    int v = 0;
};

TEST(In, SmallTriviallyCopyableTypesAreValues)
{
    static_assert(std::is_same_v<hot::In<int>, int>);
    static_assert(std::is_same_v<hot::In<double>, double>);
    static_assert(std::is_same_v<hot::In<Vec3>, Vec3>);
    static_assert(std::is_same_v<hot::In<Vec4>, Vec4>); // exactly 16 bytes
    static_assert(std::is_same_v<hot::In<int*>, int*>);
    SUCCEED();
}

TEST(In, LargeTypesAreConstReferences)
{
    static_assert(sizeof(Big) > 16);
    static_assert(std::is_same_v<hot::In<Big>, const Big&>);
    SUCCEED();
}

TEST(In, NotTriviallyCopyableTypesAreConstReferences)
{
    static_assert(std::is_same_v<hot::In<std::string>, const std::string&>);
    static_assert(std::is_same_v<hot::In<Handle>, const Handle&>);
    static_assert(std::is_same_v<hot::In<std::shared_ptr<int>>, const std::shared_ptr<int>&>);
    SUCCEED();
}

} // namespace
