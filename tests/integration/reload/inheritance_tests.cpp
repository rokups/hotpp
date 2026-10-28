// Reload tests of script inheritance (S6a, design 4.5, 7.6 and 7.7) with the backend that
// SPP_BACKEND selected: a child of a script class, script virtuals, "super" calls, "is" and "as".
#include <gtest/gtest.h>

#include <string>

#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;
using test_support::source;

template <class T>
T* fld(Session& s, const char* name)
{
    return spp::field_ptr<T>(s.inst.shell, name);
}

// The factory of a child runs the constructor of the parent first. The record knows the parent.
// The child override of Tick calls the parent implementation with "super.Tick".
TEST_P(BackendTests, ChildRunsTheParentConstructorAndSuperCalls)
{
    spp::LoadResult v1 = backend->load(source("inherit_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Grunt"));
    s.commit_at_frame_boundary();
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_STREQ(s.inst.shell->cls->super->name, "Unit");
    EXPECT_EQ(*fld<int>(s, "ctor_log"), 12);  // Unit(int) first, then Grunt(int, int)
    EXPECT_EQ(*fld<int>(s, "hp"), 1);         // super(start + 1) with the default 0
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ticks"), 1);       // super.Tick() ran the parent Tick
    EXPECT_EQ(*fld<int>(s, "super_ticks"), 1);
    EXPECT_EQ(*fld<int>(s, "unit_power"), 1);  // super.Power() is the parent implementation
}

// The destructor of the child runs first, then the destructor of the parent.
TEST_P(BackendTests, ChildDestructorRunsBeforeTheParentDestructor)
{
    spp::LoadResult v1 = backend->load(source("inherit_v1.spp"), 1);
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
    spp::LoadResult v1 = backend->load(source("inherit_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("inherit_v2.spp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spotter"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "seen_power"), 7);  // Grunt.Power through a Unit handle
    EXPECT_EQ(*fld<int>(s, "attack"), 70);     // Unit.Attack calls the virtual Power
    EXPECT_TRUE(*fld<bool>(s, "saw_grunt"));
    EXPECT_TRUE(*fld<bool>(s, "saw_unit"));
    EXPECT_EQ(*fld<int>(s, "grunt_rage"), 7);  // "as Grunt" gives the child

    std::string error;
    ASSERT_TRUE(s.loader.request_reload(std::move(*v2.module), error)) << error;
    s.world.Tick(kDt);  // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(spp::active_generation, 2u);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "seen_power"), 14);  // the Grunt.Power of generation 2
    EXPECT_EQ(*fld<int>(s, "attack"), 140);
    EXPECT_EQ(*fld<int>(s, "seen_armor"), 9);  // the new parent field of the old child object
    EXPECT_EQ(*fld<int>(s, "seen_hp"), 6);     // the parent part kept its value
    EXPECT_EQ(*fld<int>(s, "grunt_rage"), 7);  // the child part kept its value
}

// A reload migrates the parent part and the child part of the state. The new parent function
// gets a slot after the old slots of the family. The constructor does not run again.
TEST_P(BackendTests, ReloadMigratesTheParentAndTheChildPart)
{
    spp::LoadResult v1 = backend->load(source("inherit_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("inherit_v2.spp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Grunt"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(std::move(*v2.module), error)) << error;
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ctor_log"), 12);
    EXPECT_EQ(*fld<int>(s, "ticks"), 3);
    EXPECT_EQ(*fld<int>(s, "child_armor"), 9);
    EXPECT_EQ(*fld<int>(s, "bonus"), 10);  // Unit.Bonus of generation 2
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_EQ(s.inst.shell->cls->super->generation, 2u);
}

}  // namespace
