// Run-time override and revert of one AOT unit (big task 14, steps 5 and 6). The binary has two
// AOT scripts (units "override_a_v1" and "override_b_v1") and a backend. The call sequence is the
// one of samples/hot-reload/main.cpp: load, request_reload, commit_pending, frame_boundary.
#include "Actor.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

constexpr float kDt = 1.0f / 60.0f;
const char* const kUnitA = "override_a_v1";

// The loader is the first member: it is destroyed last, after the actors.
class AotOverride : public ::testing::Test
{
protected:
    void SetUp() override
    {
        backend = hot::make_default_backend();
        backend->seed_slots(hot::aot_table.slots);
        ASSERT_TRUE(loader.load_aot(hot::aot_table));
        alpha = make("Alpha");
        beta = make("Beta");
    }

    void TearDown() override
    {
        delete static_cast<Actor*>(alpha.native);
        delete static_cast<Actor*>(beta.native);
    }

    hot::Instance make(const char* name)
    {
        const hot::ClassEntry* e = loader.find(name);
        EXPECT_NE(e, nullptr) << name;
        return e->create();
    }

    int* field(hot::Instance& inst, const char* name) { return hot::field_ptr<int>(inst.shell, name); }

    int call_int(const char* thunk_name, hot::Instance& inst)
    {
        void* fn = loader.thunk(thunk_name);
        EXPECT_NE(fn, nullptr) << thunk_name;
        return reinterpret_cast<int32_t (*)(Actor*)>(fn)(static_cast<Actor*>(inst.native));
    }

    void tick(hot::Instance& inst, int frames)
    {
        for (int i = 0; i < frames; ++i)
            static_cast<Actor*>(inst.native)->Tick(kDt);
    }

    // Compiles the override of unit A, validates it and commits it.
    void override_a(uint32_t generation, const char* file = "override_a_v2.hot")
    {
        hot::LoadResult built = backend->load(test_support::source(file), generation);
        ASSERT_TRUE(built) << built.error;
        built.module->unit = kUnitA;
        const hot::ReloadResult req = loader.request_reload(std::move(*built.module));
        ASSERT_TRUE(req) << req.error;
        const hot::ReloadResult done = loader.commit_pending();
        ASSERT_TRUE(done) << done.error;
        loader.frame_boundary();
    }

    void revert_a()
    {
        const hot::ReloadResult r = loader.revert_unit(kUnitA);
        ASSERT_TRUE(r) << r.error;
        EXPECT_EQ(r.status, hot::ReloadStatus::Pending);
        const hot::ReloadResult done = loader.commit_pending();
        ASSERT_TRUE(done) << done.error;
        loader.frame_boundary();
    }

    hot::Loader loader; // before the world and the objects
    World world;
    std::unique_ptr<hot::Backend> backend;
    hot::Instance alpha{};
    hot::Instance beta{};
};

TEST_F(AotOverride, OverrideOfOneUnitMigratesItsObjectsAndLeavesTheOtherUnit)
{
    tick(alpha, 3);
    tick(beta, 2);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 3);
    // An AOT closure of unit A is in the timer of the world.
    reinterpret_cast<void (*)(Actor*, World&, float)>(loader.thunk("Alpha::arm"))(
        static_cast<Actor*>(alpha.native), world, 1.0f);
    EXPECT_EQ(world.TimerCount(), 1u);

    ASSERT_NO_FATAL_FAILURE(override_a(2));
    EXPECT_EQ(hot::active_generation, 2u);

    // The live object kept its field, got the new field with its default, and runs the new code.
    ASSERT_NE(field(alpha, "count"), nullptr);
    EXPECT_EQ(*field(alpha, "count"), 3);
    ASSERT_NE(field(alpha, "bonus"), nullptr);
    EXPECT_EQ(*field(alpha, "bonus"), 5);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 8);
    tick(alpha, 1);
    EXPECT_EQ(*field(alpha, "count"), 5);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 10);

    // Unit B still runs its AOT code, and its methods are not stale.
    const uint64_t stale0 = hot::stale_call_count();
    EXPECT_EQ(call_int("Beta::Hits", beta), 20);
    tick(beta, 1);
    EXPECT_EQ(call_int("Beta::Hits", beta), 30);
    EXPECT_EQ(hot::stale_call_count(), stale0);

    // The old AOT closure of unit A is stale: the body does not run.
    testing::internal::CaptureStderr();
    world.Tick(1.0f);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count(), stale0 + 1) << log;
    EXPECT_EQ(*field(alpha, "fired"), 0);
}

TEST_F(AotOverride, RevertGivesTheAotBehaviorBackAndKeepsTheCommonFields)
{
    tick(alpha, 3);
    ASSERT_NO_FATAL_FAILURE(override_a(2));
    tick(alpha, 1); // count 5 with the override
    ASSERT_NE(field(alpha, "bonus"), nullptr);

    ASSERT_NO_FATAL_FAILURE(revert_a());
    EXPECT_EQ(hot::active_generation, 3u);
    // The common fields are kept. The field that the AOT layout lacks is dropped.
    ASSERT_NE(field(alpha, "count"), nullptr);
    EXPECT_EQ(*field(alpha, "count"), 5);
    EXPECT_EQ(field(alpha, "bonus"), nullptr);
    // The AOT code is live again: Total has no bonus, Tick adds one.
    EXPECT_EQ(call_int("Alpha::Total", alpha), 5);
    tick(alpha, 1);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 6);
    // The unit B was not touched.
    tick(beta, 1);
    EXPECT_EQ(call_int("Beta::Hits", beta), 10);

    // A second override after the revert works. The field "bonus" is new again.
    ASSERT_NO_FATAL_FAILURE(override_a(4));
    EXPECT_EQ(hot::active_generation, 4u);
    EXPECT_EQ(*field(alpha, "count"), 6);
    ASSERT_NE(field(alpha, "bonus"), nullptr);
    EXPECT_EQ(*field(alpha, "bonus"), 5);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 11);
    tick(alpha, 1);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 13);
    // And a second revert works too.
    ASSERT_NO_FATAL_FAILURE(revert_a());
    EXPECT_EQ(call_int("Alpha::Total", alpha), 8);
}

// The override changes the signature of Alpha::Total. The slot gets the key of the new signature.
// The revert must give the key of the AOT signature back. Else the AOT thunk detaches.
TEST_F(AotOverride, RevertAfterASignatureChangeRestoresTheAotSignature)
{
    tick(alpha, 3);
    ASSERT_NO_FATAL_FAILURE(override_a(2, "override_a_sig.hot"));
    using TotalWithArg = int32_t (*)(Actor*, int32_t);
    auto changed = reinterpret_cast<TotalWithArg>(loader.thunk("Alpha::Total"));
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed(static_cast<Actor*>(alpha.native), 4), 7);

    const uint64_t detached0 = hot::detached_call_count();
    ASSERT_NO_FATAL_FAILURE(revert_a());
    // The thunk of the AOT signature works. It does not detach.
    testing::internal::CaptureStderr();
    EXPECT_EQ(call_int("Alpha::Total", alpha), 3);
    tick(alpha, 1);
    EXPECT_EQ(call_int("Alpha::Total", alpha), 4);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::detached_call_count(), detached0) << log;
    // The AOT slot has no key (the first signature is not checked), so the thunk of the override
    // signature does not detach. A host must drop the delegates of an override before the revert.
}

TEST_F(AotOverride, AotClosureIsLiveAgainAfterTheRevert)
{
    ASSERT_NO_FATAL_FAILURE(override_a(2));
    ASSERT_NO_FATAL_FAILURE(revert_a());
    reinterpret_cast<void (*)(Actor*, World&, float)>(loader.thunk("Alpha::arm"))(
        static_cast<Actor*>(alpha.native), world, 1.0f);
    const uint64_t stale0 = hot::stale_call_count();
    world.Tick(1.0f);
    EXPECT_EQ(hot::stale_call_count(), stale0);
    EXPECT_EQ(*field(alpha, "fired"), 1);
}

TEST_F(AotOverride, RevertOfAUnitWithoutAnAotModuleFails)
{
    const hot::ReloadResult unknown = loader.revert_unit("no_such_unit");
    EXPECT_FALSE(unknown);
    EXPECT_EQ(unknown.reason, hot::ReloadReason::BadModule);
    EXPECT_NE(unknown.error.find("no AOT module"), std::string::npos) << unknown.error;
    EXPECT_FALSE(loader.reload_pending());

    // A loader without load_aot has no AOT module at all.
    hot::Loader plain;
    const hot::ReloadResult none = plain.revert_unit(kUnitA);
    EXPECT_FALSE(none);
    EXPECT_EQ(none.reason, hot::ReloadReason::BadModule);

    // A revert while a reload is pending is Busy.
    hot::LoadResult built = backend->load(test_support::source("override_a_v2.hot"), 2);
    ASSERT_TRUE(built) << built.error;
    built.module->unit = kUnitA;
    ASSERT_TRUE(loader.request_reload(std::move(*built.module)));
    EXPECT_EQ(loader.revert_unit(kUnitA).reason, hot::ReloadReason::Busy);
}

} // namespace
