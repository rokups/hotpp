// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::weak_registry, the weak source of a native class that no handle owns.
#include "hot/runtime.h"

#include <gtest/gtest.h>

namespace
{

// A class that no smart pointer owns. The host tracks each object.
struct Font
{
    int size = 0;
};

} // namespace

namespace hot
{
template <> struct weak_ref_traits<::Font> : registered_weak_ref<::Font>
{
};
} // namespace hot

namespace
{

TEST(WeakRegistry, PointerReadsTheObjectWhileItIsTracked)
{
    Font f;
    f.size = 9;
    hot::weak_registry<Font>::add(&f);
    hot::CheckedPtr<Font> p = &f;
    EXPECT_EQ(p.get(), &f);
    EXPECT_EQ(p->size, 9);
    hot::weak_registry<Font>::remove(&f);
}

TEST(WeakRegistry, PointerReadsNullAfterRemove)
{
    Font f;
    hot::weak_registry<Font>::add(&f);
    hot::CheckedPtr<Font> p = &f;
    hot::weak_registry<Font>::remove(&f);
    EXPECT_EQ(p.get(), nullptr);
    EXPECT_FALSE(static_cast<bool>(p));
    EXPECT_THROW({ (void)p->size; }, hot::NullReference);
}

TEST(WeakRegistry, PointerOfAnAddressThatLivesAgainStaysNull)
{
    Font f;
    hot::weak_registry<Font>::add(&f);
    hot::CheckedPtr<Font> p = &f;
    hot::weak_registry<Font>::remove(&f);
    hot::weak_registry<Font>::add(&f); // a new object at the same address
    EXPECT_EQ(p.get(), nullptr);
    hot::weak_registry<Font>::remove(&f);
}

TEST(WeakRegistry, RawPointerOfAnUntrackedObjectThrows)
{
    Font f;
    hot::CheckedPtr<Font> p;
    EXPECT_THROW({ p = &f; }, hot::NullReference);
    EXPECT_EQ(p.get(), nullptr);
}

TEST(WeakRegistry, NullAndDoubleCallsAreSafe)
{
    Font f;
    hot::weak_registry<Font>::add(nullptr);
    hot::weak_registry<Font>::remove(&f);
    hot::weak_registry<Font>::add(&f);
    hot::weak_registry<Font>::add(&f);
    EXPECT_NE(hot::weak_registry<Font>::find(&f), nullptr);
    hot::weak_registry<Font>::remove(&f);
    EXPECT_EQ(hot::weak_registry<Font>::find(&f), nullptr);
}

} // namespace
