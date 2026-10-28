// Step R1: reload with migration, a thunk and two rejected reloads.
// CMake defines the macros GEN1_SO, GEN2_SO, GEN4_BASE_SO and GEN3_MISSING_SO.
// The file GEN3_MISSING_SO does not exist on purpose.
#include <cmath>
#include <string>

#include "test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;

TEST(R1, ReloadMigratesAndRejectedReloadsKeepGeneration2)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(GEN1_SO));
    s.commit_at_frame_boundary();
    Actor* a = s.actor;
    World& world = s.world;
    auto field = [&](const char* name) { return s.field(name); };
    std::string error;

    // Generation 1: change the state, then bind a delegate through the thunk.
    a->TakeDamage(30.0f);                          // hp 70
    for (int i = 0; i < 3; ++i) world.Tick(kDt);   // each Tick heals 1: hp 73
    ASSERT_NE(field("hp"), nullptr);
    EXPECT_EQ(*field("hp"), 73.0f);

    using HealFn = void (*)(Actor*, float);
    void* thunk = s.loader.thunk("Player::heal");
    ASSERT_NE(thunk, nullptr);
    Delegate<void(float)> heal;
    heal.Bind(a, reinterpret_cast<HealFn>(thunk));
    heal(5.0f);  // generation 1 body: hp 78
    EXPECT_EQ(*field("hp"), 78.0f);

    // Validation phase: no side effect.
    Actor* const addr = a;
    script::ShellBase* const shell = s.inst.shell;
    void* const old_state = shell->state;
    ASSERT_TRUE(s.loader.request_reload(GEN2_SO, error)) << error;
    EXPECT_TRUE(s.loader.reload_pending());
    EXPECT_EQ(script::active_generation, 1u);
    EXPECT_EQ(shell->state, old_state);
    ASSERT_NE(field("hp"), nullptr);
    EXPECT_EQ(*field("hp"), 78.0f);
    EXPECT_EQ(field("health"), nullptr);
    EXPECT_EQ(field("shield"), nullptr);

    // The commit runs at the frame boundary, after the actors ticked.
    world.Tick(kDt);  // generation 1 Tick heals 1: hp 79; then the commit
    ASSERT_FALSE(s.commit_failed);
    EXPECT_FALSE(s.loader.reload_pending());
    EXPECT_EQ(script::active_generation, 2u);
    ASSERT_EQ(world.Actors().size(), 1u);
    EXPECT_EQ(world.Actors()[0], addr);  // the same address
    EXPECT_NE(shell->state, old_state);
    EXPECT_EQ(shell->cls->generation, 2u);
    EXPECT_EQ(field("hp"), nullptr);
    ASSERT_NE(field("health"), nullptr);
    EXPECT_EQ(*field("health"), 79.0f);  // health carries hp
    ASSERT_NE(field("shield"), nullptr);
    EXPECT_EQ(*field("shield"), 25.0f);
    ASSERT_NE(field("speed"), nullptr);
    EXPECT_EQ(*field("speed"), 5.0f);

    // Generation 2: new Tick body, no heal in Tick.
    const float x0 = a->Position.x;
    world.Tick(kDt);
    EXPECT_NEAR(a->Position.x - x0, 2.0f * 5.0f * kDt, 1e-5f);
    EXPECT_EQ(*field("health"), 79.0f);

    // The delegate bound before the reload calls the generation 2 heal (x2).
    EXPECT_EQ(s.loader.thunk("Player::heal"), thunk);
    heal(5.0f);
    EXPECT_EQ(*field("health"), 89.0f);

    // Rejected reloads: other shell layout, and a library that is not there.
    void* const state2 = shell->state;
    const float pos2 = a->Position.x;
    EXPECT_FALSE(s.loader.request_reload(GEN4_BASE_SO, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(s.loader.reload_pending());
    error.clear();
    EXPECT_FALSE(s.loader.request_reload(GEN3_MISSING_SO, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(s.loader.reload_pending());

    EXPECT_EQ(script::active_generation, 2u);
    ASSERT_EQ(world.Actors().size(), 1u);
    EXPECT_EQ(world.Actors()[0], addr);
    EXPECT_EQ(shell->state, state2);
    EXPECT_EQ(shell->cls->generation, 2u);
    EXPECT_EQ(*field("health"), 89.0f);
    EXPECT_EQ(*field("shield"), 25.0f);
    EXPECT_EQ(*field("speed"), 5.0f);
    EXPECT_EQ(a->Position.x, pos2);

    // The object still ticks with the unchanged state.
    world.Tick(kDt);
    EXPECT_FALSE(s.commit_failed);
    EXPECT_NEAR(a->Position.x - pos2, 2.0f * 5.0f * kDt, 1e-5f);
    EXPECT_EQ(*field("health"), 89.0f);
    EXPECT_EQ(*field("shield"), 25.0f);
    heal(1.0f);
    EXPECT_EQ(*field("health"), 91.0f);
}

}  // namespace
