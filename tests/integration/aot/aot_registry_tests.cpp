// AOT compilation (S12, design M7), part 2. This program links spp_aot and the AOT scripts.
// It does not link the loader. The tests use the AotRegistry API.
#include "Actor.h"
#include "demo_run.h"
#include "spp/aot.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace
{

TEST(AotRegistry, RegistersTheClassesOfAllScripts)
{
    spp::AotRegistry reg(spp::aot_table);
    EXPECT_EQ(reg.script_count(), 2u);
    EXPECT_TRUE(reg.has("Fighter"));
    EXPECT_TRUE(reg.has("Hero")); // from the second script
    EXPECT_FALSE(reg.has("Nobody"));
    const std::vector<std::string> names = reg.class_names();
    EXPECT_EQ(names, (std::vector<std::string>{"Fighter", "Helper", "Hero", "Spawner", "Veteran"}));
    EXPECT_EQ(reg.class_count(), 5u);
}

TEST(AotRegistry, RecordsHaveTheGenerationAndTheParent)
{
    spp::AotRegistry reg(spp::aot_table);
    const spp::ClassRecord* veteran = reg.record("Veteran");
    ASSERT_NE(veteran, nullptr);
    EXPECT_EQ(veteran->generation, 1u);
    ASSERT_NE(veteran->super, nullptr);
    EXPECT_STREQ(veteran->super->name, "Fighter");
    EXPECT_EQ(reg.record("Nobody"), nullptr);
    EXPECT_EQ(spp::active_generation, 1u);
}

TEST(AotRegistry, CreateGivesAnObjectThatTheWorldTicks)
{
    spp::AotRegistry reg(spp::aot_table);
    World world;
    Actor* fighter = reg.create_as<Actor>("Fighter");
    world.Add(fighter);
    world.Tick(0.5f);
    world.Tick(0.5f);
    EXPECT_FLOAT_EQ(fighter->Position.x, 1.0f);
    auto* shell = dynamic_cast<spp::ShellBase*>(fighter);
    ASSERT_NE(shell, nullptr);
    ASSERT_NE(spp::field_ptr<int>(shell, "ticks"), nullptr);
    EXPECT_EQ(*spp::field_ptr<int>(shell, "ticks"), 2);
}

TEST(AotRegistry, CreateOfAnUnknownClassThrows)
{
    spp::AotRegistry reg(spp::aot_table);
    EXPECT_THROW(reg.create("Nobody"), std::runtime_error);
}

// The module variable "total" lives in a block that the registry made. Hero and Helper share it.
TEST(AotRegistry, ModuleVariablesAreSharedByTheClassesOfAScript)
{
    spp::AotRegistry reg(spp::aot_table);
    World world;
    Actor* hero = reg.create_as<Actor>("Hero");
    Actor* helper = reg.create_as<Actor>("Helper");
    world.Add(hero);
    world.Add(helper);
    world.Tick(0.1f);
    auto* shell = dynamic_cast<spp::ShellBase*>(helper);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(*spp::field_ptr<int>(shell, "seen"), 101); // Hero adds 1 (Walk), Helper adds 100
}

TEST(AotRegistry, TwoScriptsWithTheSameClassAreRejected)
{
    const spp::AotScript scripts[] = {spp::aot_table.scripts[0], spp::aot_table.scripts[0]};
    const spp::AotTable table = {scripts, 2};
    EXPECT_THROW(spp::AotRegistry reg(table), std::runtime_error);
}

TEST(AotRegistry, RegistryCanBeMadeAgainAfterItEnds)
{
    for (int i = 0; i < 2; ++i)
    {
        spp::AotRegistry reg(spp::aot_table);
        const std::string log = aot_demo::run([&](const char* name) { return reg.create(name); });
        EXPECT_NE(log.find("done"), std::string::npos);
    }
}

} // namespace
