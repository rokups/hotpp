// The handle reload scenario. Every way to get a module runs the same checks.
//
// run_handle_reload gets the modules of the scripts handle_v1, handle_v2 and handle_v1
// again (tests/scripts). It checks that a shared_ptr field keeps its object.
#pragma once

#include "Weapon.h"
#include "support/reload_assert.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace handlereload
{

using test_support::kDt;
using test_support::Session;

// A handle field (std::shared_ptr<Weapon>) must keep its object across reloads.
inline void run_handle_reload(hot::Module v1, hot::Module v2, hot::Module v1_again)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(v1), "Knight"));
    s.commit_at_frame_boundary();
    auto handle = [&] { return hot::field_ptr<std::shared_ptr<Weapon>>(s.inst.shell, "weapon"); };

    s.world.Tick(kDt);
    ASSERT_NE(handle(), nullptr);
    ASSERT_NE(handle()->get(), nullptr);
    const Weapon* raw = handle()->get();
    EXPECT_EQ(handle()->use_count(), 1);
    EXPECT_EQ((*handle())->dmg, 7);
    EXPECT_EQ((*handle())->name, "sword");

    // The reload commits after the next Tick, which is still generation 1.
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(v2)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    ASSERT_NE(handle(), nullptr);
    EXPECT_EQ(handle()->get(), raw);
    EXPECT_EQ(handle()->use_count(), 1);
    EXPECT_EQ((*handle())->dmg, 7);
    ASSERT_NE(s.field("rage"), nullptr);
    EXPECT_FLOAT_EQ(*s.field("rage"), 1.0f);

    s.world.Tick(kDt);
    EXPECT_EQ((*handle())->dmg, 8);
    EXPECT_EQ(handle()->get(), raw);

    // Back to generation 1: the handle is the same object.
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(v1_again)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    ASSERT_NE(handle(), nullptr);
    EXPECT_EQ(handle()->get(), raw);
    EXPECT_EQ(handle()->use_count(), 1);
    // The Tick before the commit still ran generation 2.
    EXPECT_EQ((*handle())->dmg, 9);
}

} // namespace handlereload
