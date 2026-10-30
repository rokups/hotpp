// Test assertions for a ReloadResult. ASSERT_RELOAD_OK and EXPECT_RELOAD_OK check that the
// result is Applied or Pending. A failure prints the status, the reason and the error text.
#pragma once

#include <gtest/gtest.h>

#include "spp/runtime/reload_result.h"

namespace test_support {

inline ::testing::AssertionResult reload_ok(const char* expr, const spp::ReloadResult& r)
{
    if (r) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << expr << " gave " << spp::to_string(r.status) << " ("
                                         << spp::to_string(r.reason) << "): " << r.error;
}

}  // namespace test_support

#define ASSERT_RELOAD_OK(expr) ASSERT_PRED_FORMAT1(::test_support::reload_ok, (expr))
#define EXPECT_RELOAD_OK(expr) EXPECT_PRED_FORMAT1(::test_support::reload_ok, (expr))
