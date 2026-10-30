// Reload tests of native interface bases (S8b, design 4.4 and 5.3) with the backend that
// SPP_BACKEND selected. The shell of Boss derives from Actor, IDamageable and ITagged.
#include "Damageable.h"
#include "spp/backend.h"
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
    return spp::field_ptr<T>(s.inst.shell, name);
}

// Native code finds the interfaces in the shell with dynamic_cast. A call through an interface
// reads the slot of the script override. A virtual without an override calls the native code.
TEST_P(BackendTests, InterfaceVirtualsDispatchThroughTheShell)
{
    spp::LoadResult v1 = backend->load(source("boss_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Boss"));
    s.commit_at_frame_boundary();
    EXPECT_STREQ(s.inst.shell->cls->base_name, "Actor, IDamageable, ITagged");
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    auto* tag = dynamic_cast<ITagged*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(dmg->Absorb(5), 45); // the script override
    EXPECT_EQ(dmg->Armor(), 2); // no override: IDamageable::Armor
    EXPECT_EQ(tag->Tag(), 7); // no override: ITagged::Tag
    EXPECT_EQ(HitThrough(*dmg, 10), 35 + 2); // native code with an interface reference
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ticks"), 1); // the virtual of the primary base
    EXPECT_EQ(*fld<int>(s, "base_armor"), 2); // "base.Armor()" calls IDamageable::Armor
    EXPECT_EQ(*fld<int>(s, "absorbed"), 2);
}

// A reload with the same bases is LIVE. A new override fills the slot of an interface virtual.
// The address of the object and its state stay.
TEST_P(BackendTests, ReloadAddsOverridesOfInterfaceVirtuals)
{
    spp::LoadResult v1 = backend->load(source("boss_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("boss_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Boss"));
    s.commit_at_frame_boundary();
    auto* dmg = dynamic_cast<IDamageable*>(s.actor);
    auto* tag = dynamic_cast<ITagged*>(s.actor);
    ASSERT_NE(dmg, nullptr);
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(dmg->Absorb(5), 45);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(dynamic_cast<IDamageable*>(s.actor), dmg);
    EXPECT_EQ(dmg->Absorb(5), 35); // the body of generation 2: amount * 2, hp kept 45
    EXPECT_EQ(dmg->Armor(), 5); // the new override
    EXPECT_EQ(tag->Tag(), 70); // the new override
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "base_armor"), 2); // "base.Armor()" still calls the native code
    EXPECT_EQ(*fld<int>(s, "absorbed"), 2);
}

// A reload that removes an interface base changes the shell (design 5.3: RECONSTRUCT). The
// loader rejects it. The old generation stays.
TEST_P(BackendTests, ReloadThatRemovesAnInterfaceBaseIsRejected)
{
    spp::LoadResult v1 = backend->load(source("boss_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult bad = backend->load(source("boss_rebase.hot"), 2);
    ASSERT_TRUE(bad) << bad.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Boss"));
    s.commit_at_frame_boundary();
    const spp::ReloadResult rejected = s.loader.request_reload(std::move(*bad.module));
    const std::string& error = rejected.error;
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status, spp::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(rejected.reason, spp::ReloadReason::InterfaceBasesChanged);
    EXPECT_EQ(rejected.classes, std::vector<std::string>{"Boss"});
    EXPECT_NE(error.find("class Boss: native interface bases changed (Actor, IDamageable, ITagged -> "
                         "Actor, IDamageable; RECONSTRUCT change)"),
        std::string::npos)
        << error;
    EXPECT_FALSE(s.loader.reload_pending());
    s.world.Tick(kDt);
    EXPECT_EQ(spp::active_generation, 1u);
    EXPECT_EQ(dynamic_cast<ITagged*>(s.actor)->Tag(), 7);
}

// A reload that adds an interface base is rejected too.
TEST_P(BackendTests, ReloadThatAddsAnInterfaceBaseIsRejected)
{
    spp::LoadResult v1 = backend->load(source("boss_rebase.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult bad = backend->load(source("boss_v1.hot"), 2);
    ASSERT_TRUE(bad) << bad.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Boss"));
    s.commit_at_frame_boundary();
    const spp::ReloadResult rejected = s.loader.request_reload(std::move(*bad.module));
    const std::string& error = rejected.error;
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status, spp::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(rejected.reason, spp::ReloadReason::InterfaceBasesChanged);
    EXPECT_NE(error.find("native interface bases changed (Actor, IDamageable -> Actor, IDamageable, ITagged"),
        std::string::npos)
        << error;
    EXPECT_FALSE(s.loader.reload_pending());
    EXPECT_EQ(spp::active_generation, 1u);
}

} // namespace
