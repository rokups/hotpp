// The F1 reload scenario. Every way to get a module runs the same checks.
//
// run_f1_reload gets the module of generation 1 (tests/scripts/player_v1.hot)
// and the module of generation 2 (tests/scripts/player_v2.hot). It checks that
// the address stays, the state migrates (hp -> health, shield 25) and a
// delegate that was bound before the reload calls the new heal.
#pragma once

#include "support/reload_assert.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <utility>

namespace f1
{

using test_support::kDt;
using test_support::Session;

inline void run_f1_reload(hot::Module v1, hot::Module v2)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(v1)));
    s.commit_at_frame_boundary();
    hot::Loader& loader = s.loader;
    World& world = s.world;
    hot::Instance& inst = s.inst;
    Actor* a = s.actor;
    const float dt = kDt;
    std::string error;
    Actor* source = new Actor("Source");
    world.Add(source);
    EXPECT_EQ(a->Name, "Player");

    auto field = [&](const char* name) { return s.field(name); };

    // Generation 1: change the state, then bind a delegate through the thunk.
    a->TakeDamage(30.0f); // hp 70
    for (int i = 0; i < 3; ++i)
        world.Tick(dt); // each Tick heals 1: hp 73
    ASSERT_NE(field("hp"), nullptr);
    EXPECT_EQ(*field("hp"), 73.0f);

    a->TakeDamage(*source, 5.0f); // hp 68, and target is set
    EXPECT_EQ(*field("hp"), 68.0f);
    std::string* target = hot::field_ptr<std::string>(inst.shell, "target");
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(*target, source->Name);

    using HealFn = void (*)(Actor*, float);
    void* thunk = loader.thunk("Player::heal");
    ASSERT_NE(thunk, nullptr);
    Delegate<void(float)> heal;
    heal.Bind(a, reinterpret_cast<HealFn>(thunk));
    heal(5.0f); // generation 1 body: hp 73
    EXPECT_EQ(*field("hp"), 73.0f);

    // Validation phase: no side effect.
    Actor* const addr = a;
    hot::ShellBase* const shell = inst.shell;
    void* const old_state = shell->state;
    ASSERT_RELOAD_OK(loader.request_reload(std::move(v2)));
    EXPECT_TRUE(loader.reload_pending());
    EXPECT_EQ(hot::active_generation, 1u);
    EXPECT_EQ(shell->state, old_state);
    EXPECT_EQ(field("health"), nullptr);

    // The commit runs at the frame boundary, after the actors ticked.
    world.Tick(dt); // generation 1 Tick heals 1: hp 74; then the commit
    ASSERT_FALSE(s.commit_failed);
    EXPECT_FALSE(loader.reload_pending());
    EXPECT_EQ(hot::active_generation, 2u);
    ASSERT_EQ(world.Actors().size(), 2u);
    EXPECT_EQ(world.Actors()[0], addr); // the same address
    EXPECT_NE(shell->state, old_state);
    EXPECT_EQ(shell->cls->generation, 2u);
    EXPECT_EQ(field("hp"), nullptr);
    ASSERT_NE(field("health"), nullptr);
    EXPECT_EQ(*field("health"), 74.0f); // health carries hp
    ASSERT_NE(field("shield"), nullptr);
    EXPECT_EQ(*field("shield"), 25.0f);
    EXPECT_EQ(*field("speed"), 5.0f);
    std::string* target2 = hot::field_ptr<std::string>(inst.shell, "target");
    ASSERT_NE(target2, nullptr);
    EXPECT_EQ(*target2, source->Name);

    // Generation 2: new Tick body, no heal in Tick.
    const float x0 = a->Position.x;
    world.Tick(dt);
    EXPECT_NEAR(a->Position.x - x0, 2.0f * 5.0f * dt, 1e-5f);
    EXPECT_EQ(*field("health"), 74.0f);

    // The delegate that was bound before the reload calls the generation 2 heal (x2).
    EXPECT_EQ(loader.thunk("Player::heal"), thunk);
    heal(5.0f);
    EXPECT_EQ(*field("health"), 84.0f);

    // Both TakeDamage overloads work in generation 2.
    a->TakeDamage(4.0f);
    EXPECT_EQ(*field("health"), 80.0f);
    a->TakeDamage(*source, 10.0f);
    EXPECT_EQ(*field("health"), 70.0f);
}

} // namespace f1
