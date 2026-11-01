// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests for weak handles (task 38) with the backend that HOT_BACKEND selected.
// A child holds its parent through "[weak]", so the parent dies when the script lets it go.
// A reload that makes a strong field weak resets the field to null.
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

TEST_P(BackendTests, WeakParentDiesAndReadsNull)
{
    hot::LoadResult v1 = backend->load(source("weak_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Keeper"));
    s.commit_at_frame_boundary();
    auto field = [&](const char* name)
    {
        int* p = hot::field_ptr<int>(s.inst.shell, name);
        EXPECT_NE(p, nullptr) << name;
        return p ? *p : -100;
    };

    // Frame 1: the parent lives while the local holds it. The weak read gives the parent.
    s.world.Tick(kDt);
    EXPECT_EQ(field("parent_hp"), 9);
    EXPECT_EQ(field("parent_alive"), 1);
    // The local was the only strong handle of the parent: the parent is gone after the block.
    EXPECT_EQ(field("deaths"), 1);
    EXPECT_EQ(field("held_alive"), 1);

    // Frame 2: the child reads its weak parent as null.
    s.world.Tick(kDt);
    EXPECT_EQ(field("parent_alive"), 0);
    EXPECT_EQ(field("deaths"), 1);
}

TEST_P(BackendTests, StrongToWeakFieldResetsOnReload)
{
    hot::LoadResult v1 = backend->load(source("weak_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("weak_v2.hot"), 2);
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
    s.world.Tick(kDt);
    EXPECT_EQ(field("held_alive"), 1);
    EXPECT_EQ(field("deaths"), 1);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStderr();
    s.world.Tick(kDt); // generation 1 runs this frame; the reload commits after it
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_NE(log.find("field held changed its type"), std::string::npos) << log;
    // The strong handle of generation 1 is gone: the field is null and the object died.
    EXPECT_EQ(field("held_alive"), 0);
    EXPECT_EQ(field("deaths"), 2);
    // The weak parent field of the child kept its type: it migrated and still reads null.
    EXPECT_EQ(field("parent_alive"), 0);
    EXPECT_EQ(field("frames"), 3);
}

} // namespace
