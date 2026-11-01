// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::lock, the read of a weak handle ("[weak] T x").
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

namespace
{

struct Node
{
    int value = 0;
};

TEST(WeakHandleRuntime, LockGivesTheStrongHandle)
{
    auto strong = std::make_shared<Node>();
    strong->value = 7;
    std::weak_ptr<Node> weak = strong;
    std::shared_ptr<Node> got = hot::lock(weak);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got.get(), strong.get());
    EXPECT_EQ(got->value, 7);
    static_assert(std::is_same_v<decltype(hot::lock(weak)), std::shared_ptr<Node>>);
}

TEST(WeakHandleRuntime, ExpiredGivesNull)
{
    std::weak_ptr<Node> weak;
    {
        auto strong = std::make_shared<Node>();
        weak = strong;
    }
    EXPECT_EQ(hot::lock(weak), nullptr);
}

TEST(WeakHandleRuntime, EmptyGivesNull)
{
    const std::weak_ptr<Node> weak;
    EXPECT_EQ(hot::lock(weak), nullptr);
}

} // namespace
