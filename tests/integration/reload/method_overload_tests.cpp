// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of method overloads (gap G17). Generation 2 adds an overload. Generation 3
// removes one and changes a parameter type. A live object keeps its fields, and each call goes
// to the body of the overload that the argument types select.
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

int fld(hot::ShellCore* shell, const char* name) { return *hot::field_ptr<int>(shell, name); }

void start(hot::Backend& backend, Session& s, hot::Instance& child)
{
    hot::LoadResult v1 = backend.load(source("method_overload_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Ov"));
    child = s.loader.find("OvChild")->create();
    s.world.Add(static_cast<Actor*>(child.native));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

void reload(hot::Backend& backend, Session& s, const char* file, int gen)
{
    hot::LoadResult v = backend.load(source(file), gen);
    ASSERT_TRUE(v) << v.error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, static_cast<unsigned>(gen));
    s.world.Tick(kDt); // the methods run the new code
}

TEST_P(BackendTests, MethodOverloadsSelectTheBodyByTheArgumentTypes)
{
    Session s;
    hot::Instance child;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, child));
    EXPECT_EQ(fld(s.inst.shell, "total"), 10 + 1000); // Score(int) + Score(float)
    EXPECT_EQ(fld(s.inst.shell, "kinds"), 1 + 2);     // Kind(int) + Kind(string)
    EXPECT_EQ(fld(child.shell, "kinds"), 10 + 2);     // the override of Kind(int) only
}

TEST_P(BackendTests, MethodOverloadsReloadWhenAnOverloadIsAddedRemovedOrChanged)
{
    Session s;
    hot::Instance child;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, child));
    ASSERT_EQ(fld(s.inst.shell, "ticks"), 1);

    ASSERT_NO_FATAL_FAILURE(reload(*backend, s, "method_overload_v2.hot", 2));
    EXPECT_EQ(fld(s.inst.shell, "ticks"), 3); // the live object keeps its state
    EXPECT_EQ(fld(s.inst.shell, "total"), 15 + 1000 + 3); // the new overload Score(int, int)
    EXPECT_EQ(fld(child.shell, "total"), 15 + 1000 + 3);
    EXPECT_EQ(fld(child.shell, "kinds"), 10 + 2);

    ASSERT_NO_FATAL_FAILURE(reload(*backend, s, "method_overload_v3.hot", 3));
    EXPECT_EQ(fld(s.inst.shell, "ticks"), 5);
    EXPECT_EQ(fld(s.inst.shell, "total"), 15 + 4); // Score(int) + Score(int, string)
    EXPECT_EQ(fld(s.inst.shell, "kinds"), 1 + 2);
    EXPECT_EQ(fld(child.shell, "kinds"), 10 + 2);
}

// The overloads that are not instance methods of a class: static methods, free functions, struct
// methods and constructors. A "base." call goes to the parent overload of the argument types.
// Generation 2 adds an overload to each group and changes some bodies. The live object keeps its fields.
TEST_P(BackendTests, FunctionOverloadsReloadWithLiveState)
{
    Session s;
    hot::LoadResult v1 = backend->load(source("fn_overload_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "FnOv"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s.inst.shell, "ticks"), 1);
    EXPECT_EQ(fld(s.inst.shell, "stat"), 6 + 500);           // Twice(int) + Twice(float)
    EXPECT_EQ(fld(s.inst.shell, "free"), 4 + 70);            // Half(int) + Half(string)
    EXPECT_EQ(fld(s.inst.shell, "strct"), 3 + 101);          // Pt.Add(int) + Pt.Add(float)
    EXPECT_EQ(fld(s.inst.shell, "based"), (1 + 10) + (2 + 20)); // base.Pick of each overload
    EXPECT_EQ(fld(s.inst.shell, "made"), 4 + 300);           // two constructors

    ASSERT_NO_FATAL_FAILURE(reload(*backend, s, "fn_overload_v2.hot", 2));
    EXPECT_EQ(fld(s.inst.shell, "ticks"), 3); // the live object keeps its state
    EXPECT_EQ(fld(s.inst.shell, "stat"), 9 + 500 + 3);
    EXPECT_EQ(fld(s.inst.shell, "free"), 5 + 70 + 3);
    EXPECT_EQ(fld(s.inst.shell, "strct"), 3 + 101 + 3);
    EXPECT_EQ(fld(s.inst.shell, "based"), (1 + 100) + (2 + 200));
    EXPECT_EQ(fld(s.inst.shell, "made"), 4 + 300 + 6);
}

} // namespace
