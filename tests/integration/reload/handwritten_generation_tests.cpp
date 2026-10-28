// Tests for the hand-written generation 1 of the Player class.
// CMake defines the macro GEN1_SO (the path of the library).
#include <cstring>
#include <typeinfo>

#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;

TEST(HandwrittenGeneration, PlayerShellTicksTakesDamageAndReflects)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(GEN1_SO));
    Actor* a = s.actor;
    Actor* source = new Actor("Source");
    s.world.Add(source);

    for (int i = 0; i < 3; ++i) s.world.Tick(kDt);

    a->TakeDamage(10.0f);
    a->TakeDamage(*source, 5.0f);

    EXPECT_NE(std::strstr(typeid(*a).name(), "Player_Shell"), nullptr) << typeid(*a).name();
    EXPECT_EQ(a->Name, "Player");  // forwarded to the Actor constructor
    EXPECT_GT(a->Position.x, 0.0f);  // Tick moved the actor

    float* hp = s.field("hp");
    ASSERT_NE(hp, nullptr) << "field hp not found by reflection";
    EXPECT_EQ(*hp, 85.0f);
}


// Builds a one-field layout. Only the name, the type key and the init function matter here.
spp::FieldInfo make_field(const char* name, const char* type_key)
{
    return spp::FieldInfo{name, nullptr, type_key, 0, 4, 4, 0, nullptr, nullptr, nullptr};
}

TEST(HandwrittenGeneration, PlanRejectsATypeChange)
{
    spp::FieldInfo of[] = {make_field("hp", spp::type_key<float>())};
    spp::FieldInfo nf[] = {make_field("hp", spp::type_key<int>())};
    spp::StateLayout o{"Player", 4, 4, 1, of};
    spp::StateLayout n{"Player", 4, 4, 1, nf};
    spp::MigrationPlan plan = spp::build_plan(o, n);
    EXPECT_FALSE(plan.error.empty());
    EXPECT_NE(plan.error.find("Player: field hp changed its type"), std::string::npos)
        << plan.error;
}

TEST(HandwrittenGeneration, PlanMovesASameTypeField)
{
    spp::FieldInfo of[] = {make_field("hp", spp::type_key<float>())};
    spp::FieldInfo nf[] = {make_field("hp", spp::type_key<float>())};
    spp::StateLayout o{"Player", 4, 4, 1, of};
    spp::StateLayout n{"Player", 4, 4, 1, nf};
    spp::MigrationPlan plan = spp::build_plan(o, n);
    EXPECT_TRUE(plan.error.empty()) << plan.error;
    ASSERT_EQ(plan.steps.size(), 1u);
    EXPECT_EQ(plan.steps[0].action, spp::FieldAction::Move);
    EXPECT_EQ(plan.steps[0].old_index, 0u);
}

}  // namespace
