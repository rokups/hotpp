// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script that uses "using std;" and "using Name = type;". The script runs with the backend that
// HOT_BACKEND selected. The aliases of a script class are handles, and the aliases of a native
// type are that type.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

int fld(Session& s, const char* name)
{
    auto* p = hot::field_ptr<int>(s.inst.shell, name);
    return p ? *p : -1;
}

TEST_P(BackendTests, NamespaceUsingAndTypeAliasesRun)
{
    hot::LoadResult v1 = backend->load(source("using_alias_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Platoon"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "count"), 1);
    EXPECT_EQ(fld(s, "sum"), 11); // hp 4 after one Bump, plus the number 7
    EXPECT_EQ(fld(s, "has_lead"), 1);
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "count"), 2);
    EXPECT_EQ(fld(s, "sum"), 11); // the first object is still the first element
}

} // namespace
