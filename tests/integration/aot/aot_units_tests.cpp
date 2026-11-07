// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Two AOT scripts as two units (modules step 6). "aot_unit_app" imports "aot_unit_base": "new" of a
// class of another unit, a field and a method of that class, a module variable and an overloaded
// free function of another unit.
// AOT_UNITS_LOADER: the test uses Loader::load_aot. Else it uses AotRegistry (hot::aot).
#include "Actor.h"
#include "hot/aot.h"
#include "hot/runtime.h"
#ifdef AOT_UNITS_LOADER
#include "hot/runtime/loader.h"
#endif
#if defined(AOT_UNITS_LOADER) && defined(AOT_UNITS_CHAIN)
#include "hot/backend.h"
#include "support/script_files.h"
#endif

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

// hp: 10 + 1, and 1100.
constexpr float kScore = 1111.0f;
// Scale(5) gives 10 and Scale(1.5f) gives 700.
constexpr float kScale = 710.0f;
// The box of size 4: area 16, size 4. base_count: 3 + 1.
constexpr float kBox = 1600.0f + 40.0f + 4.0f;

// The world owns the object: the caller keeps the world while it uses the object.
void expect_one_tick(World& world, Actor* app)
{
    world.Add(app);
    world.Tick(0.5f);
    EXPECT_FLOAT_EQ(app->Position.x, kScore);
    EXPECT_FLOAT_EQ(app->Position.y, kScale);
    EXPECT_FLOAT_EQ(app->Position.z, kBox);
}

#ifndef AOT_EXPECT_OPTIMIZED
const hot::PropertyInfo* find_property(const hot::StateLayout& layout, const char* name)
{
    for (uint32_t i = 0; i < layout.prop_count; ++i)
        if (std::strcmp(layout.props[i].name, name) == 0)
            return &layout.props[i];
    return nullptr;
}

// A property with an attribute tag has a row with accessors (G20).
void expect_total_property(Actor* app)
{
    auto* shell = dynamic_cast<hot::ShellBase*>(app);
    ASSERT_NE(shell, nullptr);
    const hot::StateLayout* layout = shell->cls->layout;
    ASSERT_NE(layout, nullptr);
    const hot::PropertyInfo* total = find_property(*layout, "Total");
    ASSERT_NE(total, nullptr);
    ASSERT_NE(total->attributes, nullptr);
    EXPECT_STREQ(total->attributes[0].name, "Attribute");
    EXPECT_STREQ(total->attributes[0].args[0], "Total");
    int32_t v = 0;
    total->get(shell, &v);
    EXPECT_EQ(v, static_cast<int32_t>(kScore));
    const int32_t next = 21;
    total->set(shell, &next); // the setter doubles the value
    total->get(shell, &v);
    EXPECT_EQ(v, 42);
}
#endif

// The thunk "Cls::__native" gives the native base of an object from its shell.
void expect_native_thunk(void* thunk, Actor* app)
{
    ASSERT_NE(thunk, nullptr);
    using NativeOf = void* (*)(hot::ShellCore*);
    auto* shell = dynamic_cast<hot::ShellCore*>(app);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(reinterpret_cast<NativeOf>(thunk)(shell), static_cast<void*>(app));
}

TEST(AotUnits, TableListsTheImportsWithTheInterfaceHashes)
{
    std::vector<std::string> names;
    const hot::AotScript* app = nullptr;
    for (size_t i = 0; i < hot::aot_table.count; ++i)
    {
        names.push_back(hot::aot_table.scripts[i].name);
        EXPECT_NE(hot::aot_table.scripts[i].iface_hash, 0u);
        if (names.back() == "aot_unit_app")
            app = &hot::aot_table.scripts[i];
    }
#ifdef AOT_UNITS_CHAIN
    // The chain: UnitApp derives from UnitMid, and UnitMid derives from UnitBase. Each class is in
    // its own unit. The importers include the interface headers of their imports.
    if (!names.empty() && names.back() == "aot_unit_app_parent_chain")
        app = &hot::aot_table.scripts[names.size() - 1];
    EXPECT_EQ(names, (std::vector<std::string>{"aot_unit_base", "aot_unit_mid", "aot_unit_app_parent_chain"}));
    ASSERT_NE(app, nullptr);
    ASSERT_EQ(app->import_count, 2u);
    EXPECT_STREQ(app->imports[0].unit, "aot_unit_base");
    EXPECT_EQ(app->imports[0].hash, hot::aot_table.scripts[0].iface_hash);
    EXPECT_STREQ(app->imports[1].unit, "aot_unit_mid");
    EXPECT_EQ(app->imports[1].hash, hot::aot_table.scripts[1].iface_hash);
#else
    EXPECT_EQ(names, (std::vector<std::string>{"aot_unit_base", "aot_unit_app"}));
    ASSERT_NE(app, nullptr);
    ASSERT_EQ(app->import_count, 1u);
    EXPECT_STREQ(app->imports[0].unit, "aot_unit_base");
    EXPECT_EQ(app->imports[0].hash, hot::aot_table.scripts[0].iface_hash);
#endif
}

#ifndef AOT_UNITS_LOADER

TEST(AotUnits, RegistryLinksTheUnits)
{
    hot::AotRegistry reg(hot::aot_table);
    EXPECT_EQ(reg.script_count(), hot::aot_table.count);
    World world;
    Actor* app = reg.create_as<Actor>("UnitApp");
    expect_one_tick(world, app);
#ifndef AOT_EXPECT_OPTIMIZED
    expect_total_property(app);
#endif
    expect_native_thunk(reg.thunk("UnitApp::__native"), app);
}

TEST(AotUnits, RegistryRejectsAStaleImport)
{
    std::vector<hot::AotScript> scripts(hot::aot_table.scripts, hot::aot_table.scripts + hot::aot_table.count);
    scripts[0].iface_hash ^= 1; // the base changed its interface; the importers did not compile again
    const hot::AotTable stale{scripts.data(), scripts.size(), hot::aot_table.slots, hot::aot_table.optimized};
    try
    {
        hot::AotRegistry reg(stale);
        FAIL() << "the registry took a stale import";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("has another interface hash"), std::string::npos) << e.what();
    }
    // The failed registry left nothing behind: a good table works.
    hot::AotRegistry reg(hot::aot_table);
    World world;
    expect_one_tick(world, reg.create_as<Actor>("UnitApp"));
}

#else // AOT_UNITS_LOADER

TEST(AotUnits, LoadAotLinksTheUnits)
{
    hot::Loader loader;
    const hot::ReloadResult r = loader.load_aot(hot::aot_table);
    ASSERT_TRUE(r) << r.error;
    const hot::ClassEntry* e = loader.find("UnitApp");
    ASSERT_NE(e, nullptr);
    World world;
    Actor* app = static_cast<Actor*>(e->create().native);
    expect_one_tick(world, app);
#ifndef AOT_EXPECT_OPTIMIZED
    expect_total_property(app);
#endif
    expect_native_thunk(loader.thunk("UnitApp::__native"), app);
}

TEST(AotUnits, LoadAotRejectsAStaleImport)
{
    std::vector<hot::AotScript> scripts(hot::aot_table.scripts, hot::aot_table.scripts + hot::aot_table.count);
    scripts[0].iface_hash ^= 1;
    const hot::AotTable stale{scripts.data(), scripts.size(), hot::aot_table.slots, hot::aot_table.optimized};
    hot::Loader loader;
    const hot::ReloadResult r = loader.load_aot(stale);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.reason, hot::ReloadReason::ImportMismatch);
    // The first importer of the base fails: "aot_unit_app", or "aot_unit_mid" in the chain.
    EXPECT_EQ(r.units, (std::vector<std::string>{hot::aot_table.scripts[1].name}));
    EXPECT_EQ(loader.find("UnitBase"), nullptr); // nothing is loaded
}

TEST(AotUnits, LoadAotRejectsAMissingImport)
{
    // Only the importer: the base unit is not in the table.
    const hot::AotTable partial{hot::aot_table.scripts + 1, 1, hot::aot_table.slots, hot::aot_table.optimized};
    hot::Loader loader;
    const hot::ReloadResult r = loader.load_aot(partial);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.reason, hot::ReloadReason::ImportMismatch);
    EXPECT_NE(r.error.find("is not in the AOT table"), std::string::npos) << r.error;
}

#ifdef AOT_UNITS_CHAIN
// A revert of the base unit keeps the chain: the children in the other units link to the new
// record of UnitBase, and "base.Score()" and "base.Tick()" reach it.
TEST(AotUnits, RevertOfTheBaseUnitKeepsTheChain)
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    backend->seed_slots(hot::aot_table.slots);
    hot::Loader loader;
    const hot::ReloadResult r = loader.load_aot(hot::aot_table);
    ASSERT_TRUE(r) << r.error;
    const hot::ClassEntry* e = loader.find("UnitApp");
    ASSERT_NE(e, nullptr);
    World world;
    Actor* app = static_cast<Actor*>(e->create().native);
    expect_one_tick(world, app);
    const hot::ReloadResult rv = loader.revert_unit("aot_unit_base", *backend);
    ASSERT_TRUE(rv) << rv.error;
    const hot::ReloadResult done = loader.commit_pending();
    ASSERT_TRUE(done) << done.error;
    loader.frame_boundary();
    world.Tick(0.5f);
    EXPECT_FLOAT_EQ(app->Position.x, kScore + 1.0f); // hp is 12 after the second tick
}

// A body change of the base unit: the constructor of the parent and the inherited slot Tick of the
// child use the new bodies. A revert gives the AOT bodies again, also to the object of the override.
TEST(AotUnits, OverrideAndRevertOfTheBaseUnitReachTheChild)
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    backend->seed_slots(hot::aot_table.slots);
    hot::Loader loader;
    const hot::ReloadResult r = loader.load_aot(hot::aot_table);
    ASSERT_TRUE(r) << r.error;
    hot::LoadResult built = backend->load(test_support::source("aot_units/aot_unit_base_v2.hot"), 2);
    ASSERT_TRUE(built) << built.error;
    built.module->unit = "aot_unit_base";
    const hot::ReloadResult req = loader.request_reload(std::move(*built.module));
    ASSERT_TRUE(req) << req.error;
    const hot::ReloadResult done = loader.commit_pending();
    ASSERT_TRUE(done) << done.error;
    loader.frame_boundary();
    const hot::ClassEntry* e = loader.find("UnitApp");
    ASSERT_NE(e, nullptr);
    World world;
    Actor* app = static_cast<Actor*>(e->create().native);
    world.Add(app);
    world.Tick(0.5f);
    EXPECT_FLOAT_EQ(app->Position.x, kScore + 41.0f); // the constructor sets hp to 50, Tick adds 2
    EXPECT_FLOAT_EQ(app->Position.y, kScale);
    const hot::ReloadResult rv = loader.revert_unit("aot_unit_base", *backend);
    ASSERT_TRUE(rv) << rv.error;
    const hot::ReloadResult back = loader.commit_pending();
    ASSERT_TRUE(back) << back.error;
    loader.frame_boundary();
    Actor* fresh = static_cast<Actor*>(e->create().native);
    world.Add(fresh);
    world.Tick(0.5f);
    EXPECT_FLOAT_EQ(app->Position.x, kScore + 42.0f); // hp is 53: Tick adds 1 again
    EXPECT_FLOAT_EQ(fresh->Position.x, kScore);
    // Each Tick deletes a UnitApp: the destructor of v2 adds 200 to base_count, of v1 adds 100. The Tick
    // adds 1, and z is 1640 + base_count. So the two z values are 1640 + 205 and 1640 + 306.
    EXPECT_FLOAT_EQ(app->Position.z + fresh->Position.z, 2 * 1640.0f + 205.0f + 306.0f);
}
#endif

#endif // AOT_UNITS_LOADER

} // namespace
