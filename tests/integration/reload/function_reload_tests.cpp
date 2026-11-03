// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of free functions and static methods. A function has no slot and no manifest
// entry. A method of a class calls it directly. A new generation has the new body. The tests use
// the backend that HOT_BACKEND selected.
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

template <class T> T* fld(hot::ShellCore* shell, const char* name)
{
    return hot::field_ptr<T>(shell, name);
}

// The start of generation 1: a Hero and a Helper after the first commit and the first tick.
void start(hot::Backend& backend, Session& s, hot::Instance& helper)
{
    hot::LoadResult v1 = backend.load(source("functions_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    helper = s.loader.find("Helper")->create();
    s.world.Add(static_cast<Actor*>(helper.native));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

TEST_P(BackendTests, FunctionsAndStaticsRunTheScriptBodies)
{
    Session s;
    hot::Instance helper;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, helper));
    hot::ShellCore* hero = s.inst.shell;
    EXPECT_EQ(*fld<int>(hero, "sum"), 5);          // Add(2, 3)
    EXPECT_EQ(*fld<int>(hero, "weight"), 12);      // Hero.Weight(10) + Weight(0)
    EXPECT_EQ(*fld<int>(hero, "fact"), 120);       // the recursive Fact(5)
    EXPECT_EQ(*fld<int>(hero, "scaled"), 12);      // the default argument: Scale(4) is Scale(4, 3)
    EXPECT_EQ(*fld<int>(hero, "scaled2"), 20);     // Scale(4, 5)
    EXPECT_EQ(*fld<int>(hero, "twice"), 42);       // the static method of a struct
    EXPECT_EQ(*fld<int>(helper.shell, "seen"), 103); // a static method of another class
}

TEST_P(BackendTests, FunctionsAndStaticsUseTheNewBodyAfterReload)
{
    hot::LoadResult v2 = backend->load(source("functions_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    hot::Instance helper;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, helper));
    ASSERT_EQ(*fld<int>(s.inst.shell, "sum"), 5);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);
    s.world.Tick(kDt); // the methods run the new code
    hot::ShellCore* hero = s.inst.shell;
    EXPECT_EQ(*fld<int>(hero, "sum"), 105);       // Add(2, 3) + 100
    EXPECT_EQ(*fld<int>(hero, "weight"), 14);     // Hero.Weight(10) + Weight(0), each with + 2
    EXPECT_EQ(*fld<int>(hero, "fact"), 120);      // the recursive function did not change
    EXPECT_EQ(*fld<int>(hero, "scaled"), 12);
    EXPECT_EQ(*fld<int>(hero, "twice"), 42);
    EXPECT_EQ(*fld<int>(helper.shell, "seen"), 102 + 102); // Hero.Weight(100) + Add(1, 1)
}

} // namespace
