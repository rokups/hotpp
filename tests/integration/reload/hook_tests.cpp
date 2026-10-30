// Reload tests of the hooks (design 4.7 and 6.6) with the backend that SPP_BACKEND selected:
// onReload(OldState), the convert hook, a type change without a hook and a rename without [alias].
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

// Generation 1 to 2: alias, onReload with OldState, convert hook, default value, rename warning.
TEST_P(BackendTests, HooksMigrateConvertAndWarn)
{
    hot::LoadResult v1 = backend->load(source("hooks_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("hooks_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_NE(fld<float>(s, "hp"), nullptr);
    EXPECT_EQ(*fld<float>(s, "hp"), 120.0f);
    Actor* before = s.actor;

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStderr();
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);

    EXPECT_EQ(s.actor, before);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(*fld<int>(s, "ticks"), 3); // migrated 2, plus the Tick of generation 2
    EXPECT_EQ(*fld<float>(s, "health"), 100.0f); // alias moved 120, the hook clamped it
    EXPECT_EQ(*fld<float>(s, "old_hp"), 120.0f); // the hook read the old field
    EXPECT_EQ(*fld<int>(s, "reloads"), 1); // the hook ran once
    EXPECT_EQ(*fld<float>(s, "level"), 30.0f); // convert_level(3)
    EXPECT_EQ(*fld<float>(s, "energy"), 5.0f); // rename without alias: default
    EXPECT_EQ(*fld<int>(s, "title"), 0); // std::string to int: default

    EXPECT_NE(log.find("field mana is removed and field energy is added"), std::string::npos) << log;
    EXPECT_NE(log.find("field title changed its type"), std::string::npos) << log;
    EXPECT_EQ(log.find("field level changed its type"), std::string::npos) << log;
}

// A hook that throws is logged. The reload completes and the object stays valid.
TEST_P(BackendTests, ThrowingOnReloadHookIsCaught)
{
    hot::LoadResult v1 = backend->load(source("hooks_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("hooks_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v3 = backend->load(source("hooks_v3.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    ASSERT_EQ(hot::active_generation, 2u);
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v3.module)));
    testing::internal::CaptureStderr();
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_FALSE(s.commit_failed);
    EXPECT_NE(log.find("ERROR: onReload of Hero threw: hook failed"), std::string::npos) << log;
    EXPECT_EQ(hot::active_generation, 3u);
    EXPECT_EQ(*fld<int>(s, "reloads"), 101); // 1 from generation 2, then 100 before the throw
    EXPECT_EQ(*fld<float>(s, "hp"), 100.0f); // the field health moved by [alias]
    s.world.Tick(kDt); // the object still runs
    EXPECT_EQ(*fld<int>(s, "ticks"), 3);
}

} // namespace
