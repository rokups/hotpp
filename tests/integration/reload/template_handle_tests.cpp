// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script class as a template argument uses the handle storage (task 48). A field of type
// std.vector<Recruit> holds std::shared_ptr handles. A reload keeps the objects, also when the
// layout of the class changes. They use the backend that HOT_BACKEND selected.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <string>

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

// Two ticks: the vector gets two handles. The first one is bumped twice, the map shares the objects.
TEST_P(BackendTests, VectorOfScriptClassHoldsHandles)
{
    hot::LoadResult v1 = backend->load(source("targ_handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Platoon"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "count"), 1);
    EXPECT_EQ(fld(s, "sum"), 4); // 3 plus one Bump
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "count"), 2);
    EXPECT_EQ(fld(s, "grouped"), 2);
    EXPECT_EQ(fld(s, "sum"), 8); // the first object is 5, the second is 3
}

// A new field in Recruit changes its layout. The vector keeps its handles and the objects keep their values.
TEST_P(BackendTests, VectorOfScriptClassSurvivesLayoutChange)
{
    hot::LoadResult v1 = backend->load(source("targ_handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("targ_handle_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Platoon"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit is at the end of this frame: the old code runs
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    EXPECT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(fld(s, "count"), 5);
    EXPECT_EQ(fld(s, "grouped"), 5);
    // The first object has hp 8 (one Bump in each of 5 ticks, from 3). The others have hp 3.
    // Each object has armor 2: the default of the new field. The sum is 8 + 3 * 4 + 5 * 2.
    EXPECT_EQ(fld(s, "sum"), 30);
}

// A script class in a std::function type and in a std::variant is a handle. The lambda takes
// a handle, and the access functions name the handle type.
TEST_P(BackendTests, FunctionAndVariantOfScriptClassUseHandles)
{
    hot::LoadResult v1 = backend->load(source("targ_func_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Camp"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "fnres"), 6);
    EXPECT_EQ(fld(s, "varres"), 3);
}

} // namespace
