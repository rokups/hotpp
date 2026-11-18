// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script interface of an imported unit in AOT code. The importer includes the interface header
// of the imported unit. The two units link into one program and share the interface helpers.
#include "Actor.h"
#include "hot/aot.h"
#include "hot/runtime.h"
#ifdef AOT_IFACE_LOADER
#include "hot/backend.h"
#include "hot/runtime/loader.h"
#include "support/script_files.h"
#endif

#include <gtest/gtest.h>

#include <memory>
#include <string>

#ifdef AOT_IFACE_LOADER
// The loader data of the interface unit and of its importer equals the translator data. The lib has
// an interface and a class, the user has a sibling class and an import of the lib (value and handle).
TEST(AotInterfaceUnits, LoaderFineDataEqualsTheTranslatorData)
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    backend->seed_slots(hot::aot_table.slots);
    hot::Loader loader;
    const hot::ReloadResult r = loader.load_aot(hot::aot_table);
    ASSERT_TRUE(r) << r.error;
    ASSERT_EQ(hot::aot_table.count, 2u);
    for (size_t i = 0; i < hot::aot_table.count; ++i)
    {
        const std::string unit = hot::aot_table.scripts[i].name;
        hot::ScriptSource src = test_support::source(("aot_iface_units/" + unit + ".hot").c_str());
        src.unit = unit;
        hot::LoadResult built = backend->load(src, 2);
        ASSERT_TRUE(built) << unit << ": " << built.error;
        hot::IfaceInfo iface;
        hot::UseInfo use;
        ASSERT_TRUE(loader.aot_unit_info(unit, iface, use)) << unit;
        EXPECT_TRUE(iface.known) << unit;
        EXPECT_TRUE(use.known) << unit;
        EXPECT_EQ(iface.hash, built.iface_info.hash) << unit;
        EXPECT_EQ(iface.shape, built.iface_info.shape) << unit;
        EXPECT_EQ(iface.members, built.iface_info.members) << unit;
        EXPECT_EQ(iface.names, built.iface_info.names) << unit;
        EXPECT_EQ(iface.copied_names, built.iface_info.copied_names) << unit;
        EXPECT_EQ(use.names, built.use_info.names) << unit;
        EXPECT_EQ(use.strong, built.use_info.strong) << unit;
    }
}

#else
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
#endif
