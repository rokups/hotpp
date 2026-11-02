// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests for handle fields with the backend that HOT_BACKEND selected.
// A shared_ptr handle and an intrusive handle keep their object across reloads.
//
// CMake defines the macro HOT_SOURCE_DIR.
#include "Object.h"
#include "RefPtr.h"
#include "Weapon.h"
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/handle_reload_scenario.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

TEST_P(BackendTests, HandleFieldSurvivesReload)
{
    hot::LoadResult v1 = backend->load(source("handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("handle_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v1b = backend->load(source("handle_v1.hot"), 3);
    ASSERT_TRUE(v1b) << v1b.error;
    handlereload::run_handle_reload(std::move(*v1.module), std::move(*v2.module), std::move(*v1b.module));
}

// A change of the field type from a handle to int does not reject the reload (design 4.7).
// The field gets its default value and the runtime logs a warning. The other fields migrate.
// The Weapon object dies with the old block.
TEST_P(BackendTests, HandleTypeChangeUsesTheDefaultAndWarns)
{
    hot::LoadResult v1 = backend->load(source("handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v3 = backend->load(source("handle_v3.hot"), 2);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    auto swings = hot::field_ptr<int>(s.inst.shell, "swings");
    ASSERT_NE(swings, nullptr);
    EXPECT_EQ(*swings, 2);
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v3.module)));
    testing::internal::CaptureStderr();
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(s.commit_failed);
    EXPECT_NE(log.find("WARNING: Knight"), std::string::npos) << log;
    EXPECT_NE(log.find("field weapon changed its type"), std::string::npos) << log;
    EXPECT_EQ(hot::active_generation, 2u);
    auto weapon = hot::field_ptr<int>(s.inst.shell, "weapon");
    ASSERT_NE(weapon, nullptr);
    EXPECT_EQ(*weapon, 0);
    swings = hot::field_ptr<int>(s.inst.shell, "swings");
    ASSERT_NE(swings, nullptr);
    EXPECT_EQ(*swings, 3); // the migrated value plus the Tick of generation 3
}

// An intrusive handle in a field of a subclass type survives reloads. The count is right:
// no leak, no early delete and no double release.
TEST_P(BackendTests, IntrusiveHandleSurvivesReload)
{
    using H = hot::Intrusive<Item, &Object::AddRef, &Object::Release>;
    hot::LoadResult v1 = backend->load(source("intrusive_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("intrusive_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v1b = backend->load(source("intrusive_v1.hot"), 3);
    ASSERT_TRUE(v1b) << v1b.error;
    ASSERT_EQ(Object::live, 0);
    {
        Session s;
        ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Smith"));
        s.commit_at_frame_boundary();
        auto item = [&] { return hot::field_ptr<H>(s.inst.shell, "item"); };

        s.world.Tick(kDt);
        ASSERT_NE(item(), nullptr);
        Item* raw = item()->get();
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(Object::live, 1);
        EXPECT_EQ(raw->value, 5);

        // The reload commits after the next Tick, which is still generation 1.
        std::string error;
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        EXPECT_EQ(hot::active_generation, 2u);
        ASSERT_NE(item(), nullptr);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(Object::live, 1);

        // Generation 2: the field "spare" shares the object.
        s.world.Tick(kDt);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 2);
        EXPECT_EQ(raw->value, 6);
        EXPECT_EQ(Object::live, 1);

        // Back to generation 1: the commit removes the field "spare" and releases it.
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v1b.module)));
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        EXPECT_EQ(hot::active_generation, 3u);
        ASSERT_NE(item(), nullptr);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(raw->value, 7);
        EXPECT_EQ(Object::live, 1);
    }
    EXPECT_EQ(Object::live, 0);
}

// A custom intrusive handle (RefPtr) and its weak handle (WeakRef, "weak_handle"). A native
// class and a script class with a native RefBase base use them. "[weak]" reads give null after
// the last strong handle goes.
TEST_P(BackendTests, CustomIntrusiveAndWeakHandle)
{
    hot::LoadResult v1 = backend->load(source("custom_handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Keeper"));
    s.commit_at_frame_boundary();
    auto field = [&](const char* name) { return hot::field_ptr<int>(s.inst.shell, name); };

    s.world.Tick(kDt);
    auto held = hot::field_ptr<RefPtr<Thing>>(s.inst.shell, "held");
    ASSERT_NE(held, nullptr);
    ASSERT_NE(held->Get(), nullptr);
    EXPECT_EQ(held->Get()->Refs(), 1);
    ASSERT_NE(field("alive"), nullptr);
    EXPECT_EQ(*field("alive"), 4);
    EXPECT_EQ(*field("petValue"), 10);

    s.world.Tick(kDt);
    EXPECT_EQ(*field("alive"), 4);
    s.world.Tick(kDt); // the script drops the strong handles
    EXPECT_EQ(*field("alive"), -1);
    EXPECT_EQ(*field("petValue"), -1);
}

// Script attributes: the loader stats give the tags of each class. A reload changes them.
TEST_P(BackendTests, ScriptAttributesAreInTheStats)
{
    hot::LoadResult v1 = backend->load(source("attributes_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("attributes_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    s.commit_at_frame_boundary();
    auto attributes = [&](const std::string& name)
    {
        for (const hot::ClassInfo& c : s.loader.stats().classes)
            if (c.name == name)
                return c.attributes;
        return std::vector<std::string>{"<no class>"};
    };
    EXPECT_EQ(attributes("Player"), (std::vector<std::string>{"Spawnable", "Editor"}));
    EXPECT_EQ(attributes("Plain"), std::vector<std::string>{});

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(attributes("Player"), std::vector<std::string>{"Editor"});
    EXPECT_EQ(attributes("Plain"), std::vector<std::string>{"Spawnable"});
}

// The stub names a method that the native class does not have. The load gives an error.
TEST_P(BackendTests, IntrusiveMissingMethodIsAnError)
{
    hot::LoadResult r = backend->load(source("intrusive_bad.hot"), 1);
    EXPECT_FALSE(r);
    EXPECT_NE(r.error.find("Grab"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("Object"), std::string::npos) << r.error;
}

} // namespace
