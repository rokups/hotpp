// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of a host type system in script shells. A cpp_prelude rule puts a macro in each shell
// below TypedActor. The macro overrides a native virtual and adds a static type tag. The tag must
// stay the same object across a reload, because a host can register its address.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include "TypedActor.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

const TypeTag* tag_of(const hot::Instance& inst)
{
    return static_cast<TypedActor*>(static_cast<Actor*>(inst.native))->GetTag();
}

} // namespace

// Each script class reports its own tag. The tag of a script child names the parent as its base.
TEST_P(BackendTests, HostTypeTagNamesTheScriptClass)
{
    hot::LoadResult v1 = backend->load(source("host_typed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Scout"));
    const TypeTag* scout = tag_of(s.inst);
    ASSERT_NE(scout, nullptr);
    EXPECT_STREQ(scout->name, "Scout");
    ASSERT_NE(scout->base, nullptr);
    EXPECT_STREQ(scout->base->name, "TypedActor");

    hot::Instance ranger = s.loader.find("Ranger")->create();
    s.world.Add(static_cast<Actor*>(ranger.native));
    const TypeTag* rtag = tag_of(ranger);
    EXPECT_STREQ(rtag->name, "Ranger");
    EXPECT_EQ(rtag->base, scout);
}

// A reload keeps the tag of the old objects. A new object after the reload has the same tag.
TEST_P(BackendTests, HostTypeTagStaysAcrossReload)
{
    hot::LoadResult v1 = backend->load(source("host_typed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("host_typed_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Scout"));
    s.commit_at_frame_boundary();
    hot::Instance ranger = s.loader.find("Ranger")->create();
    s.world.Add(static_cast<Actor*>(ranger.native));
    const TypeTag* scout = tag_of(s.inst);
    const TypeTag* rtag = tag_of(ranger);
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "ticks"), 1);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    // The new code runs, and the old objects report the same tags.
    EXPECT_GT(*hot::field_ptr<int>(s.inst.shell, "ticks"), 2);
    EXPECT_EQ(tag_of(s.inst), scout);
    EXPECT_EQ(tag_of(ranger), rtag);

    hot::Instance fresh = s.loader.find("Scout")->create();
    s.world.Add(static_cast<Actor*>(fresh.native));
    EXPECT_EQ(tag_of(fresh), scout);
    hot::Instance fresh_ranger = s.loader.find("Ranger")->create();
    s.world.Add(static_cast<Actor*>(fresh_ranger.native));
    EXPECT_EQ(tag_of(fresh_ranger), rtag);
}

// The module attribute "lib": "mocklib.vector" and "mocklib.array" work next to "std.vector" and
// have the bounds check of std. Sum: 2 + 3 (the loop), 100 and 1000 (the checks), 1 and 3.
TEST_P(BackendTests, LibraryNamespaceWorksNextToStd)
{
    hot::LoadResult v1 = backend->load(source("lib_ns_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "LibUser"));
    s.world.Tick(kDt);
    EXPECT_EQ(s.actor->Name, "1109");
}

// The containers of "mocklib" with plain elements are free of code, as the std ones are
// (hot::holds_no_code). So the state of LibUser does not block the unload of a generation.
TEST_P(BackendTests, LibraryContainersHoldNoCode)
{
    hot::LoadResult v1 = backend->load(source("lib_ns_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "LibUser"));
    for (const char* name : {"a", "b", "c", "label", "names"})
    {
        const hot::FieldInfo* f = hot::find_field(s.inst.shell->cls, name);
        ASSERT_NE(f, nullptr) << name;
        EXPECT_EQ(f->flags & hot::kMayHoldCode, 0) << name;
    }
}
