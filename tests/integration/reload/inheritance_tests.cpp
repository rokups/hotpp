// Reload tests of script inheritance (S6a, design 4.5, 7.6 and 7.7) with the backend that
// SPP_BACKEND selected: a child of a script class, script virtuals, "base" calls, "is" and "as".
#include "Util.h"
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

// The factory of a child runs the constructor of the parent first. The record knows the parent.
// The child override of Tick calls the parent implementation with "base.Tick".
TEST_P(BackendTests, ChildRunsTheParentConstructorAndSuperCalls)
{
    hot::LoadResult v1 = backend->load(source("inherit_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Grunt"));
    s.commit_at_frame_boundary();
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_STREQ(s.inst.shell->cls->super->name, "Unit");
    EXPECT_EQ(*fld<int>(s, "ctor_log"), 12); // Unit(int) first, then Grunt(int, int)
    EXPECT_EQ(*fld<int>(s, "hp"), 1); // base(start + 1) with the default 0
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ticks"), 1); // base.Tick() ran the parent Tick
    EXPECT_EQ(*fld<int>(s, "super_ticks"), 1);
    EXPECT_EQ(*fld<int>(s, "unit_power"), 1); // base.Power() is the parent implementation
}

// The destructor of the child runs first, then the destructor of the parent.
TEST_P(BackendTests, ChildDestructorRunsBeforeTheParentDestructor)
{
    hot::LoadResult v1 = backend->load(source("inherit_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    testing::internal::CaptureStdout();
    {
        Session s;
        ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Grunt"));
        s.commit_at_frame_boundary();
    }
    const std::string log = testing::internal::GetCapturedStdout();
    const size_t child = log.find("[inherit_v1] Grunt dtor 0"), parent = log.find("[inherit_v1] Unit dtor 1");
    ASSERT_NE(child, std::string::npos) << log;
    ASSERT_NE(parent, std::string::npos) << log;
    EXPECT_LT(child, parent) << log;
}

// A call of a virtual through a parent handle uses the child. "is" and "as" read the records.
// After the reload, the changed child method runs and the new parent field has its initializer.
TEST_P(BackendTests, VirtualCallThroughAParentHandleFollowsTheReload)
{
    hot::LoadResult v1 = backend->load(source("inherit_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("inherit_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spotter"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "seen_power"), 7); // Grunt.Power through a Unit handle
    EXPECT_EQ(*fld<int>(s, "attack"), 70); // Unit.Attack calls the virtual Power
    EXPECT_TRUE(*fld<bool>(s, "saw_grunt"));
    EXPECT_TRUE(*fld<bool>(s, "saw_unit"));
    EXPECT_EQ(*fld<int>(s, "grunt_rage"), 7); // "as Grunt" gives the child

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "seen_power"), 14); // the Grunt.Power of generation 2
    EXPECT_EQ(*fld<int>(s, "attack"), 140);
    EXPECT_EQ(*fld<int>(s, "seen_armor"), 9); // the new parent field of the old child object
    EXPECT_EQ(*fld<int>(s, "seen_hp"), 6); // the parent part kept its value
    EXPECT_EQ(*fld<int>(s, "grunt_rage"), 7); // the child part kept its value
}

// A reload migrates the parent part and the child part of the state. The new parent function
// gets a slot after the old slots of the family. The constructor does not run again.
TEST_P(BackendTests, ReloadMigratesTheParentAndTheChildPart)
{
    hot::LoadResult v1 = backend->load(source("inherit_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("inherit_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Grunt"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ctor_log"), 12);
    EXPECT_EQ(*fld<int>(s, "ticks"), 3);
    EXPECT_EQ(*fld<int>(s, "child_armor"), 9);
    EXPECT_EQ(*fld<int>(s, "bonus"), 10); // Unit.Bonus of generation 2
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_EQ(s.inst.shell->cls->super->generation, 2u);
}

// A family of three levels with a sibling (S6b). A child object reads and writes the properties
// of its parents. The access works through a parent handle and through a child handle.
TEST_P(BackendTests, FamilyOfThreeLevelsWithInheritedProperties)
{
    hot::LoadResult v1 = backend->load(source("family_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Driver"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    // G: Score = 20 (written through the child handle), then h(): b = 2 + 20, Score += 1, Bonus = 44 + 1 (b = 45).
    EXPECT_EQ(*fld<int>(s, "top_score"), 21); // the same object through the parent handle
    EXPECT_EQ(*fld<int>(s, "leaf_score"), 21);
    EXPECT_EQ(*fld<int>(s, "sib_score"), 7); // the sibling has its own value
    EXPECT_EQ(*fld<int>(s, "sib_only"), 107); // a bare "Score" in a child of A
    EXPECT_EQ(*fld<int>(s, "top_f"), 92); // G.f: (B.f(1) = 1 + 45) * 2
    EXPECT_EQ(*fld<int>(s, "leaf_bonus"), 90); // B.Bonus getter: b * 2 with b = 45
    EXPECT_EQ(*fld<int>(s, "leaf_a"), 8); // A(int) got 4 * 2
}

// A reload of the family. The new property of the root gets its initializer in an old object.
// The changed getter of the middle class applies. The values of the old objects stay.
TEST_P(BackendTests, FamilyReloadKeepsInheritedPropertyValues)
{
    hot::LoadResult v1 = backend->load(source("family_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("family_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Driver"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    ASSERT_EQ(*fld<int>(s, "leaf_bonus"), 90);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "top_score"), 21);
    EXPECT_EQ(*fld<int>(s, "sib_score"), 7);
    EXPECT_EQ(*fld<int>(s, "sib_only"), 107);
    EXPECT_EQ(*fld<int>(s, "top_f"), 92);
    EXPECT_EQ(*fld<int>(s, "leaf_bonus"), 135); // the getter of generation 2: b * 3
    EXPECT_EQ(*fld<int>(s, "leaf_rank"), 6); // the new property Rank: 3 for the child and the sibling
}

// A reload that changes the script parent of a class is rejected. The old generation stays.
TEST_P(BackendTests, ReloadThatChangesTheScriptParentIsRejected)
{
    hot::LoadResult v1 = backend->load(source("family_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult bad = backend->load(source("family_reparent.hot"), 2);
    ASSERT_TRUE(bad) << bad.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Driver"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    const hot::ReloadResult rejected = s.loader.request_reload(std::move(*bad.module));
    const std::string& error = rejected.error;
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status, hot::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(rejected.reason, hot::ReloadReason::ScriptParentChanged);
    EXPECT_NE(error.find("script parent class changed"), std::string::npos) << error;
    EXPECT_NE(error.find("G"), std::string::npos) << error;
    EXPECT_FALSE(s.loader.reload_pending());
    s.world.Tick(kDt);
    EXPECT_EQ(hot::active_generation, 1u);
}

// The "base(...)" arguments of a chain evaluate one time for each object (S16). Each argument
// calls Util.Count, so one Leaf counts two calls: one for Leaf and one for Mid. This is true for
// the factory and for "new", also after a reload that changes the argument of Leaf.
TEST_P(BackendTests, SuperArgumentsOfAChainEvaluateOneTime)
{
    hot::LoadResult v1 = backend->load(source("chain_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("chain_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    int calls = Util::Calls();
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Maker"));
    s.commit_at_frame_boundary();
    EXPECT_EQ(Util::Calls(), calls); // Maker has no "base(...)" argument

    // The factory: Leaf(0) gives Mid(10) and Root(11).
    auto make_leaf = [&](int want_got, int want_mid)
    {
        const int before = Util::Calls();
        const hot::ClassEntry* entry = s.loader.find("Leaf");
        ASSERT_NE(entry, nullptr);
        hot::Instance leaf = entry->create();
        ASSERT_NE(leaf.shell, nullptr);
        EXPECT_EQ(Util::Calls() - before, 2);
        EXPECT_EQ(*hot::field_ptr<int>(leaf.shell, "got"), want_got);
        EXPECT_EQ(*hot::field_ptr<int>(leaf.shell, "mid"), want_mid);
        EXPECT_EQ(*hot::field_ptr<int>(leaf.shell, "ctor_log"), 123);
        delete static_cast<Actor*>(leaf.native);
    };
    ASSERT_NO_FATAL_FAILURE(make_leaf(11, 10));

    // "new Leaf(5)" in Maker.Tick: Count(15) and Count(16).
    calls = Util::Calls();
    s.world.Tick(kDt);
    EXPECT_EQ(Util::Calls() - calls, 2);
    EXPECT_EQ(*fld<int>(s, "kid_got"), 16);
    EXPECT_EQ(*fld<int>(s, "kid_mid"), 15);
    EXPECT_EQ(*fld<int>(s, "kid_log"), 123);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);

    // Generation 2 adds 20 in the "base(...)" argument of Leaf. The factory uses it now.
    ASSERT_NO_FATAL_FAILURE(make_leaf(21, 20));
    calls = Util::Calls();
    s.world.Tick(kDt);
    EXPECT_EQ(Util::Calls() - calls, 2);
    EXPECT_EQ(*fld<int>(s, "kid_got"), 26);
    EXPECT_EQ(*fld<int>(s, "kid_mid"), 25);
    EXPECT_EQ(*fld<int>(s, "made"), 3);
}

// A child of a script class under the slot ABI of the state member (design 4.4 and 4.5).
// Kid inherits Bump and Twice from Base. Kid overrides the script virtual Power and the native
// virtuals Tick and TakeDamage. Each override calls the parent version with "base". Kid calls
// the native method Move. A reload of Kid changes the overrides and adds fields.
TEST_P(BackendTests, ChildUsesInheritedMethodsNativeBaseAndSuperUnderTheSlotAbi)
{
    hot::LoadResult v1 = backend->load(source("abi_child_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("abi_child_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Kid"));
    s.commit_at_frame_boundary();
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_STREQ(s.inst.shell->cls->super->name, "Base");

    s.world.Tick(kDt);
    EXPECT_FLOAT_EQ(*fld<float>(s, "moved"), 1.0f); // the native Move through the child
    EXPECT_FLOAT_EQ(s.actor->Position.x, 1.0f);
    EXPECT_EQ(*fld<int>(s, "hits"), 10); // the inherited Bump
    EXPECT_EQ(*fld<int>(s, "twice"), 10); // the inherited Twice calls the virtual Kid.Power (5)
    EXPECT_EQ(*fld<int>(s, "super_power"), 1); // base.Power() is Base.Power

    // A native virtual call: native code to the shell, the slot and the Kid override.
    // Kid.TakeDamage calls Base.TakeDamage with twice the amount.
    s.actor->TakeDamage(3.0f);
    EXPECT_FLOAT_EQ(*fld<float>(s, "hp"), 94.0f);
    EXPECT_EQ(*fld<int>(s, "hits"), 11);
    EXPECT_EQ(*fld<int>(s, "kid_hits"), 1);
    s.world.Tick(kDt);
    EXPECT_FLOAT_EQ(*fld<float>(s, "moved"), 2.0f);
    EXPECT_EQ(*fld<int>(s, "hits"), 21);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(*fld<int>(s, "twice"), 10); // the old value stays until the next Tick
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "twice"), 14); // Twice of Base now calls Kid.Power (7)
    EXPECT_EQ(*fld<int>(s, "super_power"), 1);
    EXPECT_EQ(*fld<int>(s, "armor"), 4); // the new parent field has its initializer
    EXPECT_EQ(*fld<int>(s, "gen"), 2); // the new child field
    EXPECT_FLOAT_EQ(*fld<float>(s, "moved"), 4.0f);
    s.actor->TakeDamage(2.0f); // the new override: Base.TakeDamage(6)
    EXPECT_FLOAT_EQ(*fld<float>(s, "hp"), 88.0f);
    EXPECT_EQ(*fld<int>(s, "hits"), 42); // 21 + 10 + 10 + 1
    EXPECT_EQ(*fld<int>(s, "kid_hits"), 2);
    EXPECT_EQ(s.inst.shell->cls->super->generation, 2u);
}

} // namespace
