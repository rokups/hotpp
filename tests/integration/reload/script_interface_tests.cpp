// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Script interfaces (design 41b, step B5) with the backend that HOT_BACKEND selected: the calls
// of an interface value, "is" and "as", a list of interface values, identity and a null call.
#include "IfaceNative.h"
#include "Util.h"
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <typeinfo>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

template <class T> T* fld(Session& s, const char* name)
{
    return hot::field_ptr<T>(s.inst.shell, name);
}

TEST_P(BackendTests, InterfaceCallsReachTheImplementations)
{
    hot::LoadResult v1 = backend->load(source("iface_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Probe"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "area_sq"), 9);
    EXPECT_EQ(*fld<int>(s, "area_wide"), 15); // Wide overrides Area and gets IShape from Square
    EXPECT_EQ(*fld<int>(s, "area_disc"), 31); // a class handle converts to the interface argument
    EXPECT_EQ(*fld<int>(s, "scaled"), 12);
    EXPECT_EQ(*fld<std::string>(s, "name"), "wide"); // the child method wins over the parent method
    EXPECT_EQ(*fld<int>(s, "list_sum"), 9 + 15 + 31); // a list of interface values
}

TEST_P(BackendTests, InterfaceIsAsAndIdentity)
{
    hot::LoadResult v1 = backend->load(source("iface_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Probe"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "is_sq"), 1);
    EXPECT_EQ(*fld<int>(s, "is_wide"), 1); // through the parent
    EXPECT_EQ(*fld<int>(s, "is_plain"), 0);
    EXPECT_EQ(*fld<int>(s, "as_plain_null"), 1);
    EXPECT_EQ(*fld<int>(s, "as_back"), 1);
    EXPECT_EQ(*fld<int>(s, "same"), 1);
    EXPECT_EQ(*fld<int>(s, "same_cls"), 1);
    EXPECT_EQ(*fld<int>(s, "differ"), 1);
    EXPECT_EQ(*fld<int>(s, "null_cmp"), 1);
    EXPECT_EQ(*fld<int>(s, "me_true"), 1); // "this == i"
    EXPECT_EQ(*fld<int>(s, "me_false"), 0);
}

TEST_P(BackendTests, CallOfANullInterfaceThrowsAScriptError)
{
    hot::LoadResult v1 = backend->load(source("iface_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Probe"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "threw"), 1);
    EXPECT_EQ(*fld<std::string>(s, "why"), "null interface handle 'IShape'");
}

// Reload with the interface values alive (F3). A body change keeps the slots. A new virtual method
// gets a new slot, and the slot of Area stays. A class that starts to list the interface gets a
// table. The value in the field "keep" is the same object and it works in each generation,
// because the loader rebuilds the table of the ids at each commit.
uint32_t area_slot(Session& s, const char* cls)
{
    const hot::ClassEntry* e = s.loader.find(cls);
    EXPECT_NE(e, nullptr);
    const uint32_t id = hot::iface_method_id("IShape.Area", typeid(int()).name());
    return e && e->record && id < e->record->iface_slot_count ? e->record->iface_slots[id] : hot::kNoIfaceSlot;
}

void reload_to(Session& s, hot::LoadResult& r, uint32_t gen)
{
    ASSERT_TRUE(r) << r.error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*r.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, gen);
}

TEST_P(BackendTests, InterfaceValueSurvivesReloadsThatKeepAndMoveTheSlots)
{
    hot::LoadResult v1 = backend->load(source("iface_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Probe"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "kept"), 9);
    EXPECT_EQ(*fld<int>(s, "kept_scaled"), 6);
    const uint32_t slot1 = area_slot(s, "Square");
    ASSERT_NE(slot1, hot::kNoIfaceSlot);
    void* const shell = s.inst.shell;

    // Generation 2: only a body changes, so the slot of Area stays.
    hot::LoadResult v2 = backend->load(source("iface_v2.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v2, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(area_slot(s, "Square"), slot1);
    EXPECT_EQ(*fld<int>(s, "kept"), 109); // the same interface value, the new body
    EXPECT_EQ(*fld<int>(s, "kept_scaled"), 6);
    EXPECT_EQ(*fld<int>(s, "area_wide"), 15);

    // Generation 3: a new virtual method, and Plain lists IShape.
    const uint32_t count2 = s.loader.find("Square")->record->slot_count;
    EXPECT_EQ(*fld<int>(s, "is_plain"), 0);
    hot::LoadResult v3 = backend->load(source("iface_v3.hot"), 3);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v3, 3));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(area_slot(s, "Square"), slot1); // the slots are stable
    EXPECT_EQ(s.loader.find("Square")->record->slot_count, count2 + 1); // Extra has a new slot
    EXPECT_EQ(*fld<int>(s, "is_plain"), 1);
    EXPECT_NE(s.loader.find("Plain")->record->iface_slots, nullptr);
    EXPECT_EQ(*fld<int>(s, "kept"), 209);
    EXPECT_EQ(*fld<int>(s, "kept_scaled"), 6);
    EXPECT_EQ(*fld<int>(s, "area_wide"), 15);
    EXPECT_EQ(*fld<int>(s, "list_sum"), 209 + 15 + 31);
    EXPECT_EQ(*fld<std::string>(s, "name"), "wide");
}

// Reference, const reference, [weak], [value] and script class parameters (F3). The call must
// find the slot (the row signature equals the helper signature) and pass the values right.
TEST_P(BackendTests, InterfaceMethodsWithEachKindOfParameter)
{
    hot::LoadResult v1 = backend->load(source("iface_params_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Prober"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "r_ref"), 1);
    EXPECT_EQ(*fld<int>(s, "out_ref"), 42); // the reference reaches the caller
    EXPECT_EQ(*fld<int>(s, "r_const"), 3);
    EXPECT_EQ(*fld<int>(s, "r_weak"), 1);
    EXPECT_EQ(*fld<int>(s, "r_val"), 2);
    EXPECT_EQ(*fld<int>(s, "r_val_after"), 1); // a [value] argument is a copy
    EXPECT_EQ(*fld<int>(s, "r_handle"), 11);
}

// A native virtual method implements an interface method, also through a script child (F1, F2).
// The child must reach the exact overload Foo(float), not Foo(int).
TEST_P(BackendTests, NativeVirtualOverrideImplementsAnInterfaceMethod)
{
    hot::LoadResult v1 = backend->load(source("iface_native_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "NativeProbe"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "r_foo_p"), 2000);
    EXPECT_EQ(*fld<int>(s, "r_foo_c"), 2000);
    EXPECT_EQ(*fld<int>(s, "r_area_p"), 70);
    EXPECT_EQ(*fld<int>(s, "r_area_c"), 70);
    EXPECT_EQ(*fld<int>(s, "r_native_int"), 3); // Foo(int) is not overridden
}

} // namespace
