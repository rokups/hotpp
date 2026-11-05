// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// view_type_name(): the name follows the CMake option HOT_VIEW_TYPE.
#include "hot/config.h"

#include <string>

#include <gtest/gtest.h>

TEST(ViewType, NameFollowsOption)
{
#ifdef HOT_VIEW_STD
    EXPECT_EQ(std::string(hot::view_type_name()), "std");
#else
    EXPECT_EQ(std::string(hot::view_type_name()), "hot");
#endif
}
