// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The box lambdas of a .hot script (tests/scripts/lambda_box.hot). The tests run with the clang
// files backend, with the JIT backend and with the AOT code of the script. A mode that is not in
// the build is skipped. The reload tests need a backend, so the AOT mode skips them.
//
// CMake defines HOT_SOURCE_DIR.
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "hot/runtime/lambda.h"
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

enum class Mode
{
    Files,
    Jit,
    Aot,
};

class LambdaBoxScript : public ::testing::TestWithParam<Mode>
{
public:
    void SetUp() override
    {
        const Mode m = GetParam();
        if (m == Mode::Files && !hot::backend_available(hot::BackendKind::ClangFiles))
            GTEST_SKIP() << "the clang files backend is not in this build";
        if (m == Mode::Jit && !hot::backend_available(hot::BackendKind::Jit))
            GTEST_SKIP() << "the JIT backend is not in this build";
        if (m != Mode::Aot)
        {
            hot::DefaultBackendOptions o;
            o.kind = m == Mode::Files ? hot::BackendKind::ClangFiles : hot::BackendKind::Jit;
            o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine"};
            backend = hot::make_default_backend(o);
        }
    }

    hot::Module build(uint32_t generation)
    {
        hot::LoadResult r = backend->load(test_support::source("lambda_box.hot"), generation);
        if (!r)
            throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    // Loads generation 1 (or the AOT code) and makes a Boxer in the world.
    void start(Session& s)
    {
        if (!backend)
        {
            const hot::ReloadResult r = s.loader.load_aot(hot::aot_table);
            ASSERT_TRUE(r) << r.error;
            const hot::ClassEntry* entry = s.loader.find("Boxer");
            ASSERT_NE(entry, nullptr);
            s.inst = entry->create();
            s.actor = static_cast<Actor*>(inst_native(s));
            s.world.Add(s.actor);
        }
        else
            ASSERT_NO_FATAL_FAILURE(s.start(build(1), "Boxer"));
        s.commit_at_frame_boundary();
    }

    static void* inst_native(Session& s) { return s.inst.native; }

    template <class... A> void call(Session& s, const char* name, A... a)
    {
        using Fn = void (*)(Actor*, A...);
        void* thunk = s.loader.thunk(name);
        ASSERT_NE(thunk, nullptr) << name;
        reinterpret_cast<Fn>(thunk)(s.actor, a...);
    }

    void store(Session& s)
    {
        using Fn = void (*)(Actor*, World&);
        void* thunk = s.loader.thunk("Boxer::store");
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

TEST_P(LambdaBoxScript, DeclaredParametersAndAValueResult)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    ASSERT_NO_FATAL_FAILURE(call(s, "Boxer::typed", 4));
    EXPECT_EQ(field(s, "result"), 11);
}

TEST_P(LambdaBoxScript, VoidResultUsesThis)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    ASSERT_NO_FATAL_FAILURE(call(s, "Boxer::voids"));
    EXPECT_EQ(field(s, "hits"), 2);
}

TEST_P(LambdaBoxScript, SharedCellIsWritten)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    ASSERT_NO_FATAL_FAILURE(call(s, "Boxer::cell"));
    EXPECT_EQ(field(s, "result"), 5);
}

TEST_P(LambdaBoxScript, AutoParameterTakesTheTargetSignature)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    ASSERT_NO_FATAL_FAILURE(call(s, "Boxer::autos"));
    EXPECT_EQ(field(s, "result"), 5);
}

TEST_P(LambdaBoxScript, NestedLambdaCapturesAClosure)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    const size_t boxes = hot::lambda_box_count();
    ASSERT_NO_FATAL_FAILURE(call(s, "Boxer::nested", 10));
    EXPECT_EQ(field(s, "result"), 2123);
    EXPECT_EQ(hot::lambda_box_count(), boxes); // the locals of the method released each box
}

TEST_P(LambdaBoxScript, StoredLambdaRunsInTheLiveGeneration)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    const size_t boxes = hot::lambda_box_count();
    ASSERT_NO_FATAL_FAILURE(store(s));
    EXPECT_EQ(hot::lambda_box_count(), boxes + 2); // the timer lambda and the closure "add"
    // The record of "add" has the key, the capture names and the live flag of the module.
    const hot::LambdaRecord* rec = s.loader.find_lambda("lambda_box:Boxer::store/v:add");
    ASSERT_NE(rec, nullptr);
    EXPECT_STREQ(rec->what, "Boxer::store lambda");
    ASSERT_NE(rec->names, nullptr);
    EXPECT_STREQ(rec->names[0], "__self");
    EXPECT_STREQ(rec->names[1], "__cell_n");
    EXPECT_EQ(rec->names[2], nullptr);
    ASSERT_NE(rec->aliases, nullptr);
    EXPECT_EQ(rec->aliases[0], nullptr);
    EXPECT_NE(rec->live, nullptr);
    EXPECT_NE(rec->destroy, nullptr);
    s.world.Tick(1.0f);
    EXPECT_EQ(field(s, "total"), 6);
    EXPECT_EQ(s.world.TimerCount(), 0u);
    EXPECT_EQ(hot::lambda_box_count(), boxes);
}

// The commit gives the boxes of the retired module the code of the new module. The capture of
// the timer lambda holds the closure "add": the move of the outer block keeps the inner box. The
// timer runs the new code, drops the last reference, and then the retired generation unloads.
TEST_P(LambdaBoxScript, StoredNestedLambdaRunsAfterTheCommitAndTheGenerationUnloads)
{
    if (GetParam() == Mode::Aot)
        GTEST_SKIP() << "the AOT code has no reload";
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    const size_t boxes = hot::lambda_box_count();
    ASSERT_NO_FATAL_FAILURE(store(s));
    ASSERT_EQ(hot::lambda_box_count(), boxes + 2);
    EXPECT_EQ(hot::pin_count(1), 2u); // each closure has a pin

    ASSERT_RELOAD_OK(s.loader.request_reload(build(2)));
    s.world.Tick(kDt); // the commit
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    // The two boxes moved to generation 2. The closures keep the pins of the glue code.
    EXPECT_EQ(hot::lambda_box_count(), boxes + 2);
    EXPECT_EQ(hot::pin_count(1), 2u);

    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    s.world.Tick(1.0f); // the timer fires
    s.world.Tick(kDt); // a frame boundary after the last unpin unloads generation 1
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0);
    EXPECT_EQ(field(s, "total"), 6);
    EXPECT_EQ(s.world.TimerCount(), 0u);
    EXPECT_EQ(hot::lambda_box_count(), boxes);
    EXPECT_EQ(hot::pin_count(1), 0u);
    EXPECT_NE(log.find("generation 1 unloaded"), std::string::npos) << log;

    // The new generation makes new boxes that run.
    ASSERT_NO_FATAL_FAILURE(store(s));
    s.world.Tick(1.0f);
    EXPECT_EQ(field(s, "total"), 6);
}

const char* mode_name(const ::testing::TestParamInfo<Mode>& i)
{
    return i.param == Mode::Files ? "Files" : i.param == Mode::Jit ? "Jit" : "Aot";
}

// The tests read the fields of the Boxer by name. Optimized AOT code has no field table, so that
// build has no Aot mode.
#ifdef __HOTPP_AOT_OPTIMIZED__
INSTANTIATE_TEST_SUITE_P(Backends, LambdaBoxScript, ::testing::Values(Mode::Files, Mode::Jit), mode_name);
#else
INSTANTIATE_TEST_SUITE_P(Backends, LambdaBoxScript, ::testing::Values(Mode::Files, Mode::Jit, Mode::Aot), mode_name);
#endif

} // namespace
