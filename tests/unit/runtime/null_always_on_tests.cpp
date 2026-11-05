// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests that hot::as_ref throws on a null value also when HOT_RUNTIME_CHECKS is 0. This file
// sets the macro to 0 before the include. The types are local to this file, so the templates
// of the other test files (compiled with the macro on) do not share an instantiation with it.
#ifndef HOT_RUNTIME_CHECKS
#define HOT_RUNTIME_CHECKS 0
#endif
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{

struct Probe : std::enable_shared_from_this<Probe>
{
    int v = 1;
};

TEST(NullAlwaysOn, AsRefOfNullHandleThrows)
{
    std::shared_ptr<Probe> h;
    EXPECT_THROW((void)hot::as_ref(h), hot::NullReference);
}

TEST(NullAlwaysOn, AsRefOfNullPointerThrows)
{
    Probe* p = nullptr;
    EXPECT_THROW((void)hot::as_ref(p), hot::NullReference);
    Probe v;
    EXPECT_EQ(&hot::as_ref(&v), &v);
}

TEST(NullAlwaysOn, AsRefOfNullCheckedPtrThrows)
{
    hot::CheckedPtr<Probe> p;
    EXPECT_THROW((void)hot::as_ref(p), hot::NullReference);
}

TEST(NullAlwaysOn, AsPtrOfNullGivesNullptr)
{
    std::shared_ptr<Probe> h;
    EXPECT_EQ(hot::as_ptr(h), nullptr);
    Probe* p = nullptr;
    EXPECT_EQ(hot::as_ptr(p), nullptr);
}

} // namespace
