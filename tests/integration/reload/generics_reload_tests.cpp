// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The hot reload of script generics (doc/generics.md). A live object of the instantiation
// Counter<int> keeps its state across the reload. Its methods then run the new generic body.
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

int field(hot::ShellCore* shell, const char* name)
{
    return *hot::field_ptr<int>(shell, name);
}

TEST_P(BackendTests, GenericInstantiationsReloadLive)
{
    hot::LoadResult v1 = backend->load(source("generics_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("generics_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    EXPECT_EQ(v2.reason, hot::ReloadReason::None);

    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Keeper"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    hot::ShellCore* keeper = s.inst.shell;
    EXPECT_EQ(field(keeper, "got"), 2);   // Counter<int>.Add(2) on a new object
    EXPECT_EQ(field(keeper, "pair"), 7);  // Pair<int, float>.First()
    EXPECT_EQ(field(keeper, "twice"), 6); // Twice<int>(3)

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the old code runs, then the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(field(keeper, "got"), 4);
    s.world.Tick(kDt); // the new bodies run on the same live Counter<int> object
    EXPECT_EQ(field(keeper, "got"), 4 + 2 * 10);
    EXPECT_EQ(field(keeper, "pair"), 8);
    EXPECT_EQ(field(keeper, "twice"), 7);
}

} // namespace
