// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of script code that uses the operators of a script class on bare class values (boxes).
// The first Tick of the holder runs each use once and writes the results to int fields. The tests
// use the backend that HOT_BACKEND selected.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

int field_int(Session& s, const char* name)
{
    const int* p = hot::field_ptr<int>(s.inst.shell, name);
    return p ? *p : -1000;
}

// Each result is 1 when the use gave the expected value.
void expect_one(hot::Backend& backend, const char* field)
{
    hot::LoadResult v1 = backend.load(source("operators_use_class.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(field_int(s, "ticks"), 1);
    EXPECT_EQ(field_int(s, field), 1) << field;
}

} // namespace

TEST_P(BackendTests, ClassOperatorAddOfTwoBoxes) { expect_one(*backend, "r_add"); }
TEST_P(BackendTests, ClassOperatorMultiplyByNativeOperand) { expect_one(*backend, "r_mul_int"); }
TEST_P(BackendTests, ClassOperatorResultMemberAccess) { expect_one(*backend, "r_member"); }
TEST_P(BackendTests, ClassOperatorResultInVar) { expect_one(*backend, "r_var"); }
TEST_P(BackendTests, ClassOperatorNegate) { expect_one(*backend, "r_neg"); }
TEST_P(BackendTests, ClassOperatorEqual) { expect_one(*backend, "r_eq"); }
TEST_P(BackendTests, ClassOperatorNotEqualIsGenerated) { expect_one(*backend, "r_ne"); }
TEST_P(BackendTests, ClassOperatorLess) { expect_one(*backend, "r_lt"); }
TEST_P(BackendTests, ClassOperatorIndex) { expect_one(*backend, "r_index"); }
TEST_P(BackendTests, ClassCompoundOperatorInScript) { expect_one(*backend, "r_compound"); }
TEST_P(BackendTests, ClassOperatorChain) { expect_one(*backend, "r_chain"); }
TEST_P(BackendTests, ClassOperatorResultAssignedToField) { expect_one(*backend, "r_field"); }
