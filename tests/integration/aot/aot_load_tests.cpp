// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// AOT scripts in a Loader (big task 14, steps 3 and 4). This program links the loader, a
// backend and the AOT scripts (the library links only the core of the runtime). It checks
// three things.
//  - AotTable::slots has the slot manifest of all scripts.
//  - A backend that Backend::seed_slots seeded continues the slot numbers of the AOT code.
//  - Loader::load_aot registers each script as a unit, and the unit reloads alone.
#include "Actor.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/runtime.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#ifdef _WIN32
    #include <process.h>
    #define getpid _getpid // the MSVC CRT has the POSIX name only as a deprecated name (C4996)
#else
    #include <unistd.h>
#endif

namespace
{

// The script module_v1 with one more method before "Weight". A generation 2 that has no
// seeded slot state gives the new method the slot of "Weight".
hot::ScriptSource module_with_extra_method()
{
    hot::ScriptSource src = test_support::source("module_v1.hot");
    const std::string anchor = "    int Weight(Mode m) {";
    const size_t at = src.text.find(anchor);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos)
        src.text.insert(at, "    int Extra() { return 7; }\n\n");
    return src;
}

TEST(AotLoad, TableHasTheSlotManifestOfAllScripts)
{
    ASSERT_NE(hot::aot_table.slots, nullptr);
    const std::string slots = hot::aot_table.slots;
    // One line for each fn method of both scripts, and the signature lines.
    EXPECT_NE(slots.find("Hero::Weight 1\n"), std::string::npos) << slots;
    EXPECT_NE(slots.find("Fighter::Power 1\n"), std::string::npos) << slots;
    EXPECT_NE(slots.find("sig Hero::Weight "), std::string::npos) << slots;
}

// The thunk "Hero::Weight" of the AOT code keeps its slot number. After a reload of the script
// the thunk must still reach "Weight". Without the seed it reaches the new method "Extra".
int weight_after_reload_with(bool seed)
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    if (seed)
        backend->seed_slots(hot::aot_table.slots);
    hot::LoadResult v2 = backend->load(module_with_extra_method(), 2);
    EXPECT_TRUE(v2) << v2.error;
    if (!v2)
        return -1;
    v2.module->unit = "module_v1";

    int result = -1;
    {
        hot::Loader loader;
        const hot::ReloadResult loaded = loader.load_aot(hot::aot_table);
        EXPECT_TRUE(loaded) << loaded.error;
        const hot::ClassEntry* hero = loader.find("Hero");
        EXPECT_NE(hero, nullptr);
        void* thunk = loader.thunk("Hero::Weight");
        EXPECT_NE(thunk, nullptr);
        if (!loaded || !hero || !thunk)
            return -1;
        hot::Instance inst = hero->create();
        const hot::ReloadResult req = loader.request_reload(std::move(*v2.module));
        EXPECT_TRUE(req) << req.error;
        const hot::ReloadResult done = loader.commit_pending();
        EXPECT_TRUE(done) << done.error;
        // Mode::Run is 2 and gives 10.
        result = reinterpret_cast<int32_t (*)(Actor*, int32_t)>(thunk)(static_cast<Actor*>(inst.native), 2);
        delete static_cast<Actor*>(inst.native);
    }
    return result;
}

TEST(AotLoad, SeededBackendKeepsTheSlotNumbersOfUnchangedMethods)
{
    EXPECT_EQ(weight_after_reload_with(true), 10);
}

TEST(AotLoad, UnseededBackendRenumbersTheSlots)
{
    // The control of the test above: it proves that the seed has an effect.
    EXPECT_NE(weight_after_reload_with(false), 10);
}

// The seed rule: the text must cover the state that the backend has.
TEST(AotLoad, SlotsCoverRule)
{
    EXPECT_TRUE(hot::slots_cover("A::f 1\nA::g 2\n", ""));
    EXPECT_TRUE(hot::slots_cover("A::f 1\nA::g 2\n", "A::g 2\n"));
    EXPECT_FALSE(hot::slots_cover("A::f 1\n", "A::f 1\nA::g 2\n")); // the backend has a newer method
}

std::string read_all(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A slots file of an earlier run (a stale file in the default work dir) has a line that the AOT
// text lacks. The seed must replace it. A file with every line of the text and more is newer state.
TEST(AotLoad, SeedReplacesAStaleSlotsFileAndKeepsANewerOne)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("hot_seed_stale_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string aot = hot::aot_table.slots;
    {
        hot::ClangFilesOptions opt;
        opt.work_dir = dir.string();
        opt.slots_file = (dir / "slots.txt").string();
        hot::ClangFilesBackend backend(opt);

        std::ofstream(opt.slots_file) << "Old::gone 4\n"; // stale: no line of the AOT text
        backend.seed_slots(aot);
        EXPECT_EQ(read_all(opt.slots_file), aot);

        std::ofstream(opt.slots_file) << "Hero::Weight 1\nHero::Weight2 9\n"; // stale: part of the text and a foreign line
        backend.seed_slots(aot);
        EXPECT_EQ(read_all(opt.slots_file), aot);

        const std::string newer = aot + "Hero::Extra 7\n"; // every line of the text and one more
        std::ofstream(opt.slots_file) << newer;
        backend.seed_slots(aot);
        EXPECT_EQ(read_all(opt.slots_file), newer);
    }
    fs::remove_all(dir);
}

TEST(AotLoad, LoadAotRegistersEachScriptAsAUnit)
{
    hot::Loader loader;
    ASSERT_TRUE(loader.load_aot(hot::aot_table));
    for (const char* name : {"Fighter", "Veteran", "Spawner", "Hero", "Helper"})
        EXPECT_NE(loader.find(name), nullptr) << name;
    EXPECT_EQ(hot::active_generation, 1u);
    ASSERT_NE(loader.aot_record("Hero"), nullptr);
    EXPECT_EQ(loader.aot_record("Hero")->generation, 1u);
    EXPECT_EQ(loader.aot_record("Nobody"), nullptr);
    // A second load needs a reset.
    EXPECT_FALSE(loader.load_aot(hot::aot_table));
}

} // namespace
