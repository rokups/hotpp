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

// Reference, const reference, [weak], and script class parameters (F3). The call must
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
    EXPECT_EQ(*fld<int>(s, "r_val_after"), 1); // a argument is a copy
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

// ---- Reload with a stored interface value of a class that changes (design 41b, D10).
// The reload is lazy: the field keeps the value. Only the table of the class decides if a call works.

struct HolderProbe
{
    Session& s;
    int i(const char* name) const { return *fld<int>(s, name); }
    std::string why() const { return *fld<std::string>(s, "why"); }
};

// Starts generation 1 with the Holder and runs one frame.
void start_holder(Session& s, hot::LoadResult& v1)
{
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

// The class C drops IThing on a reload. The stored value keeps the object. "obj is IThing" is
// false and the call throws. A later reload that lists IThing again makes the calls work.
TEST_P(BackendTests, ClassThatDropsTheInterfaceThrowsAtTheCallAndRecovers)
{
    hot::LoadResult v4 = backend->load(source("iface_v4.hot"), 1);
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_holder(s, v4));
    const HolderProbe p{s};
    EXPECT_EQ(p.i("r"), 1);
    EXPECT_EQ(p.i("is_i"), 1);
    EXPECT_EQ(p.i("threw"), 0);

    hot::LoadResult v5 = backend->load(source("iface_v5.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v5, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("is_i"), 0);
    EXPECT_EQ(p.i("threw"), 1);
    EXPECT_EQ(p.i("r"), -5);
    EXPECT_EQ(p.why(), "class 'C' does not implement 'IThing.Get'");
    s.world.Tick(kDt); // a second throw is the same
    EXPECT_EQ(p.i("threw"), 1);

    hot::LoadResult v6 = backend->load(source("iface_v6.hot"), 3);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v6, 3));
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("is_i"), 1);
    EXPECT_EQ(p.i("threw"), 0);
    EXPECT_EQ(p.i("r"), 3); // the same object, the new body
}

// The signature of IThing.Get changes in the interface and in the class. The stored value finds
// the new method: the call site of the new generation has a new id.
TEST_P(BackendTests, InterfaceSignatureChangeOnBothSidesReachesTheNewMethod)
{
    hot::LoadResult v7 = backend->load(source("iface_v7.hot"), 1);
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_holder(s, v7));
    const HolderProbe p{s};
    EXPECT_EQ(p.i("r"), 11);

    hot::LoadResult v8 = backend->load(source("iface_v8.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v8, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("threw"), 0);
    EXPECT_EQ(p.i("r"), 30);
    EXPECT_EQ(p.i("is_i"), 1);
}

// A change on one side only is a translate error: the class does not implement the interface.
TEST_P(BackendTests, InterfaceSignatureChangeOnOneSideIsATranslateError)
{
    for (const char* file : {"iface_bad_iface_only.hot", "iface_bad_class_only.hot"})
    {
        hot::LoadResult bad = backend->load(source(file), 2);
        EXPECT_FALSE(bad) << file;
        EXPECT_NE(bad.error.find("does not implement"), std::string::npos) << file << ": " << bad.error;
    }
}

// The class C is removed. A call through the stored value goes to the handler of the removed
// class: it is ignored and counted. It does not throw.
TEST_P(BackendTests, InterfaceCallOfARemovedClassIsIgnoredAndCounted)
{
    hot::LoadResult v4 = backend->load(source("iface_v4.hot"), 1);
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_holder(s, v4));
    const HolderProbe p{s};
    EXPECT_EQ(p.i("r"), 1);

    hot::LoadResult v9 = backend->load(source("iface_v9.hot"), 2);
    const uint64_t calls0 = hot::removed_class_call_count();
    testing::internal::CaptureStderr();
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v9, 2));
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("WARNING: class C is removed from the source; 1 live object(s)"), std::string::npos) << log;
    EXPECT_EQ(p.i("threw"), 0);
    EXPECT_GE(hot::removed_class_call_count(), calls0 + 2);
    EXPECT_EQ(p.i("r"), 0); // the handler gives a zero result
}

// The interface is renamed and the field keep changes its type. The old value does not convert,
// so the field gets its initializer (null). The Tick assigns a new object, and the call works.
TEST_P(BackendTests, RenamedInterfaceChangesTheTypeOfTheStoredField)
{
    hot::LoadResult v4 = backend->load(source("iface_v4.hot"), 1);
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_holder(s, v4));
    const HolderProbe p{s};
    hot::LoadResult v10 = backend->load(source("iface_v10.hot"), 2);
    testing::internal::CaptureStderr();
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v10, 2));
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("field keep changed its type"), std::string::npos) << log;
    EXPECT_EQ(p.i("threw"), 0);
    EXPECT_EQ(p.i("r"), 10);
    EXPECT_EQ(p.i("is_i"), 1);
}

// The interface is removed and the field keep becomes an int. The field gets its initializer.
TEST_P(BackendTests, RemovedInterfaceChangesTheFieldToAnInt)
{
    hot::LoadResult v4 = backend->load(source("iface_v4.hot"), 1);
    Session s;
    ASSERT_NO_FATAL_FAILURE(start_holder(s, v4));
    const HolderProbe p{s};
    hot::LoadResult v11 = backend->load(source("iface_v11.hot"), 2);
    testing::internal::CaptureStderr();
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v11, 2));
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("field keep changed its type"), std::string::npos) << log;
    EXPECT_EQ(p.i("keep"), 7);
    EXPECT_EQ(p.i("r"), 7);
    EXPECT_EQ(p.i("threw"), 0);
}

// A script class with a native intrusive base gets an interface by a reload. The interface value
// shares the object by a deleter that holds a handle: the count grows by one. The object lives
// while the interface value lives, also after the class handle is gone.
TEST_P(BackendTests, InterfaceOnAClassWithAnIntrusiveNativeBase)
{
    hot::LoadResult v12 = backend->load(source("iface_v12.hot"), 1);
    ASSERT_TRUE(v12) << v12.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v12.module), "Keeper"));
    s.commit_at_frame_boundary();
    const HolderProbe p{s};
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("refs"), 1);
    EXPECT_EQ(p.i("alive"), 9);

    hot::LoadResult v13 = backend->load(source("iface_v13.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v13, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("refs"), 1);
    EXPECT_EQ(p.i("via"), -1);

    *fld<int>(s, "step") = 1; // the interface value shares the object
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("refs"), 2);
    EXPECT_EQ(p.i("via"), 10);
    EXPECT_EQ(p.i("alive"), 9);

    *fld<int>(s, "step") = 2; // the class handle goes, the interface value keeps the object
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("refs"), 1);
    EXPECT_EQ(p.i("via"), 10);
    EXPECT_EQ(p.i("alive"), 9);

    *fld<int>(s, "step") = 3; // the interface value goes: the count is zero and the object dies
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("via"), -1);
    EXPECT_EQ(p.i("alive"), -1);
}

// A reload that removes the interface (and the field iv) releases the interface value. The class
// handle keeps the object and the count is back to one.
TEST_P(BackendTests, ReloadThatRemovesTheInterfaceFieldReleasesTheIntrusiveObject)
{
    hot::LoadResult v13 = backend->load(source("iface_v13.hot"), 1);
    ASSERT_TRUE(v13) << v13.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v13.module), "Keeper"));
    s.commit_at_frame_boundary();
    const HolderProbe p{s};
    s.world.Tick(kDt);
    *fld<int>(s, "step") = 1;
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("refs"), 2);

    hot::LoadResult v12 = backend->load(source("iface_v12.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload_to(s, v12, 2));
    *fld<int>(s, "step") = 0;
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("refs"), 1); // the field iv is gone and its deleter released the handle
    EXPECT_EQ(p.i("alive"), 9);
    *fld<int>(s, "step") = 2;
    s.world.Tick(kDt);
    EXPECT_EQ(p.i("alive"), -1);
}

} // namespace
