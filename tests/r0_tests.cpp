// Step R0: tests for the hand-written generation 1 of the Player class.
// CMake defines the macro GEN1_SO (the path of the library).
#include <cstring>
#include <typeinfo>

#include "test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;

TEST(R0, PlayerShellTicksTakesDamageAndReflects)
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

}  // namespace
