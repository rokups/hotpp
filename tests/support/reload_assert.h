// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Test assertions for a ReloadResult. ASSERT_RELOAD_OK and EXPECT_RELOAD_OK check that the
// result is Applied or Pending. A failure prints the status, the reason and the error text.
#pragma once

#include "hot/runtime/reload_result.h"

#include <gtest/gtest.h>

namespace test_support
{

inline ::testing::AssertionResult reload_ok(const char* expr, const hot::ReloadResult& r)
{
    if (r)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << expr << " gave " << hot::to_string(r.status) << " ("
                                         << hot::to_string(r.reason) << "): " << r.error;
}

} // namespace test_support

#define ASSERT_RELOAD_OK(expr) ASSERT_PRED_FORMAT1(::test_support::reload_ok, (expr))
#define EXPECT_RELOAD_OK(expr) EXPECT_PRED_FORMAT1(::test_support::reload_ok, (expr))
