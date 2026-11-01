// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of the script constructor, the destructor and "new" of a script class (S4)
// with the backend that HOT_BACKEND selected.
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

template <class T> T* fld(Session& s, const char* name)
{
    return hot::field_ptr<T>(s.inst.shell, name);
}

// A script object builds a Squad with "new". The constructor runs once for each new object.
TEST_P(BackendTests, NewBuildsAScriptObjectAndRunsTheConstructor)
{
    hot::LoadResult v1 = backend->load(source("ctor_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spawner"));
    s.commit_at_frame_boundary();
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_EQ(*fld<int>(s, "spawned"), 2);
    EXPECT_EQ(*fld<std::string>(s, "last"), "unit"); // base(label) reached the base
    EXPECT_EQ(*fld<float>(s, "seen_x"), 2.0f); // the constructor body ran
    // The second "new" replaced the first Squad. Its destructor ran once.
    EXPECT_NE(log.find("[ctor_v1] unit gone"), std::string::npos) << log;
}

// The engine factory runs the constructor with default arguments.
TEST_P(BackendTests, FactoryRunsTheConstructorOnce)
{
    hot::LoadResult v1 = backend->load(source("ctor_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Squad"));
    s.commit_at_frame_boundary();
    EXPECT_EQ(*fld<int>(s, "ctor_runs"), 1);
    EXPECT_EQ(*fld<int>(s, "size"), 5);
}

// A reload does not run the constructor again for an old object.
// A "new" after the reload uses the new constructor body.
TEST_P(BackendTests, ReloadDoesNotRerunTheConstructor)
{
    hot::LoadResult v1 = backend->load(source("ctor_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("ctor_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spawner"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<float>(s, "seen_x"), 2.0f);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    testing::internal::CaptureStdout();
    s.world.Tick(kDt); // the first Tick of generation 2
    const std::string log = testing::internal::GetCapturedStdout();
    // The Squad of the frame before was built by generation 1. The destructor of generation 2 runs.
    EXPECT_NE(log.find("[ctor_v2] unit gone"), std::string::npos) << log;
    EXPECT_EQ(*fld<int>(s, "spawned"), 3);
    EXPECT_EQ(*fld<float>(s, "seen_x"), 20.0f); // the new Squad uses the constructor of generation 2
}

// A Squad that exists before the reload keeps its state. The constructor does not run again.
TEST_P(BackendTests, OldObjectKeepsItsConstructorState)
{
    hot::LoadResult v1 = backend->load(source("ctor_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("ctor_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Squad"));
    s.commit_at_frame_boundary();
    ASSERT_EQ(*fld<int>(s, "ctor_runs"), 1);
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(*fld<int>(s, "ctor_runs"), 1);
    EXPECT_EQ(*fld<int>(s, "size"), 5);
}

} // namespace
