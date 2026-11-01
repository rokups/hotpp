// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of the Loader units (Module::unit) with hand-written records and no backend.
// A reload of a named unit keeps the classes and the module block of the other units.
#include "hot/runtime.h"
#include "support/reload_assert.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct Fake : hot::ShellBase
{
    virtual ~Fake() { hot::destroy_state(this); }
};
struct Other : hot::ShellBase
{
    virtual ~Other() { hot::destroy_state(this); }
};

struct State
{
    int value;
};
hot::FieldInfo fields[] = {
    {"value", nullptr, hot::type_key<int>(), offsetof(State, value), sizeof(int), alignof(int),
        hot::field_flags<int>(), [](void*, void* f) { new (f) int(1); }, hot::move_field<int>,
        hot::field_destroyer<int>()},
};
hot::StateLayout layout{"State", sizeof(State), alignof(State), 1, fields};

// Each unit has a module block with its own slot variable, as the generated code has.
hot::StateLayout mlayout{"Mod", sizeof(State), alignof(State), 1, fields};
void* slot_a1 = nullptr;
void* slot_a3 = nullptr;
void* slot_b2 = nullptr;
// The live flag of each module, as the generated code has (Registry::set_live_flag).
uint32_t live_a1 = 0;
uint32_t live_a3 = 0;
uint32_t live_b2 = 0;

hot::SlotFn slots[1] = {nullptr};
constexpr uint64_t kHash = hot::shell_layout_hash("Base", sizeof(Fake), alignof(Fake), 0);

hot::ClassRecord fake1, fake2, fake3, other1, other2;

void reset_records()
{
    fake1 = hot::ClassRecord{"Fake", 1, slots, 1, &layout, nullptr, "Base", kHash};
    fake2 = hot::ClassRecord{"Fake", 2, slots, 1, &layout, nullptr, "Base", kHash};
    fake3 = hot::ClassRecord{"Fake", 3, slots, 1, &layout, nullptr, "Base", kHash};
    other1 = hot::ClassRecord{"Other", 1, slots, 1, &layout, nullptr, "Base", kHash};
    other2 = hot::ClassRecord{"Other", 2, slots, 1, &layout, nullptr, "Base", kHash};
    slot_a1 = slot_a3 = slot_b2 = nullptr;
    live_a1 = live_a3 = live_b2 = 0;
}

template <class T> hot::Instance make(hot::ClassRecord* rec)
{
    auto* s = new T;
    hot::init_state(s, rec);
    return {s, s};
}
hot::Instance create_fake1() { return make<Fake>(&fake1); }
hot::Instance create_fake2() { return make<Fake>(&fake2); }
hot::Instance create_fake3() { return make<Fake>(&fake3); }
hot::Instance create_other1() { return make<Other>(&other1); }
hot::Instance create_other2() { return make<Other>(&other2); }

// Unit A, generation 1: the class Fake and a module block.
void register_a1(hot::Registry& reg)
{
    reg.set_live_flag(&live_a1);
    reg.add(&fake1, &create_fake1);
    reg.set_module_state(&mlayout, &slot_a1);
}
// Unit B, generation 2: the class Other and a module block.
void register_b2(hot::Registry& reg)
{
    reg.set_live_flag(&live_b2);
    reg.add(&other2, &create_other2);
    reg.set_module_state(&mlayout, &slot_b2);
}
// Two AOT scripts: both are generation 1.
void register_x1(hot::Registry& reg)
{
    reg.add(&fake1, &create_fake1);
}
void register_y1(hot::Registry& reg)
{
    reg.add(&other1, &create_other1);
}
const hot::AotScript kAot[] = {{"X", &register_x1}, {"Y", &register_y1}};
const hot::AotTable kAotTable = {kAot, 2};

// Unit A, generation 3: the class Fake only.
void register_a3(hot::Registry& reg)
{
    reg.set_live_flag(&live_a3);
    reg.add(&fake3, &create_fake3);
    reg.set_module_state(&mlayout, &slot_a3);
}
// Unit B in a wrong form: the class Fake of unit A, generation 2.
void register_b2_fake(hot::Registry& reg)
{
    reg.add(&fake2, &create_fake2);
}
// Without a unit: both classes, generation 1 and 2.
void register_both1(hot::Registry& reg)
{
    reg.add(&fake1, &create_fake1);
    reg.add(&other2, &create_other2);
}
// Without a unit: the class Fake only, generation 3.
void register_fake3(hot::Registry& reg)
{
    reg.add(&fake3, &create_fake3);
}

hot::Module module_of(hot::Module::RegisterFn fn, const char* name, const char* unit)
{
    hot::Module m;
    m.register_fn = fn;
    m.origin = name;
    m.unit = unit;
    return m;
}

bool is_removed(const hot::Loader& loader, const std::string& name)
{
    for (const hot::ClassInfo& c : loader.stats().classes)
        if (c.name == name)
            return c.removed;
    return false;
}

struct Units : ::testing::Test
{
    Units() { reset_records(); }
    hot::Loader loader;
};

} // namespace

TEST_F(Units, ReloadOfOneUnitKeepsTheClassAndTheBlockOfTheOtherUnit)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_a1, "A1", "A")));
    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_b2, "B2", "B")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(loader.commit_pending());
    testing::internal::GetCapturedStderr();
    // The load of unit B did not remove the class of unit A.
    EXPECT_FALSE(is_removed(loader, "Fake"));
    ASSERT_NE(slot_a1, nullptr);
    ASSERT_NE(slot_b2, nullptr);
    EXPECT_EQ(live_a1, 1u);
    EXPECT_EQ(live_b2, 1u);

    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_a3, "A3", "A")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(loader.commit_pending());
    testing::internal::GetCapturedStderr();
    EXPECT_FALSE(is_removed(loader, "Other"));
    EXPECT_FALSE(is_removed(loader, "Fake"));
    ASSERT_NE(loader.find("Other"), nullptr);
    hot::Instance o = loader.find("Other")->create();
    ASSERT_NE(o.shell, nullptr);
    delete static_cast<Other*>(o.shell);
    // The block of unit A migrated. The block of unit B is untouched.
    EXPECT_EQ(slot_a1, nullptr);
    ASSERT_NE(slot_a3, nullptr);
    EXPECT_NE(slot_b2, nullptr);
    EXPECT_EQ(loader.module_state("A").slot, &slot_a3);
    EXPECT_EQ(loader.module_state("B").slot, &slot_b2);
    EXPECT_EQ(loader.module_state().layout, nullptr);
    // Only the module of unit A went stale.
    EXPECT_EQ(live_a1, 0u);
    EXPECT_EQ(live_a3, 1u);
    EXPECT_EQ(live_b2, 1u);
}

TEST_F(Units, ReloadOfAUnitRemovesTheMissingClassOfThisUnit)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_a1, "A1", "A")));
    // Unit A at generation 3 has no class Fake: the class of unit A goes.
    auto empty = [](hot::Registry&) {};
    ASSERT_RELOAD_OK(loader.request_reload(module_of(empty, "A3", "A")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(loader.commit_pending());
    testing::internal::GetCapturedStderr();
    EXPECT_TRUE(is_removed(loader, "Fake"));
    EXPECT_EQ(slot_a1, nullptr);
    EXPECT_EQ(loader.module_state("A").layout, nullptr);
}

TEST_F(Units, ReloadWithTheEmptyUnitRemovesEveryMissingClass)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_both1, "gen1", "")));
    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_fake3, "gen3", "")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(loader.commit_pending());
    testing::internal::GetCapturedStderr();
    EXPECT_FALSE(is_removed(loader, "Fake"));
    EXPECT_TRUE(is_removed(loader, "Other"));
}

TEST_F(Units, AClassThatMovesToAnotherUnitMakesTheModuleBad)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_a1, "A1", "A")));
    const hot::ReloadResult r = loader.request_reload(module_of(register_b2_fake, "B2", "B"));
    EXPECT_EQ(r.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(r.reason, hot::ReloadReason::BadModule);
    EXPECT_NE(r.error.find("unit"), std::string::npos) << r.error;
    EXPECT_FALSE(loader.reload_pending());
    EXPECT_EQ(loader.stats().active_generation, 1u);
    // A move from a named unit to the empty unit is the same change.
    const hot::ReloadResult r2 = loader.request_reload(module_of(register_b2_fake, "X2", ""));
    EXPECT_EQ(r2.reason, hot::ReloadReason::BadModule);
}

// The units of load_aot share generation 1. The count of the live objects of a module is
// by unit and generation: an object of unit Y does not hold the retired module of unit X.
TEST_F(Units, UnitsThatShareGenerationOneCountTheirObjectsApart)
{
    ASSERT_RELOAD_OK(loader.load_aot(kAotTable));
    hot::Instance y = loader.find("Other")->create();
    auto info_of = [&](const char* origin)
    {
        for (const hot::GenerationInfo& g : loader.stats().generations)
            if (g.origin == origin)
                return g;
        ADD_FAILURE() << origin;
        return hot::GenerationInfo{};
    };
    EXPECT_EQ(info_of("aot:Y").code_refs, 2u); // the entry of the class and the object
    EXPECT_EQ(info_of("aot:X").code_refs, 1u); // the entry of the class
    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_a3, "X3", "X")));
    ASSERT_RELOAD_OK(loader.commit_pending());
    loader.frame_boundary();
    loader.frame_boundary();
    EXPECT_EQ(info_of("aot:Y").code_refs, 2u);
    EXPECT_EQ(info_of("aot:X").state, hot::GenState::Retired);
    EXPECT_EQ(info_of("aot:Y").state, hot::GenState::Active);
    delete static_cast<Other*>(y.native);
}

TEST_F(Units, LoadAotKeepsACopyOfTheRecordAndFailsForTwoScriptsWithOneClass)
{
    ASSERT_RELOAD_OK(loader.load_aot(kAotTable));
    ASSERT_NE(loader.aot_record("Fake"), nullptr);
    EXPECT_EQ(loader.aot_record("Fake")->generation, 1u);
    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_a3, "X3", "X")));
    ASSERT_RELOAD_OK(loader.commit_pending());
    EXPECT_EQ(loader.find("Fake")->record->generation, 3u);
    EXPECT_EQ(loader.aot_record("Fake")->generation, 1u); // the copy does not change
    std::string error;
    ASSERT_TRUE(loader.reset(error)) << error;
    // reset puts the saved record back
    EXPECT_EQ(fake1.generation, 1u);
    const hot::AotScript twice[] = {{"X", &register_x1}, {"X2", &register_x1}};
    const hot::ReloadResult r = loader.load_aot({twice, 2});
    EXPECT_EQ(r.reason, hot::ReloadReason::BadModule);
    EXPECT_NE(r.error.find("two units"), std::string::npos) << r.error;
    EXPECT_EQ(loader.find("Fake"), nullptr); // the loader is empty again
}

// An optimized AOT table (AotTable::optimized) locks the loader: find and create work, but the
// loader refuses request_reload and load_unit (through load). Reset is refused too, because the
// loader does not track the objects of optimized code.
TEST_F(Units, OptimizedAotTableLocksTheLoader)
{
    const hot::AotTable table = {kAot, 2, nullptr, true};
    ASSERT_RELOAD_OK(loader.load_aot(table));
    ASSERT_NE(loader.find("Fake"), nullptr);
    hot::Instance f = loader.find("Fake")->create();
    EXPECT_NE(f.native, nullptr);
    delete static_cast<Fake*>(f.native);
    for (const hot::ReloadResult& r : {loader.request_reload(module_of(register_a3, "X3", "X")),
             loader.request_reload(hot::LoadResult{})})
    {
        EXPECT_EQ(r.status, hot::ReloadStatus::Failed);
        EXPECT_EQ(r.reason, hot::ReloadReason::BadModule);
        EXPECT_NE(r.error.find("optimized AOT code"), std::string::npos) << r.error;
        EXPECT_NE(r.error.find("Build the scripts without OPTIMIZED"), std::string::npos) << r.error;
    }
    EXPECT_EQ(loader.find("Fake")->record->generation, 1u);
    std::string error;
    EXPECT_FALSE(loader.reset(error));
    EXPECT_EQ(error,
        "the loader holds optimized AOT code (MODE AOT OPTIMIZED): objects are not tracked, "
        "so it cannot reset. Exit the program instead");
    EXPECT_NE(loader.find("Fake"), nullptr); // the loader keeps its state
}

// The host and the runtime must agree on HOT_THREAD_SAFE (config.h). The test gives the opposite
// value by hand, because one build has one value.
TEST(ThreadSafeBuild, AHostWithAnotherValueThrows)
{
    EXPECT_NO_THROW(hot::check_thread_safe_build(HOT_THREAD_SAFE != 0));
    try
    {
        hot::check_thread_safe_build(!HOT_THREAD_SAFE);
        FAIL() << "no exception";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("HOT_THREAD_SAFE mismatch"), std::string::npos) << e.what();
    }
}

namespace
{
// A class whose state block has a std::shared_ptr. The field has kMayHoldCode, so a live object
// makes Loader::state_holds_code true and blocks each unload.
struct Holder : hot::ShellBase
{
    virtual ~Holder() { hot::destroy_state(this); }
};
struct HolderState
{
    std::shared_ptr<int> p;
};
hot::FieldInfo holder_fields[] = {
    {"p", nullptr, hot::type_key<std::shared_ptr<int>>(), offsetof(HolderState, p), sizeof(std::shared_ptr<int>),
        alignof(std::shared_ptr<int>), hot::field_flags<std::shared_ptr<int>>(),
        [](void*, void* f) { new (f) std::shared_ptr<int>(); }, hot::move_field<std::shared_ptr<int>>,
        hot::field_destroyer<std::shared_ptr<int>>()},
};
hot::StateLayout holder_layout{"HolderState", sizeof(HolderState), alignof(HolderState), 1, holder_fields};
} // namespace

// Workers create and destroy objects in a loop while the main thread calls stats() and
// frame_boundary() after a reload. The walks of the loader take the lock of the list
// (HOT_THREAD_SAFE=1). TSan reports a race or a missing lock here.
#if HOT_THREAD_SAFE
TEST_F(Units, StatsAndFrameBoundaryWhileWorkersCreateAndDestroyObjects)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_a1, "A1", "A")));
    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_a3, "A3", "A")));
    ASSERT_RELOAD_OK(loader.commit_pending());
    hot::ClassRecord holder_rec{"Holder", 1, slots, 1, &holder_layout, nullptr, "Base", kHash};
    auto* keep = new Holder; // one object lives to the end
    hot::init_state(keep, &holder_rec);
    ASSERT_TRUE(loader.stats().state_holds_code);

    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
        workers.emplace_back([&]
        {
            while (!stop.load(std::memory_order_relaxed))
            {
                auto* h = new Holder;
                hot::init_state(h, &holder_rec);
                hot::note_creator(h, 1);
                auto* f = new Fake;
                hot::init_state(f, &fake3);
                delete h;
                delete f;
            }
        });
    size_t max_live = 0;
    for (int i = 0; i < 300; ++i)
    {
        const hot::LoaderStats st = loader.stats();
        max_live = std::max(max_live, st.live_objects);
        EXPECT_TRUE(st.state_holds_code);
        loader.frame_boundary();
    }
    stop = true;
    for (std::thread& w : workers)
        w.join();
    delete keep;
    EXPECT_GE(max_live, 1u);
    EXPECT_EQ(loader.stats().live_objects, 0u);
}
#endif
