// Reload tests for handle fields with the backend that SPP_BACKEND selected.
// A shared_ptr handle and an intrusive handle keep their object across reloads.
//
// CMake defines the macro SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"
#include "Object.h"
#include "Weapon.h"
#include "support/handle_reload_scenario.h"

namespace {

using test_support::kDt;
using test_support::Session;
using test_support::source;

TEST_P(BackendTests, HandleFieldSurvivesReload)
{
    spp::LoadResult v1 = backend->load(source("handle_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("handle_v2.spp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    spp::LoadResult v1b = backend->load(source("handle_v1.spp"), 3);
    ASSERT_TRUE(v1b) << v1b.error;
    handlereload::run_handle_reload(std::move(*v1.module), std::move(*v2.module), std::move(*v1b.module));
}

// A change of the field type from a handle to int is rejected. The old generation stays.
TEST_P(BackendTests, HandleTypeChangeIsRejected)
{
    spp::LoadResult v1 = backend->load(source("handle_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v3 = backend->load(source("handle_v3.spp"), 2);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    std::string error;
    EXPECT_FALSE(s.loader.request_reload(std::move(*v3.module), error));
    EXPECT_FALSE(error.empty());
    s.world.Tick(kDt);
    EXPECT_EQ(spp::active_generation, 1u);
    auto handle = spp::field_ptr<std::shared_ptr<Weapon>>(s.inst.shell, "weapon");
    ASSERT_NE(handle, nullptr);
    EXPECT_NE(handle->get(), nullptr);
}

// An intrusive handle in a field of a subclass type survives reloads. The count is right:
// no leak, no early delete and no double release.
TEST_P(BackendTests, IntrusiveHandleSurvivesReload)
{
    using H = spp::Intrusive<Item, &Object::AddRef, &Object::Release>;
    spp::LoadResult v1 = backend->load(source("intrusive_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("intrusive_v2.spp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    spp::LoadResult v1b = backend->load(source("intrusive_v1.spp"), 3);
    ASSERT_TRUE(v1b) << v1b.error;
    ASSERT_EQ(Object::live, 0);
    {
        Session s;
        ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Smith"));
        s.commit_at_frame_boundary();
        auto item = [&] { return spp::field_ptr<H>(s.inst.shell, "item"); };

        s.world.Tick(kDt);
        ASSERT_NE(item(), nullptr);
        Item* raw = item()->get();
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(Object::live, 1);
        EXPECT_EQ(raw->value, 5);

        // The reload commits after the next Tick, which is still generation 1.
        std::string error;
        ASSERT_TRUE(s.loader.request_reload(std::move(*v2.module), error)) << error;
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        EXPECT_EQ(spp::active_generation, 2u);
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
        ASSERT_TRUE(s.loader.request_reload(std::move(*v1b.module), error)) << error;
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        EXPECT_EQ(spp::active_generation, 3u);
        ASSERT_NE(item(), nullptr);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(raw->value, 7);
        EXPECT_EQ(Object::live, 1);
    }
    EXPECT_EQ(Object::live, 0);
}

// The stub names a method that the native class does not have. The load gives an error.
TEST_P(BackendTests, IntrusiveMissingMethodIsAnError)
{
    spp::LoadResult r = backend->load(source("intrusive_bad.spp"), 1);
    EXPECT_FALSE(r);
    EXPECT_NE(r.error.find("Grab"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("Object"), std::string::npos) << r.error;
}

}  // namespace
