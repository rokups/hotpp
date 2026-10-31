// Reload tests for classes that use each other through handles (task 47) with the backend that
// HOT_BACKEND selected. Child comes before Parent in the script, and each class uses the other.
// The child holds its parent through "[weak]", so the parent dies when the script lets it go.
// Generation 2 gives a field of Child a new name ([alias]): the value migrates.
//
// CMake defines the macro HOT_SOURCE_DIR.
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

TEST_P(BackendTests, CycleHandlesReloadAndTheWeakParentDies)
{
    hot::LoadResult v1 = backend->load(source("cycle_handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("cycle_handle_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Keeper"));
    s.commit_at_frame_boundary();
    auto field = [&](const char* name)
    {
        int* p = hot::field_ptr<int>(s.inst.shell, name);
        EXPECT_NE(p, nullptr) << name;
        return p ? *p : -100;
    };

    // Frame 1: the parent adopts the child. The child reads the parent through its weak handle.
    s.world.Tick(kDt);
    EXPECT_EQ(field("parent_hp"), 9 + 5);
    EXPECT_EQ(field("parent_alive"), 1);
    EXPECT_EQ(field("kid_age"), 5);
    EXPECT_EQ(field("deaths"), 0);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // generation 1 runs this frame; the reload commits after it
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);

    // Frame 3: generation 2 reads the field with the new name. The weak handle still gives the parent.
    s.world.Tick(kDt);
    EXPECT_EQ(field("parent_hp"), 9 + 5);
    EXPECT_EQ(field("parent_alive"), 1);
    EXPECT_EQ(field("kid_age"), 5);
    EXPECT_EQ(field("deaths"), 0);

    // Frame 4: the script drops the only strong handle of the parent. The weak probe reads null.
    s.world.Tick(kDt);
    EXPECT_EQ(field("frames"), 4);
    EXPECT_EQ(field("deaths"), 1);
    EXPECT_EQ(field("parent_alive"), 0);
    EXPECT_EQ(field("parent_hp"), -1);
    EXPECT_EQ(field("kid_age"), -1);
}

} // namespace
