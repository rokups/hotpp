// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests for a native pointer field. The field "T*" of a native class with a weak source is
// a hot::CheckedPtr<T>. It keeps its target across a reload. It reads null after the target dies.
// The tests use the backend that HOT_BACKEND selected.
#include "Equip.h"
#include "RefPtr.h"
#include "hot/backend.h"
#include "hot/runtime/checked_ptr.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

// The host needs the traits that the module header of the script has for the stub of RefBase.
// This is the text that hotc writes for "weak_handle".
namespace hot
{
template <class U>
struct weak_ref_traits<U, std::enable_if_t<std::is_same_v<U, ::RefBase> && !detail::has_weak_from_this<U>::value>>
    : hot::custom_weak_ref<U, RefPtr<U>, WeakRef<U>>
{
};
} // namespace hot

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

hot::CheckedPtr<Charm>* target_of(Session& s)
{
    return hot::field_ptr<hot::CheckedPtr<Charm>>(s.inst.shell, "target");
}

TEST_P(BackendTests, CheckedPtrFieldKeepsItsTargetAcrossReload)
{
    hot::LoadResult v1 = backend->load(source("native_ptr_field_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("native_ptr_field_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();

    auto charm = std::make_shared<Charm>();
    charm->power = 7;
    ASSERT_NE(target_of(s), nullptr);
    EXPECT_EQ(*target_of(s), nullptr); // the initializer "null"
    *target_of(s) = charm.get();
    s.world.Tick(kDt);
    EXPECT_EQ(target_of(s)->get(), charm.get());
    EXPECT_EQ(charm.use_count(), 1); // the field is weak: it adds no owner

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // generation 1 runs this frame; the reload commits after it
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    ASSERT_NE(target_of(s), nullptr);
    EXPECT_EQ(target_of(s)->get(), charm.get());
    EXPECT_EQ((*target_of(s))->power, 7);
}

TEST_P(BackendTests, CheckedPtrFieldReadsNullAfterTheTargetDies)
{
    hot::LoadResult v1 = backend->load(source("native_ptr_field_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();

    auto charm = std::make_shared<Charm>();
    *target_of(s) = charm.get();
    s.world.Tick(kDt);
    EXPECT_NE(target_of(s)->get(), nullptr);
    charm.reset();
    EXPECT_EQ(target_of(s)->get(), nullptr);
    EXPECT_EQ(*target_of(s), nullptr);
    EXPECT_THROW({ (void)(*target_of(s))->power; }, hot::NullReference);
}

// A stub with a custom weak handle is a weak source too. The field is a hot::CheckedPtr<RefBase>
// over RefPtr and WeakRef. It adds no strong count, and it reads null after the last RefPtr goes.
TEST_P(BackendTests, CheckedPtrFieldOverACustomWeakHandle)
{
    hot::LoadResult v1 = backend->load(source("custom_weak_ptr_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();

    auto* field = hot::field_ptr<hot::CheckedPtr<RefBase>>(s.inst.shell, "target");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(*field, nullptr); // the initializer "null"
    RefPtr<Thing> thing(new Thing(5));
    *field = RefPtr<RefBase>(thing);
    s.world.Tick(kDt);
    EXPECT_EQ(field->get(), thing.Get());
    EXPECT_EQ(thing->Refs(), 1); // the field is weak: it adds no owner
    thing = nullptr;
    EXPECT_EQ(field->get(), nullptr);
    EXPECT_THROW({ (void)(**field).Refs(); }, hot::NullReference);
}

// The expression forms on a pointer field, in both frontends. The ".hot" script and the ".cpp"
// script do the same work. The host sets "phase" and runs Tick.
template <class T> T* module_var(Session& s, const char* name)
{
    const hot::ModuleStateInfo ms = s.loader.module_state();
    if (!ms.layout || !*ms.slot)
        return nullptr;
    for (uint32_t i = 0; i < ms.layout->field_count; ++i)
        if (std::string(ms.layout->fields[i].name) == name)
            return reinterpret_cast<T*>(static_cast<char*>(*ms.slot) + ms.layout->fields[i].offset);
    return nullptr;
}

void run_forms_script(hot::Backend& backend, const char* file)
{
    hot::LoadResult v1 = backend.load(source(file), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Prober"));
    s.commit_at_frame_boundary();
    auto geti = [&](const char* name) { return *hot::field_ptr<int>(s.inst.shell, name); };
    auto seti = [&](const char* name, int v) { *hot::field_ptr<int>(s.inst.shell, name) = v; };
    auto* other = hot::field_ptr<hot::CheckedPtr<Charm>>(s.inst.shell, "other");
    auto* g_other = module_var<hot::CheckedPtr<Charm>>(s, "g_other");
    ASSERT_NE(other, nullptr);
    ASSERT_NE(g_other, nullptr);
    Vault* vault = dynamic_cast<Vault*>(s.actor);
    ASSERT_NE(vault, nullptr);
    vault->held = std::make_shared<Charm>();
    vault->held->power = 4;

    // The field starts null: "other == null" is true and the guarded block does not run.
    s.world.Tick(kDt);
    EXPECT_EQ(geti("is_null"), 1);
    EXPECT_EQ(geti("via_call"), -1);

    // "other = Best()": a raw pointer from a native call. Then "other.Twice()", a native
    // call with the pointer, a lambda that uses this, and a module variable pointer.
    seti("phase", 1);
    s.world.Tick(kDt);
    EXPECT_EQ(other->get(), vault->held.get());
    EXPECT_EQ(g_other->get(), vault->held.get());
    EXPECT_EQ(geti("is_null"), 0);
    EXPECT_EQ(vault->held->power, 5); // Bump(1)
    EXPECT_EQ(geti("via_call"), 10);
    EXPECT_EQ(geti("via_pass"), 5);
    EXPECT_EQ(geti("via_lambda"), 10);
    EXPECT_EQ(geti("via_module"), 10);
    EXPECT_EQ(vault->held.use_count(), 1); // the field and the module variable are weak

    // "other = null".
    seti("phase", 3);
    s.world.Tick(kDt);
    EXPECT_EQ(*other, nullptr);
    EXPECT_EQ(*g_other, nullptr);
    EXPECT_EQ(geti("is_null"), 1);
    EXPECT_EQ(geti("via_call"), -1);

    // "other = h": a handle.
    seti("phase", 2);
    s.world.Tick(kDt);
    EXPECT_EQ(other->get(), vault->held.get());
    EXPECT_EQ(geti("is_null"), 0);
    EXPECT_EQ(vault->held->power, 6);
    EXPECT_EQ(geti("via_call"), 12);
    EXPECT_EQ(vault->held.use_count(), 1);

    // The target dies: the field reads null and the script takes the null branch.
    seti("phase", 0);
    vault->held.reset();
    s.world.Tick(kDt);
    EXPECT_EQ(geti("is_null"), 1);
    EXPECT_EQ(geti("via_call"), -1);
}

TEST_P(BackendTests, CheckedPtrFormsInAHotScript)
{
    run_forms_script(*backend, "native_ptr_forms.hot");
}

TEST_P(BackendTests, CheckedPtrFormsInACppScript)
{
    run_forms_script(*backend, "cpp_native_ptr_forms.cpp");
}

} // namespace
