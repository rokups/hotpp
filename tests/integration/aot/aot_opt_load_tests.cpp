// Optimized AOT scripts in a Loader (MODE AOT OPTIMIZED). This program links the loader, a
// backend and an optimized AOT library. The table has the flag "optimized". The loader accepts it,
// and then it refuses each override and each reload.
#include "Actor.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

void expect_locked(const hot::ReloadResult& r)
{
    EXPECT_EQ(r.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(r.reason, hot::ReloadReason::BadModule);
    EXPECT_NE(r.error.find("the loader holds optimized AOT code (MODE AOT OPTIMIZED)"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("Build the scripts without OPTIMIZED"), std::string::npos) << r.error;
}

TEST(AotOptLoad, TableHasTheOptimizedFlag)
{
    EXPECT_TRUE(hot::aot_table.optimized);
    EXPECT_EQ(hot::aot_table.count, 2u);
}

TEST(AotOptLoad, LoadAotAcceptsTheTableAndTheClassesWork)
{
    World world; // the world owns the actor and deletes it before the loader unloads
    hot::Loader loader;
    const hot::ReloadResult loaded = loader.load_aot(hot::aot_table);
    ASSERT_TRUE(loaded) << loaded.error;
    for (const char* name : {"Fighter", "Veteran", "Spawner", "Hero", "Helper"})
        EXPECT_NE(loader.find(name), nullptr) << name;
    EXPECT_NE(loader.thunk("Hero::Weight"), nullptr);
    hot::Instance inst = loader.find("Fighter")->create();
    ASSERT_NE(inst.native, nullptr);
    Actor* fighter = static_cast<Actor*>(inst.native);
    world.Add(fighter);
    world.Tick(0.5f);
    world.Tick(0.5f);
    EXPECT_FLOAT_EQ(fighter->Position.x, 1.0f);
}

TEST(AotOptLoad, LoaderRefusesReloadRevertAndLoad)
{
    hot::Loader loader;
    ASSERT_TRUE(loader.load_aot(hot::aot_table));
    hot::Module module;
    module.register_fn = hot::aot_table.scripts[0].register_fn;
    module.origin = "test";
    module.unit = hot::aot_table.scripts[0].name;
    expect_locked(loader.request_reload(std::move(module)));
    expect_locked(loader.request_reload(hot::LoadResult{}));
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    expect_locked(loader.revert_unit(hot::aot_table.scripts[0].name, *backend));
    EXPECT_FALSE(loader.reload_pending());
    // The loader still works, and the generation does not change.
    EXPECT_EQ(loader.find("Hero")->record->generation, 1u);
    // A second load fails: the loader has a generation.
    EXPECT_FALSE(loader.load_aot(hot::aot_table));
}

TEST(AotOptLoad, ResetUnlocksTheLoader)
{
    hot::Loader loader;
    ASSERT_TRUE(loader.load_aot(hot::aot_table));
    std::string error;
    ASSERT_TRUE(loader.reset(error)) << error;
    EXPECT_EQ(loader.find("Hero"), nullptr);
    ASSERT_TRUE(loader.load_aot(hot::aot_table));
    EXPECT_NE(loader.find("Hero"), nullptr);
}

} // namespace
