// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the operators of a script struct with a loaded script. The first Tick of the holder
// runs each use once and writes the results to int fields. The tests use the backend that
// HOT_BACKEND selected. The script sorts a std::vector of the struct with std::sort, so the
// generated C++ operator< works with native code.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
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

void start(hot::Backend& backend, Session& s)
{
    hot::LoadResult v1 = backend.load(source("operators_struct.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

// Each result is 1 when the use gave the expected value.
void expect_one(hot::Backend& backend, const char* field)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(backend, s));
    EXPECT_EQ(field_int(s, "ticks"), 1);
    EXPECT_EQ(field_int(s, field), 1) << field;
}

TEST_P(BackendTests, StructOperatorAdd) { expect_one(*backend, "r_add"); }
TEST_P(BackendTests, StructOperatorNegate) { expect_one(*backend, "r_neg"); }
TEST_P(BackendTests, StructOperatorMultiplyByFloat) { expect_one(*backend, "r_mul"); }
TEST_P(BackendTests, StructOperatorLess) { expect_one(*backend, "r_lt"); }
TEST_P(BackendTests, StructOperatorGreater) { expect_one(*backend, "r_gt"); }
TEST_P(BackendTests, StructOperatorIndexWritesThroughReference) { expect_one(*backend, "r_index"); }
TEST_P(BackendTests, StructCompoundOperatorsAreGenerated) { expect_one(*backend, "r_compound"); }
TEST_P(BackendTests, StructGeneratedEqual) { expect_one(*backend, "r_eq"); }
TEST_P(BackendTests, StructGeneratedNotEqual) { expect_one(*backend, "r_ne"); }
TEST_P(BackendTests, StructWithoutWrittenEqualCompares) { expect_one(*backend, "r_pair_eq"); }
TEST_P(BackendTests, StructWithoutWrittenEqualDetectsDifference) { expect_one(*backend, "r_pair_ne"); }
TEST_P(BackendTests, StructOperatorLessWorksWithStdSort) { expect_one(*backend, "r_sorted"); }

} // namespace
