// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Runtime polish (S9): the removed class handler (design 6.1), script_stats (8.4), the entry
// counter check at the commit (6.3) and the harness with the file watcher and the commands (8.1).
#include "Harness.h"
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{

using test_support::kDt;
using test_support::read_file;
using test_support::Session;
using test_support::source;

const hot::ClassInfo* find_class(const hot::LoaderStats& st, const std::string& name)
{
    for (const hot::ClassInfo& c : st.classes)
        if (c.name == name)
            return &c;
    return nullptr;
}

// Generation 2 does not have the class Ghost. The Ghost object stays valid. Its virtual
// methods log and do nothing. The Hero object runs the new code.
TEST_P(BackendTests, RemovedClassKeepsItsObjectsValid)
{
    hot::LoadResult v1 = backend->load(source("removed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("removed_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    hot::Instance ghost = s.loader.find("Ghost")->create();
    s.world.Add(static_cast<Actor*>(ghost.native));
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_EQ(*hot::field_ptr<int>(ghost.shell, "haunts"), 2);
    const uint64_t calls0 = hot::removed_class_call_count();

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStderr();
    s.world.Tick(kDt); // the commit happens at the end of this frame
    s.world.Tick(kDt); // the Ghost virtuals now go to the handler
    static_cast<Actor*>(ghost.native)->TakeDamage(1.0f);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);

    EXPECT_NE(log.find("WARNING: class Ghost is removed from the source; 1 live object(s)"), std::string::npos) << log;
    EXPECT_NE(log.find("call of a method of a removed script class or function ignored"), std::string::npos) << log;
    EXPECT_EQ(hot::active_generation, 2u);
    // The Ghost object kept its state. The Tick of the first frame after the commit ran in the
    // old code (the commit is at the end of that frame), so haunts is 3 and not more.
    EXPECT_EQ(*hot::field_ptr<int>(ghost.shell, "haunts"), 3);
    EXPECT_EQ(*hot::field_ptr<float>(ghost.shell, "chill"), 2.5f);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "ticks"), 2 + 1 + 100); // Hero runs the new code
    EXPECT_GE(hot::removed_class_call_count(), calls0 + 2); // one Tick and one TakeDamage

    const hot::LoaderStats st = s.loader.stats();
    const hot::ClassInfo* g = find_class(st, "Ghost");
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->removed);
    EXPECT_EQ(g->live_objects, 1u);
    for (uint32_t i = 0; i < g->slots; ++i)
        EXPECT_TRUE(ghost.shell->cls->slots[i] == reinterpret_cast<hot::SlotFn>(&hot::StateBase::__removed));
    EXPECT_FALSE(find_class(st, "Hero")->removed);
}

// Backend::load runs on a worker thread. The main thread runs the old code during the compile and
// commits the finished module at the frame boundary. The object keeps its state.
TEST_P(BackendTests, LoadOnAWorkerThreadWhileTheOldCodeRuns)
{
    hot::LoadResult v1 = backend->load(source("removed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();

    std::atomic<bool> done{false};
    hot::LoadResult v2;
    std::thread worker([&]
    {
        v2 = backend->load(source("removed_v2.hot"), 2);
        done = true;
    });
    int frames = 0;
    do
    {
        s.world.Tick(kDt); // the old code runs during the compile
        ++frames;
    } while (!done);
    worker.join();
    ASSERT_TRUE(v2) << v2.error;
    EXPECT_EQ(hot::active_generation, 1u);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "ticks"), frames);

    testing::internal::CaptureStderr(); // the commit warns about the removed class Ghost
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    s.world.Tick(kDt);
    testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(*hot::field_ptr<int>(s.inst.shell, "ticks"), frames + 1 + 100); // the new code adds 100
}

// The class comes back in a later generation. The old object gets the new code.
TEST_P(BackendTests, RemovedClassCanReturn)
{
    hot::LoadResult v1 = backend->load(source("removed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("removed_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v3 = backend->load(source("removed_v1.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    hot::Instance ghost = s.loader.find("Ghost")->create();
    s.world.Add(static_cast<Actor*>(ghost.native));
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    ASSERT_TRUE(find_class(s.loader.stats(), "Ghost")->removed);
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v3.module)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_FALSE(find_class(s.loader.stats(), "Ghost")->removed);
    const int before = *hot::field_ptr<int>(ghost.shell, "haunts");
    s.world.Tick(kDt);
    EXPECT_EQ(*hot::field_ptr<int>(ghost.shell, "haunts"), before + 1);
}

// script_stats: generations, pins, live objects and slots (design 8.4).
TEST_P(BackendTests, StatsListGenerationsClassesAndSlots)
{
    hot::LoadResult v1 = backend->load(source("removed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("removed_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    hot::Instance ghost = s.loader.find("Ghost")->create();
    s.world.Add(static_cast<Actor*>(ghost.native));

    hot::LoaderStats st = s.loader.stats();
    EXPECT_EQ(st.live_objects, 2u);
    ASSERT_EQ(st.classes.size(), 2u);
    EXPECT_EQ(st.classes[0].name, "Ghost"); // sorted by name
    EXPECT_EQ(st.classes[1].name, "Hero");
    EXPECT_EQ(st.classes[0].live_objects, 1u);
    EXPECT_GE(st.classes[0].slots, 3u); // the three virtuals of Actor
    EXPECT_EQ(st.slots, st.classes[0].slots + st.classes[1].slots);

    const std::string text = s.loader.stats_text();
    EXPECT_NE(text.find("script_stats: active generation 1, 2 live objects, 2 classes"), std::string::npos) << text;
    EXPECT_NE(text.find("generation 1: active, pins 0"), std::string::npos) << text;
    EXPECT_NE(text.find("class Ghost: generation 1,"), std::string::npos) << text;
    EXPECT_EQ(text.find("REMOVED"), std::string::npos) << text;

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    const std::string after = s.loader.stats_text();
    EXPECT_NE(after.find("active generation 2"), std::string::npos) << after;
    EXPECT_NE(after.find("generation 1: retired"), std::string::npos) << after;
    EXPECT_NE(after.find(", REMOVED"), std::string::npos) << after;
}

#ifndef NDEBUG
// Design 6.3: the entry counters are a debug check. A commit while a thread is in script code
// is a bug of the engine. The runtime counts it and asserts.
TEST(EntryCounters, CommitWithAnOpenScopeAssertsInDebugBuilds)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(GEN1_SO));
    hot::LoadResult v2 = hot::module_from_library(GEN2_SO);
    ASSERT_TRUE(v2) << v2.error;
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    EXPECT_EQ(hot::in_script_tick(), 0);
    EXPECT_DEATH(
        {
            hot::ScriptTickScope scope;
            s.loader.commit_pending();
        },
        "BUG: commit with 1 thread");
    // The parent process did not commit and has no open scope.
    EXPECT_EQ(hot::in_script_tick(), 0);
    EXPECT_TRUE(s.loader.reload_pending());
}
#else
// A release build has no counter: the scope does nothing and the commit has no check.
TEST(EntryCounters, ReleaseBuildHasNoCounter)
{
    static_assert(!hot::kTickChecks);
    hot::ScriptTickScope scope;
    EXPECT_EQ(hot::in_script_tick(), 0);
}
#endif

// A temporary directory with a copy of a script. The test rewrites the script like an editor.
struct TempScript
{
    std::filesystem::path dir, file;
    explicit TempScript(const char* name)
    {
        dir = std::filesystem::temp_directory_path()
            / ("hot_harness_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        file = dir / "watched.hot";
        write(name);
    }
    ~TempScript() { std::filesystem::remove_all(dir); }
    void write(const char* name) const
    {
        std::ofstream f(file, std::ios::trunc);
        f << read_file(std::string(HOT_SOURCE_DIR) + "/tests/scripts/" + name);
    }
};

// The harness reloads when the file changes, and the commands work (design 8.1).
TEST(HarnessTests, FileChangeReloadsAndCommandsWork)
{
    TempScript script("player_v1.hot");
    harness::Harness h(script.file.string());
    std::string error;
    testing::internal::CaptureStdout();
    ASSERT_TRUE(h.start(error)) << error;
    h.frame(kDt, 0);
    h.frame(kDt, 0);
    EXPECT_EQ(hot::active_generation, 1u);
    EXPECT_EQ(h.reloads_started(), 0u); // no change: no reload

    script.write("player_v2.hot");
    h.frame(kDt, 2000); // the watcher sees the change; the commit is at the end of the frame
    EXPECT_EQ(h.reloads_started(), 1u);
    EXPECT_EQ(hot::active_generation, 2u);

    const std::string stats = h.command("stats");
    EXPECT_NE(stats.find("script_stats: active generation 2, 1 live objects"), std::string::npos) << stats;
    EXPECT_NE(h.command("reload").find("reload validated"), std::string::npos); // generation 3
    h.frame(kDt, 0);
    EXPECT_EQ(hot::active_generation, 3u);
    EXPECT_NE(h.command("bogus").find("unknown command"), std::string::npos);
    EXPECT_EQ(h.command(""), "");
    EXPECT_FALSE(h.quit_requested());
    h.command("quit");
    EXPECT_TRUE(h.quit_requested());
    testing::internal::GetCapturedStdout();
}

// A change of the script parent needs a full reload. The harness saves the actors, resets the Loader,
// loads the script as generation 1 and makes the actors again with the saved state.
TEST(HarnessTests, FullReloadKeepsPositionAndName)
{
    TempScript script("fnremoved_v1.hot");
    harness::Harness h(script.file.string(), "Medic");
    std::string error;
    testing::internal::CaptureStdout();
    ASSERT_TRUE(h.start(error)) << error;
    h.world().Actors()[0]->Name = "Doc";
    h.world().Actors()[0]->Position = Vec3(1.5f, 2.0f, -3.0f);
    h.frame(kDt, 0);

    script.write("medic_reparent.hot");
    h.frame(kDt, 2000);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("[host] full reload: "), std::string::npos) << out;
    EXPECT_EQ(h.reloads_started(), 1u);
    EXPECT_EQ(hot::active_generation, 1u);
    ASSERT_EQ(h.world().Actors().size(), 1u);
    EXPECT_EQ(h.world().Actors()[0]->Name, "Doc");
    EXPECT_EQ(h.world().Actors()[0]->Position.x, 1.5f);
    EXPECT_EQ(h.world().Actors()[0]->Position.z, -3.0f);
}

// A script that changes the script parent and has a C++ type error: the Reload build fails, so
// the harness starts no full reload. The actors and the old generation stay as they are. The fixed
// script then gives the full reload.
TEST(HarnessTests, BrokenScriptFailsTheReloadAndKeepsTheActors)
{
    TempScript script("fnremoved_v1.hot");
    harness::Harness h(script.file.string(), "Medic");
    std::string error;
    testing::internal::CaptureStdout();
    ASSERT_TRUE(h.start(error)) << error;
    h.world().Actors()[0]->Name = "Doc";
    h.frame(kDt, 0);

    {
        std::ofstream f(script.file, std::ios::trunc);
        f << read_file(std::string(HOT_SOURCE_DIR) + "/tests/scripts/medic_reparent.hot");
        f << "\nclass Broken : Actor {\n    void f() { int x = \"text\"; }\n}\n";
    }
    testing::internal::CaptureStderr();
    const hot::ReloadResult result = h.reload();
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result.status, hot::ReloadStatus::Failed) << result.error << log;
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(hot::active_generation, 1u);
    ASSERT_EQ(h.world().Actors().size(), 1u);
    EXPECT_EQ(h.world().Actors()[0]->Name, "Doc");

    // The fixed script applies the full reload. The actor comes back.
    script.write("medic_reparent.hot");
    EXPECT_EQ(h.reload().status, hot::ReloadStatus::Applied);
    ASSERT_EQ(h.world().Actors().size(), 1u);
    EXPECT_EQ(h.world().Actors()[0]->Name, "Doc");
    testing::internal::GetCapturedStdout();
}

// A script with an error keeps the old generation and the harness goes on.
TEST(HarnessTests, BrokenScriptKeepsTheOldGeneration)
{
    TempScript script("player_v1.hot");
    harness::Harness h(script.file.string());
    std::string error;
    testing::internal::CaptureStdout();
    ASSERT_TRUE(h.start(error)) << error;
    {
        std::ofstream f(script.file, std::ios::trunc);
        f << "class Player : Actor { this is not valid }\n";
    }
    testing::internal::CaptureStderr();
    h.frame(kDt, 2000);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("[harness] reload failed"), std::string::npos) << log;
    EXPECT_EQ(hot::active_generation, 1u);
    h.frame(kDt, 0);
    testing::internal::GetCapturedStdout();
}

TEST(HarnessTests, ParseCommand)
{
    EXPECT_EQ(harness::parse_command("  reload "), harness::Command::Reload);
    EXPECT_EQ(harness::parse_command("stats"), harness::Command::Stats);
    EXPECT_EQ(harness::parse_command("script_stats"), harness::Command::Stats);
    EXPECT_EQ(harness::parse_command("quit"), harness::Command::Quit);
    EXPECT_EQ(harness::parse_command(""), harness::Command::Empty);
    EXPECT_EQ(harness::parse_command("what"), harness::Command::Unknown);
}

} // namespace
