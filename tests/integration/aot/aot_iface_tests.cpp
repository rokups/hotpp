// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Script interfaces in AOT code (design 41b, B5). This program links hot_aot and does not link the
// loader. The registry must build the tables of the interface methods itself.
#include "Actor.h"
#include "hot/aot.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

// The calls of the interface values work before any loader exists. The rows of Wide list the
// methods of its parent Square.
TEST(AotInterfaces, CallsWorkWithoutALoader)
{
    hot::AotRegistry reg(hot::aot_table);
    World world;
    Actor* probe = reg.create_as<Actor>("Probe");
    world.Add(probe);
    world.Tick(0.5f);
    auto* shell = dynamic_cast<hot::ShellCore*>(probe);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "area_sq"), 9);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "area_wide"), 15);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "area_disc"), 31);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "scaled"), 12);
    EXPECT_EQ(*hot::field_ptr<std::string>(shell, "name"), "wide");
    EXPECT_EQ(*hot::field_ptr<int>(shell, "list_sum"), 55);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "is_wide"), 1);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "is_plain"), 0);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "threw"), 1);
}

TEST(AotInterfaces, TheRecordsHaveTheTables)
{
    hot::AotRegistry reg(hot::aot_table);
    const hot::ClassRecord* wide = reg.record("Wide");
    ASSERT_NE(wide, nullptr);
    EXPECT_NE(wide->iface_slots, nullptr);
    EXPECT_GT(wide->iface_slot_count, 0u);
    const hot::ClassRecord* plain = reg.record("Plain");
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->iface_slots, nullptr);
}

// Two registries of the same AOT table are alive at once. The records are static, so the later
// registry sets the tables. The destructor of the earlier registry must not clear them.
TEST(AotInterfaces, TheEarlierRegistryDoesNotClearTheTablesOfTheLaterOne)
{
    auto first = std::make_unique<hot::AotRegistry>(hot::aot_table);
    hot::AotRegistry second(hot::aot_table);
    first.reset();
    World world;
    Actor* probe = second.create_as<Actor>("Probe");
    world.Add(probe);
    world.Tick(0.5f);
    auto* shell = dynamic_cast<hot::ShellCore*>(probe);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "area_wide"), 15);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "threw"), 1);
}

} // namespace
