// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The core module (design 4.9, M8). The core holds the class records, the factories, the
// thunks and the vtables. So every retired generation can unload, generation 1 too.
//
// The stress tests load copies of the library stress_generation (tests/integration/fixtures) as
// generation 1, 2, 3 and so on. They count the loaded modules in the loader stats and in
// /proc/self/maps. The backend tests use the backend that HOT_BACKEND selected.
//
// CMake defines STRESS_SO and HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

namespace
{

namespace fs = std::filesystem;
using hot::GenerationInfo;
using hot::GenState;
using test_support::kDt;
using test_support::Session;
using test_support::source;

const GenerationInfo* find_gen(const hot::LoaderStats& st, uint32_t generation)
{
    for (const GenerationInfo& g : st.generations)
        if (g.generation == generation)
            return &g;
    return nullptr;
}

// The core modules and the generations that are not unloaded.
size_t loaded_modules(const hot::LoaderStats& st)
{
    size_t n = st.core_modules;
    for (const GenerationInfo& g : st.generations)
        n += g.state != GenState::Unloaded;
    return n;
}

// The count of different mapped files whose path has "needle".
size_t mapped_files(const std::string& needle)
{
    std::ifstream maps("/proc/self/maps");
    std::set<std::string> paths;
    for (std::string line; std::getline(maps, line);)
    {
        const size_t slash = line.find('/');
        if (slash != std::string::npos && line.find(needle) != std::string::npos)
            paths.insert(line.substr(slash));
    }
    return paths.size();
}

int* int_field(const hot::Instance& o, const char* name)
{
    return hot::field_ptr<int>(o.shell, name);
}

// Reloads the stress library "reloads" times. Objects of many generations stay alive, some
// objects die, and some generations have a pin for three reloads. The count of loaded modules
// must stay bounded, and each object must keep its state.
void run_reloads(int reloads)
{
    const fs::path dir = fs::temp_directory_path() / ("hot-stress-" + std::to_string(getpid()));
    fs::create_directories(dir);
    int copies = 0;
    auto load_copy = [&]()
    {
        const fs::path p = dir / ("stress." + std::to_string(++copies) + ".so");
        fs::copy_file(STRESS_SO, p, fs::copy_options::overwrite_existing);
        hot::LoadResult r = hot::module_from_library(p.string());
        if (!r)
            throw std::runtime_error(r.error);
        return std::make_pair(std::move(*r.module), p);
    };
    using PokeFn = void (*)(Actor*, int);

    Session s;
    auto [m1, p1] = load_copy();
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(m1), "Pulse"));
    fs::remove(p1); // the mapping stays; the core has its own copy
    s.commit_at_frame_boundary();
    const uint32_t first = hot::active_generation;
    std::deque<hot::Instance> objs; // objects outside the world; the test deletes them
    std::deque<std::pair<uint32_t, int>> pins; // pinned generation, reload of the unpin
    int pokes = 0;
    size_t max_loaded = 0;
    size_t max_mapped = 0;

    testing::internal::CaptureStderr(); // one commit line and one unload line for each reload
    for (int i = 1; i <= reloads; ++i)
    {
        if (i % 7 == 0)
        {
            hot::pin(hot::active_generation);
            pins.emplace_back(hot::active_generation, i + 3);
        }
        auto [m, p] = load_copy();
        std::string error;
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(m)));
        fs::remove(p);
        s.world.Tick(kDt); // the commit runs at the frame boundary
        ASSERT_FALSE(s.commit_failed);
        ASSERT_EQ(hot::active_generation, first + static_cast<uint32_t>(i));
        while (!pins.empty() && pins.front().second <= i)
        {
            hot::unpin(pins.front().first);
            pins.pop_front();
        }
        reinterpret_cast<PokeFn>(s.loader.thunk("Pulse::poke"))(s.actor, 1);
        ++pokes;
        if (i % 10 == 0)
            objs.push_back(s.loader.find("Pulse")->create());
        if (i % 25 == 0 && objs.size() > 2)
        {
            delete static_cast<Actor*>(objs.front().native);
            objs.pop_front();
        }
        for (const hot::Instance& o : objs)
            static_cast<Actor*>(o.native)->Tick(kDt);
        s.world.Tick(kDt); // the retired generation unloads at this boundary
        const hot::LoaderStats st = s.loader.stats();
        max_loaded = std::max(max_loaded, loaded_modules(st));
        max_mapped = std::max(max_mapped, mapped_files(dir.string()) + mapped_files("hot-core-"));
        // The core, the active generation and the pinned generations stay.
        ASSERT_LE(loaded_modules(st), 2 + pins.size()) << "reload " << i << "\n" << s.loader.stats_text();
        ASSERT_EQ(st.core_modules, 1u);
    }
    testing::internal::GetCapturedStderr();
    while (!pins.empty())
    {
        hot::unpin(pins.front().first);
        pins.pop_front();
    }
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    const hot::LoaderStats st = s.loader.stats();
    EXPECT_EQ(loaded_modules(st), 2u) << s.loader.stats_text();
    EXPECT_EQ(find_gen(st, first)->state, GenState::Unloaded);
    EXPECT_LE(max_loaded, 2u + 1u); // at most one pin is open at a time
    EXPECT_LE(max_mapped, max_loaded);
    EXPECT_EQ(mapped_files(dir.string()) + mapped_files("hot-core-"), 2u);
    EXPECT_EQ(st.live_objects, 1 + objs.size());

    // The object of generation 1 kept its state over all reloads.
    EXPECT_EQ(*int_field(s.inst, "hits"), pokes);
    EXPECT_EQ(*int_field(s.inst, "ticks"), 2 * reloads + 2);
    for (const hot::Instance& o : objs)
    {
        const int t = *int_field(o, "ticks");
        static_cast<Actor*>(o.native)->Tick(kDt);
        EXPECT_EQ(*int_field(o, "ticks"), t + 1);
        delete static_cast<Actor*>(o.native);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int reload_count(int fallback)
{
    const char* v = std::getenv("HOT_STRESS_RELOADS");
    return v && std::atoi(v) > 0 ? std::atoi(v) : fallback;
}

// The fast variant of the stress test is in the normal suite.
TEST(CoreModule, ManyReloadsKeepTheModuleCountBounded)
{
    run_reloads(reload_count(200));
}

// The full variant (design M8: 1000 reloads). Set HOT_STRESS=1 to run it.
TEST(CoreModule, ThousandReloads)
{
    if (!std::getenv("HOT_STRESS"))
        GTEST_SKIP() << "set HOT_STRESS=1 to run 1000 reloads";
    run_reloads(1000);
}

// A removed class (design 6.1). Its record is in the core. Its live object uses the layout of
// the last generation that had the class, so that generation stays loaded. When the object
// is gone, the generation unloads and the factory makes an object with an empty state.
TEST_P(BackendTests, RemovedClassHoldsOnlyTheGenerationOfItsLayout)
{
    hot::LoadResult v1 = backend->load(source("removed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("removed_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v3 = backend->load(source("removed_v2.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    hot::Instance ghost = s.loader.find("Ghost")->create();
    static_cast<Actor*>(ghost.native)->Tick(kDt);
    ASSERT_EQ(*int_field(ghost, "haunts"), 1);

    std::string error;
    testing::internal::CaptureStderr();
    for (hot::LoadResult* r : {&v2, &v3})
    {
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*r->module)));
        s.world.Tick(kDt);
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
    }
    s.world.Tick(kDt);
    hot::LoaderStats st = s.loader.stats();
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired); // the layout of Ghost
    EXPECT_EQ(find_gen(st, 2)->state, GenState::Unloaded);
    EXPECT_EQ(st.core_modules, 1u);
    // The Ghost methods go to the handler. The state stays.
    static_cast<Actor*>(ghost.native)->Tick(kDt);
    EXPECT_EQ(*int_field(ghost, "haunts"), 1);

    delete static_cast<Actor*>(ghost.native);
    s.world.Tick(kDt);
    st = s.loader.stats();
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Unloaded);
    // The factory of the removed class still works. The object has an empty state.
    hot::Instance late = s.loader.find("Ghost")->create();
    static_cast<Actor*>(late.native)->Tick(kDt);
    EXPECT_EQ(int_field(late, "haunts"), nullptr);
    delete static_cast<Actor*>(late.native);
    testing::internal::GetCapturedStderr();
    // The Hero object runs generation 3.
    const int t = *int_field(s.inst, "ticks");
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s.inst, "ticks"), t + 100);
}

// A std::shared_ptr field: its control block has the vtable of the generation that made it.
// So no retired generation unloads while the object lives (kMayHoldCode).
TEST_P(BackendTests, CodeHoldingFieldBlocksTheUnload)
{
    hot::LoadResult v1 = backend->load(source("handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("handle_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    std::string error;
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    for (int i = 0; i < 4; ++i)
        s.world.Tick(kDt);
    testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);
    const hot::LoaderStats st = s.loader.stats();
    EXPECT_TRUE(st.state_holds_code);
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired);
    EXPECT_NE(s.loader.stats_text().find("unload blocked"), std::string::npos);
}

} // namespace
