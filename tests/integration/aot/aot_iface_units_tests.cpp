// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script interface of an imported unit in AOT code. The importer includes the interface header
// of the imported unit. The two units link into one program and share the interface helpers.
#include "Actor.h"
#include "hot/aot.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

TEST(AotInterfaceUnits, ImporterCallsTheInterfaceOfTheImportedUnit)
{
    hot::AotRegistry reg(hot::aot_table);
    World world;
    Actor* probe = reg.create_as<Actor>("ShapeProbe");
    world.Add(probe);
    world.Tick(0.5f);
    auto* shell = dynamic_cast<hot::ShellCore*>(probe);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "a"), 31);
    EXPECT_EQ(*hot::field_ptr<int>(shell, "b"), 9);
}
