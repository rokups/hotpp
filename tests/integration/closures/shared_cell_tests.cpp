// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Shared cells (G06). The script shared_cell_v1 has lambdas that write captured locals and a
// captured parameter. Each such variable lives in a heap cell that the function and the lambdas
// share. The tests use the backend that HOT_BACKEND selected (make_default_backend).
//
// CMake defines HOT_SOURCE_DIR.
#include "hot/backend.h"
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

class SharedCell : public ::testing::Test
{
public:
    void SetUp() override { backend = hot::make_default_backend(); }

    hot::Module build(uint32_t generation)
    {
        hot::LoadResult r = backend->load(source("shared_cell_v1.hot"), generation);
        if (!r)
            throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    void reload(Session& s, hot::Module m)
    {
        std::string error;
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(m)));
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
    }

    void call(Session& s, const char* name)
    {
        using Fn = void (*)(Actor*);
        void* thunk = s.loader.thunk(name);
        ASSERT_NE(thunk, nullptr) << name;
        reinterpret_cast<Fn>(thunk)(s.actor);
    }

    void bump(Session& s, int p)
    {
        using Fn = void (*)(Actor*, int);
        void* thunk = s.loader.thunk("Counter::bump");
        ASSERT_NE(thunk, nullptr);
        reinterpret_cast<Fn>(thunk)(s.actor, p);
    }

    void arm(Session& s)
    {
        using Fn = void (*)(Actor*, World&);
        void* thunk = s.loader.thunk("Counter::arm");
        ASSERT_NE(thunk, nullptr);
        reinterpret_cast<Fn>(thunk)(s.actor, s.world);
    }

    int field(Session& s, const char* name)
    {
        int* p = hot::field_ptr<int>(s.inst.shell, name);
        return p ? *p : -1;
    }

    std::unique_ptr<hot::Backend> backend;
};

// A counter lambda increments a captured local. The function reads the value after the calls.
TEST_F(SharedCell, CounterLambdaWritesTheLocalOfTheFunction)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build(1), "Counter"));
    ASSERT_NO_FATAL_FAILURE(call(s, "Counter::count"));
    EXPECT_EQ(field(s, "counted"), 3);
}

// Two lambdas share one cell: "add" writes it and "read" reads the new value.
TEST_F(SharedCell, TwoLambdasShareOneCell)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build(1), "Counter"));
    ASSERT_NO_FATAL_FAILURE(call(s, "Counter::share"));
    EXPECT_EQ(field(s, "shared"), 1010);
}

// A lambda writes a parameter. The function sees the new value.
TEST_F(SharedCell, LambdaWritesAParameter)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build(1), "Counter"));
    ASSERT_NO_FATAL_FAILURE(bump(s, 3));
    EXPECT_EQ(field(s, "param"), 12);
}

// Two timer lambdas outlive the function. The cell stays alive and both lambdas write it.
TEST_F(SharedCell, CellOutlivesTheFunction)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build(1), "Counter"));
    ASSERT_NO_FATAL_FAILURE(arm(s));
    s.world.Tick(1.0f);
    EXPECT_EQ(field(s, "fired"), 11);
    EXPECT_EQ(s.world.TimerCount(), 0u);
}

// After a reload the cells work in the new generation. The timers of the old generation hold
// stale closures: they do not run, and the cells go away with them.
TEST_F(SharedCell, SurvivesAReload)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build(1), "Counter"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(arm(s));
    ASSERT_NO_FATAL_FAILURE(reload(s, build(2)));
    EXPECT_EQ(hot::active_generation, 2u);
    testing::internal::CaptureStderr();
    s.world.Tick(1.0f); // the stale timers fire here
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(field(s, "fired"), 0);
    EXPECT_EQ(s.world.TimerCount(), 0u);

    ASSERT_NO_FATAL_FAILURE(call(s, "Counter::count"));
    EXPECT_EQ(field(s, "counted"), 3);
    ASSERT_NO_FATAL_FAILURE(arm(s));
    s.world.Tick(1.0f);
    EXPECT_EQ(field(s, "fired"), 11);
}

} // namespace
