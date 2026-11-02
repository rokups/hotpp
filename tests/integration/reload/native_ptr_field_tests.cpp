// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests for a native pointer field. The field "T*" of a native class with a weak source is
// a hot::CheckedPtr<T>. It keeps its target across a reload. It reads null after the target dies.
// The tests use the backend that HOT_BACKEND selected.
#include "Equip.h"
#include "hot/backend.h"
#include "hot/runtime/checked_ptr.h"
#include "support/backend_fixture.h"
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

} // namespace
