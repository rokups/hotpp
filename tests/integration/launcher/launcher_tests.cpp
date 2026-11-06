// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the launcher library: load an entry script and run its "main".
// Each test runs for each backend that the build has.
#include "hot/launcher.h"

#include "TestHost.h"
#include "hot/backend.h"
#include "hot/runtime/threads.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The statics of TestHost. They live in the test executable. The script modules find them there.
std::function<void(int frame)> TestHost::on_frame;
bool TestHost::running = false;

namespace
{

namespace fs = std::filesystem;
namespace launcher = hot::launcher;

// A new empty directory for each call. The directory name has the test name.
fs::path make_temp_dir()
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const fs::path dir = fs::path(HOT_SCRATCH_DIR) / (std::string(info->test_suite_name()) + "_" + info->name());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string write_file(const fs::path& dir, const std::string& name, const std::string& text)
{
    const fs::path path = dir / name;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    return path.string();
}

// The names of the backends that the build has.
std::vector<std::string> backends()
{
    std::vector<std::string> names;
    if (hot::backend_available(hot::BackendKind::ClangFiles))
        names.push_back("files");
    if (hot::backend_available(hot::BackendKind::Jit))
        names.push_back("jit");
    return names;
}

launcher::Options options_for(const std::string& backend, const fs::path& dir, std::vector<std::string>* logs)
{
    launcher::Options o;
    o.backend = backend;
    o.work_dir = (dir / "work").string();
    if (logs)
        o.log = [logs](const std::string& text) { logs->push_back(text); };
    return o;
}

TEST(Launcher, MainReturnsExitCode)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        const fs::path dir = make_temp_dir();
        const std::string script = write_file(dir, "entry.hot", "int main(std.string[] args) { return args.size() * 10; }\n");
        std::string error;
        auto program = launcher::load(options_for(backend, dir, nullptr), script, error);
        ASSERT_NE(program, nullptr) << error;
        EXPECT_EQ(launcher::run_main(*program, {"a", "b"}), 30);
        EXPECT_EQ(launcher::run_main(*program, {}), 10);
        EXPECT_EQ(program->script_path(), script);
    }
}

TEST(Launcher, ProgramMoves)
{
    const fs::path dir = make_temp_dir();
    const std::string script = write_file(dir, "entry.hot", "int main() { return 7; }\n");
    std::string error;
    auto program = launcher::load(options_for("", dir, nullptr), script, error);
    ASSERT_NE(program, nullptr) << error;
    launcher::Program moved = std::move(*program);
    program.reset();
    EXPECT_EQ(launcher::run_main(moved, {}), 7);
}

TEST(Launcher, ImportedScriptRuns)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        const fs::path dir = make_temp_dir();
        write_file(dir, "util.hot", "class Util { static int Triple(int x) { return x * 3; } }\n");
        const std::string script = write_file(dir, "entry.hot",
                                              "using \"util.hot\";\nint main() { return Util.Triple(14); }\n");
        std::string error;
        auto program = launcher::load(options_for(backend, dir, nullptr), script, error);
        ASSERT_NE(program, nullptr) << error;
        EXPECT_EQ(launcher::run_main(*program, {}), 42);
        // The import is a dependency of the program.
        bool found = false;
        for (const std::string& d : program->dependencies())
            found = found || d.find("util.hot") != std::string::npos;
        EXPECT_TRUE(found);
    }
}

TEST(Launcher, CompileErrorGivesNoProgram)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        const fs::path dir = make_temp_dir();
        const std::string script = write_file(dir, "entry.hot", "int main() { return missing_name; }\n");
        std::string error;
        auto program = launcher::load(options_for(backend, dir, nullptr), script, error);
        EXPECT_EQ(program, nullptr);
        EXPECT_FALSE(error.empty());
    }
}

TEST(Launcher, MissingScriptGivesError)
{
    const fs::path dir = make_temp_dir();
    std::string error;
    auto program = launcher::load(options_for("", dir, nullptr), (dir / "none.hot").string(), error);
    EXPECT_EQ(program, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(Launcher, UnknownBackendGivesError)
{
    const fs::path dir = make_temp_dir();
    const std::string script = write_file(dir, "entry.hot", "int main() { return 0; }\n");
    std::string error;
    auto program = launcher::load(options_for("nope", dir, nullptr), script, error);
    EXPECT_EQ(program, nullptr);
    EXPECT_NE(error.find("nope"), std::string::npos);
}

TEST(Launcher, NoMainGivesCode2)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        const fs::path dir = make_temp_dir();
        const std::string script = write_file(dir, "entry.hot", "int helper() { return 1; }\n");
        std::vector<std::string> logs;
        std::string error;
        auto program = launcher::load(options_for(backend, dir, &logs), script, error);
        ASSERT_NE(program, nullptr) << error;
        EXPECT_EQ(launcher::run_main(*program, {}), 2);
        ASSERT_EQ(logs.size(), 1u);
        EXPECT_EQ(logs[0], "the script has no 'main'");
    }
}

TEST(Launcher, ThrowingMainGivesCode1)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        const fs::path dir = make_temp_dir();
        const std::string script = write_file(dir, "entry.hot",
                                              "using <stdexcept>;\n"
                                              "int main() { throw std.runtime_error(\"boom\"); return 0; }\n");
        std::vector<std::string> logs;
        std::string error;
        auto program = launcher::load(options_for(backend, dir, &logs), script, error);
        ASSERT_NE(program, nullptr) << error;
        EXPECT_EQ(launcher::run_main(*program, {}), 1);
        ASSERT_EQ(logs.size(), 1u);
        EXPECT_NE(logs[0].find("boom"), std::string::npos) << logs[0];
    }
}

// A script class derives from the native class TestHost. The native loop Run calls its Step.
// on_frame runs before Step. The test stops the loop at frame 3 after Step runs for frame 3.
// The loop checks "running" at the start of the next iteration. Steps 1, 2 and 3 give 1 + 2 + 3 = 6.
TEST(Launcher, LoopRunsFrames)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        const fs::path dir = make_temp_dir();
        fs::copy_file(fs::path(HOT_LAUNCHER_TEST_DIR) / "TestHost.h", dir / "TestHost.h");
        const std::string script = write_file(dir, "entry.hot",
                                              "using \"TestHost.h\";\n"
                                              "class Game : TestHost {\n"
                                              "    int count = 0;\n"
                                              "    override int Step() { count = count + 1; return count; }\n"
                                              "}\n"
                                              "int main() { $Game g = new $Game(); return g.Run(); }\n");
        launcher::Options options = options_for(backend, dir, nullptr);
        options.include_dirs.push_back(dir.string());
        std::string error;
        auto program = launcher::load(options, script, error);
        ASSERT_NE(program, nullptr) << error;

        std::vector<int> frames;
        TestHost::running = true;
        TestHost::on_frame = [&frames](int frame)
        {
            frames.push_back(frame);
            if (frame == 3)
                TestHost::running = false;
        };
        const int result = launcher::run_main(*program, {});
        TestHost::on_frame = nullptr;
        TestHost::running = false;
        EXPECT_EQ(result, 6);
        EXPECT_EQ(frames, (std::vector<int>{1, 2, 3}));
    }
}

// Writes the new text of a file and moves its write time forward by two seconds. Then a poll sees
// the change, also on a file system with a coarse clock.
void edit_file(const fs::path& path, const std::string& text)
{
    const fs::file_time_type old_time = fs::last_write_time(path);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    fs::last_write_time(path, old_time + std::chrono::seconds(2));
}

// The script of a reload test: a class Game derived from TestHost. "step_body" is the body of Step.
// "members" are more members of the class.
std::string game_script(const std::string& step_body, const std::string& imports = "", const std::string& members = "")
{
    return "using \"TestHost.h\";\n" + imports +
           "class Game : TestHost {\n"
           "    int count = 0;\n"
           "    " + members + "\n"
           "    override int Step() { " + step_body + " }\n"
           "}\n"
           "int main() { $Game g = new $Game(); return g.Run(); }\n";
}

// A program for a reload test. It has poll_interval_ms = 0 and debounce_ms = 0.
struct ReloadFixture
{
    fs::path dir;
    std::vector<std::string> logs;
    std::string script;
    std::unique_ptr<launcher::Program> program;
    // Include directories that open adds after the folder of the script. They are relative to it.
    std::vector<std::string> extra_include_dirs;
    unsigned debounce_ms = 0;
    // More members of the class Game, and the quit hook of the options.
    std::string members;
    std::function<void()> request_quit;

    // Returns false when the load failed. The caller uses ASSERT_TRUE.
    bool open(const std::string& backend, const std::string& step_body, const std::string& imports = "",
              const std::vector<std::pair<std::string, std::string>>& extra_files = {})
    {
        dir = make_temp_dir();
        for (const auto& [name, text] : extra_files)
            write_file(dir, name, text);
        fs::copy_file(fs::path(HOT_LAUNCHER_TEST_DIR) / "TestHost.h", dir / "TestHost.h");
        script = write_file(dir, "entry.hot", game_script(step_body, imports, members));
        launcher::Options options = options_for(backend, dir, &logs);
        options.include_dirs.push_back(dir.string());
        for (const std::string& d : extra_include_dirs)
            options.include_dirs.push_back((dir / d).string());
        options.poll_interval_ms = 0;
        options.debounce_ms = debounce_ms;
        options.request_quit = request_quit;
        std::string error;
        program = launcher::load(options, script, error);
        if (!program)
            logs.push_back(error);
        return program != nullptr;
    }

    // Runs "main" with the hook. The loop stops after the frame "last_frame".
    int run(int last_frame, std::function<void(int)> hook)
    {
        TestHost::running = true;
        TestHost::on_frame = [&](int frame)
        {
            hook(frame);
            if (frame == last_frame)
                TestHost::running = false;
        };
        const int result = launcher::run_main(*program, {});
        TestHost::on_frame = nullptr;
        TestHost::running = false;
        return result;
    }
};

// Calls poll with a 10 ms sleep until the status is not Building. It gives the last status.
// The status Building stays when the build does not end in the time limit (60 s).
launcher::PollStatus poll_until_settled(launcher::Program& program)
{
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    launcher::PollStatus status = program.poll();
    while (status == launcher::PollStatus::Building && std::chrono::steady_clock::now() < limit)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        status = program.poll();
    }
    return status;
}

// Frames 1 and 2 run v1: count is 1 and 2. At frame 3 the poll applies v2 before Step. Frames 3 to 6
// run v2. The count continues from 2 (the state moves to the new generation): 3, 4, 5 and 6, and
// each frame adds 1000. Total = (1 + 2) + (3 + 4 + 5 + 6) + 4 * 1000 = 4021.
TEST(Launcher, ReloadKeepsState)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "count = count + 1; return count;")) << f.logs.back();
        EXPECT_EQ(f.program->generation(), 1u);
        std::vector<launcher::PollStatus> statuses;
        const int total = f.run(6, [&](int frame)
        {
            if (frame == 3)
                edit_file(f.script, game_script("count = count + 1; return count + 1000;"));
            statuses.push_back(poll_until_settled(*f.program));
        });
        ASSERT_EQ(statuses.size(), 6u);
        for (size_t i = 0; i < statuses.size(); ++i)
            EXPECT_EQ(statuses[i], i == 2 ? launcher::PollStatus::Applied : launcher::PollStatus::Idle) << "frame " << i + 1;
        EXPECT_EQ(total, 4021);
        EXPECT_EQ(f.program->generation(), 2u);
    }
}

// The first poll after an edit starts the build and gives Building. A later poll gives Applied.
TEST(Launcher, PollReturnsBuildingThenApplied)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "return 1;")) << f.logs.back();
        edit_file(f.script, game_script("return 10;"));
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Building);
        // The new code is not live while the build runs or before the commit.
        EXPECT_EQ(f.program->generation(), 1u);
        EXPECT_EQ(poll_until_settled(*f.program), launcher::PollStatus::Applied);
        EXPECT_EQ(f.program->generation(), 2u);
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Idle);
    }
}

// The destructor waits for a running build. The test has no loop: the program never runs.
TEST(Launcher, DestroyWhileBuilding)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "return 1;")) << f.logs.back();
        edit_file(f.script, game_script("return 10;"));
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Building);
        f.program.reset();
    }
}

// An edit during a build is not lost. The first build has the first text. The scan after the build
// sees the second edit, and a second build applies it.
// Frame 1: the first poll starts the build of v2, and the file changes to v3 at once. The build
// applies v2 before Step: 10. Frame 2: the hook waits for the build of v3: 100. Total = 110.
TEST(Launcher, EditDuringBuildIsNotLost)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "return 1;")) << f.logs.back();
        std::vector<launcher::PollStatus> statuses;
        const int total = f.run(2, [&](int frame)
        {
            if (frame == 1)
            {
                edit_file(f.script, game_script("return 10;"));
                statuses.push_back(f.program->poll());
                edit_file(f.script, game_script("return 100;"));
                statuses.push_back(poll_until_settled(*f.program));
                return;
            }
            // The poll after the first reload starts the second build.
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            launcher::PollStatus status = launcher::PollStatus::Idle;
            while (status == launcher::PollStatus::Idle && std::chrono::steady_clock::now() < limit)
            {
                status = poll_until_settled(*f.program);
                if (status == launcher::PollStatus::Idle)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            statuses.push_back(status);
        });
        ASSERT_EQ(statuses.size(), 3u);
        EXPECT_EQ(statuses[0], launcher::PollStatus::Building);
        EXPECT_EQ(statuses[1], launcher::PollStatus::Applied);
        EXPECT_EQ(statuses[2], launcher::PollStatus::Applied);
        EXPECT_EQ(total, 110);
        EXPECT_EQ(f.program->generation(), 3u);
    }
}

TEST(Launcher, CompileErrorKeepsOldCode)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "count = count + 1; return count;")) << f.logs.back();
        launcher::PollStatus first = launcher::PollStatus::Idle;
        launcher::PollStatus second = launcher::PollStatus::Applied;
        // The old code runs for all 6 frames: 1 + 2 + 3 + 4 + 5 + 6 = 21.
        const int total = f.run(6, [&](int frame)
        {
            if (frame == 3)
            {
                edit_file(f.script, game_script("count = count + 1; return count +;"));
                first = poll_until_settled(*f.program);
            }
            else if (frame == 4)
                second = poll_until_settled(*f.program);
            else
                poll_until_settled(*f.program);
        });
        EXPECT_EQ(first, launcher::PollStatus::Failed);
        EXPECT_EQ(second, launcher::PollStatus::Idle);
        EXPECT_EQ(total, 21);
        EXPECT_EQ(f.program->generation(), 1u);
        bool reported = false;
        for (const std::string& line : f.logs)
            reported = reported || line.find("[hot] reload failed: ") == 0;
        EXPECT_TRUE(reported);
    }
}

// Frames 1 and 2 give 1 each. At frame 3 the poll applies the new util.hot. Frames 3 and 4 give 100 each.
// Total = 2 + 200 = 202.
TEST(Launcher, ImportChangeReloads)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "return Util.Bonus();", "using \"util.hot\";\n",
                           {{"util.hot", "class Util { static int Bonus() { return 1; } }\n"}}))
            << f.logs.back();
        bool found = false;
        for (const std::string& d : f.program->dependencies())
            found = found || d.find("util.hot") != std::string::npos;
        ASSERT_TRUE(found);
        std::vector<launcher::PollStatus> statuses;
        const int total = f.run(4, [&](int frame)
        {
            if (frame == 3)
                edit_file(f.dir / "util.hot", "class Util { static int Bonus() { return 100; } }\n");
            statuses.push_back(poll_until_settled(*f.program));
        });
        ASSERT_EQ(statuses.size(), 4u);
        for (size_t i = 0; i < statuses.size(); ++i)
            EXPECT_EQ(statuses[i], i == 2 ? launcher::PollStatus::Applied : launcher::PollStatus::Idle) << "frame " << i + 1;
        EXPECT_EQ(total, 202);
        EXPECT_EQ(f.program->generation(), 2u);
    }
}

TEST(Launcher, NoChangeIsIdle)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "count = count + 1; return count;")) << f.logs.back();
        int not_idle = 0;
        const int total = f.run(5, [&](int)
        {
            if (f.program->poll() != launcher::PollStatus::Idle)
                ++not_idle;
        });
        EXPECT_EQ(not_idle, 0);
        EXPECT_EQ(total, 15);
        EXPECT_EQ(f.program->generation(), 1u);
    }
}

// Two reloads happen while "main" is on the stack. The generation of "main" is retired, and the
// pin keeps it loaded. Frame 1 gives 1 (v1). At frame 2 the poll applies v2 (10). At frame 3 the
// poll applies v3 (100). Frames 4, 5 and 6 run frame_boundary again and give 100 each.
// Total = 1 + 10 + 4 * 100 = 411.
TEST(Launcher, MainGenerationStaysLoadedAfterReloads)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, "return 1;")) << f.logs.back();
        std::vector<launcher::PollStatus> statuses;
        std::vector<uint32_t> pins;
        const int total = f.run(6, [&](int frame)
        {
            if (frame == 2)
                edit_file(f.script, game_script("return 10;"));
            if (frame == 3)
                edit_file(f.script, game_script("return 100;"));
            statuses.push_back(poll_until_settled(*f.program));
            // The pin of generation 1 (the generation of "main") guards its library.
            pins.push_back(hot::pin_count(1));
        });
        // The pin is gone when run_main returns.
        EXPECT_EQ(hot::pin_count(1), 0u);
        ASSERT_EQ(pins.size(), 6u);
        for (size_t i = 1; i < pins.size(); ++i)
            EXPECT_EQ(pins[i], 1u) << "frame " << i + 1;
        ASSERT_EQ(statuses.size(), 6u);
        EXPECT_EQ(statuses[1], launcher::PollStatus::Applied);
        EXPECT_EQ(statuses[2], launcher::PollStatus::Applied);
        EXPECT_EQ(total, 411);
        EXPECT_EQ(f.program->generation(), 3u);
    }
}

// The debounce time is longer than the poll interval (0). Each scan finds the same change. The
// debounce time must run from the first scan that sees the change, not restart on each scan.
TEST(Launcher, NonZeroDebounceStillBuilds)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.debounce_ms = 50;
        ASSERT_TRUE(f.open(backend, "return 1;")) << f.logs.back();
        launcher::PollStatus result = launcher::PollStatus::Idle;
        const int total = f.run(2, [&](int frame)
        {
            if (frame != 1)
                return;
            edit_file(f.script, game_script("return 10;"));
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < limit)
            {
                result = f.program->poll();
                if (result == launcher::PollStatus::Applied)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        EXPECT_EQ(result, launcher::PollStatus::Applied);
        EXPECT_EQ(f.program->generation(), 2u);
        // The hook runs before Step. Frames 1 and 2 run v2 (10 each).
        EXPECT_EQ(total, 20);
    }
}

// A change that returns to the stamps of the build is no change. The poll does not build.
TEST(Launcher, RevertedChangeIsIdle)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.debounce_ms = 50;
        ASSERT_TRUE(f.open(backend, "return 1;")) << f.logs.back();
        const fs::file_time_type old_time = fs::last_write_time(f.script);
        const std::string old_text = game_script("return 1;");
        edit_file(f.script, game_script("return 2;"));
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Idle); // dirty, waits for the debounce
        // The same size and the old write time: the stamps equal the stamps of the build.
        std::ofstream(f.script, std::ios::binary | std::ios::trunc) << old_text;
        fs::last_write_time(f.script, old_time);
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Idle); // the scan clears the dirty flag
        // A dirty flag that stays set would build now and give Applied.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Idle);
        EXPECT_EQ(f.program->generation(), 1u);
        EXPECT_TRUE(f.logs.empty());
    }
}

// The script imports "lib/helper.hot". The file is only in an include directory, not in the folder
// of the script. The program watches the file there. An edit of it applies a reload.
// Frame 1 gives 1. At frame 2 the poll applies the new file. Frames 2 and 3 give 100 each. Total = 201.
TEST(Launcher, IncludeDirImportChangeReloads)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.extra_include_dirs.push_back("inc");
        ASSERT_TRUE(f.open(backend, "return Helper.Bonus();", "using \"lib/helper.hot\";\n",
                           {{"inc/lib/helper.hot", "class Helper { static int Bonus() { return 1; } }\n"}}))
            << f.logs.back();
        const fs::path inc = f.dir / "inc";
        // The translator gives the path where it found the import. For this import it is absolute.
        bool found = false;
        for (const std::string& d : f.program->dependencies())
            if (d.find("helper.hot") != std::string::npos)
            {
                found = true;
                EXPECT_TRUE(fs::path(d).is_absolute()) << d;
                EXPECT_TRUE(fs::exists(d)) << d;
            }
        ASSERT_TRUE(found);
        std::vector<launcher::PollStatus> statuses;
        const int total = f.run(3, [&](int frame)
        {
            if (frame == 2)
                edit_file(inc / "lib" / "helper.hot", "class Helper { static int Bonus() { return 100; } }\n");
            statuses.push_back(poll_until_settled(*f.program));
        });
        ASSERT_EQ(statuses.size(), 3u);
        EXPECT_EQ(statuses[0], launcher::PollStatus::Idle);
        EXPECT_EQ(statuses[1], launcher::PollStatus::Applied);
        EXPECT_EQ(total, 201);
        EXPECT_EQ(f.program->generation(), 2u);
    }
}

// The number of log lines that start with the text.
int count_logs(const std::vector<std::string>& logs, const std::string& prefix)
{
    int n = 0;
    for (const std::string& line : logs)
        n += line.rfind(prefix, 0) == 0 ? 1 : 0;
    return n;
}

// The quit hook of the restart tests: it ends the loop of the engine.
void stop_loop()
{
    TestHost::running = false;
}

// v1 of Game derives from the native class TestHost. v2 derives from the script class Base. A
// change of the script parent needs a full restart. A removed function does not: it reloads live.
std::string reparented_script(const std::string& step_body)
{
    return "using \"TestHost.h\";\n"
           "class Base : TestHost {\n"
           "}\n"
           "class Game : Base {\n"
           "    int count = 0;\n"
           "    override int Step() { " + step_body + " }\n"
           "}\n"
           "int main() { $Game g = new $Game(); return g.Run(); }\n";
}

const char* const kStepV1 = "count = count + 1; return count;";
const char* const kStepV2 = "count = count + 1; return count + 100;";

// The first run starts v1. At frame 1 the hook writes v2 (Game gets a script parent). The poll gives
// RestartPending and calls the quit hook, which ends the loop. Step of frame 1 runs: v1, the
// result 1 is lost with the first run. "main" returns and run_main restarts: a new Game
// object, and "count" starts at 0 again. The second run uses v2 and stops at frame 2 (the
// limit of f.run). Frame 1 gives 0 + 1 + 100 = 101, frame 2 gives 2 + 100 = 102.
// Total = 203. The hook sees two starts (frame 1 twice).
TEST(Launcher, ScriptParentChangeRestarts)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.request_quit = stop_loop;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        int starts = 0;
        launcher::PollStatus status = launcher::PollStatus::Idle;
        bool pending_in_hook = false;
        const int total = f.run(2, [&](int frame)
        {
            if (frame != 1)
                return;
            if (++starts == 1)
            {
                edit_file(f.script, reparented_script(kStepV2));
                status = poll_until_settled(*f.program);
                pending_in_hook = f.program->restart_pending();
            }
        });
        EXPECT_EQ(status, launcher::PollStatus::RestartPending);
        EXPECT_TRUE(pending_in_hook);
        EXPECT_EQ(starts, 2);
        EXPECT_EQ(total, 203);
        EXPECT_FALSE(f.program->restart_pending());
        EXPECT_EQ(f.program->generation(), 1u);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart: "), 2); // "waiting" and the restart itself
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart needed: "), 1);
        // The program goes on after the restart: no change, no status.
        EXPECT_EQ(f.program->poll(), launcher::PollStatus::Idle);
    }
}

// The text of a file.
std::string read_text(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A change of a native header is rejected. The test adds a comment line to the copy of TestHost.h.
// The poll starts no build and gives Failed once, with one log line. The later polls give Idle.
// The old code goes on: no restart. The loop stops at frame 4: 1 + 2 + 3 + 4 = 10.
TEST(Launcher, HeaderChangeIsRejected)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.request_quit = stop_loop;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        const fs::path header = f.dir / "TestHost.h";
        int starts = 0;
        std::vector<launcher::PollStatus> statuses;
        const int total = f.run(4, [&](int frame)
        {
            if (frame == 1)
            {
                ++starts;
                edit_file(header, read_text(header) + "// a comment line\n");
            }
            statuses.push_back(poll_until_settled(*f.program));
        });
        ASSERT_EQ(statuses.size(), 4u);
        EXPECT_EQ(statuses[0], launcher::PollStatus::Failed);
        for (size_t i = 1; i < statuses.size(); ++i)
            EXPECT_EQ(statuses[i], launcher::PollStatus::Idle) << "frame " << i + 1;
        EXPECT_EQ(starts, 1);
        EXPECT_EQ(total, 10);
        EXPECT_FALSE(f.program->restart_pending());
        EXPECT_EQ(f.program->generation(), 1u);
        EXPECT_EQ(count_logs(f.logs, "[hot] native dependency changed: "), 1);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart"), 0);
        EXPECT_EQ(count_logs(f.logs, "[hot] reload"), 0);
    }
}

// A header change and a script edit together: no build starts while the header differs from its
// build stamp. Then the test restores the header (the old text and the old write time): the stamps
// are equal again, the pause ends, and the script edit builds (Applied).
// Frame 1 runs v2 after the commit (count 1 + 100 = 101). Frame 2 gives 102. Total = 203.
TEST(Launcher, HeaderRestoredEndsThePause)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        const fs::path header = f.dir / "TestHost.h";
        const std::string old_text = read_text(header);
        const fs::file_time_type old_time = fs::last_write_time(header);
        std::vector<launcher::PollStatus> statuses;
        int logs_while_paused = 0;
        const int total = f.run(2, [&](int frame)
        {
            if (frame != 1)
                return;
            edit_file(header, old_text + "// a comment line\n");
            edit_file(f.script, game_script(kStepV2));
            statuses.push_back(poll_until_settled(*f.program));
            statuses.push_back(poll_until_settled(*f.program));
            statuses.push_back(poll_until_settled(*f.program));
            logs_while_paused = static_cast<int>(f.logs.size());
            EXPECT_EQ(f.program->generation(), 1u);
            std::ofstream(header, std::ios::binary | std::ios::trunc) << old_text;
            fs::last_write_time(header, old_time);
            statuses.push_back(poll_until_settled(*f.program));
        });
        ASSERT_EQ(statuses.size(), 4u);
        EXPECT_EQ(statuses[0], launcher::PollStatus::Failed);
        EXPECT_EQ(statuses[1], launcher::PollStatus::Idle);
        EXPECT_EQ(statuses[2], launcher::PollStatus::Idle);
        EXPECT_EQ(statuses[3], launcher::PollStatus::Applied);
        EXPECT_EQ(logs_while_paused, 1);
        EXPECT_EQ(count_logs(f.logs, "[hot] native dependency changed: "), 1);
        EXPECT_EQ(total, 203);
        EXPECT_EQ(f.program->generation(), 2u);
    }
}

// The engine has no quit hook. The poll gives RestartPending and logs one line. The old code
// goes on: the loop stops at frame 4 (the limit of f.run), and run_main returns the total of v1
// (1 + 2 + 3 + 4 = 10) with no restart. The later polls give RestartPending and ignore edits.
TEST(Launcher, NoQuitHookLogs)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        int starts = 0;
        std::vector<launcher::PollStatus> statuses;
        const int total = f.run(4, [&](int frame)
        {
            if (frame == 1)
            {
                ++starts;
                edit_file(f.script, reparented_script(kStepV2));
            }
            statuses.push_back(poll_until_settled(*f.program));
            if (frame == 2)
                edit_file(f.script, game_script("return 5;")); // ignored
        });
        EXPECT_EQ(starts, 1);
        EXPECT_EQ(total, 10);
        ASSERT_EQ(statuses.size(), 4u);
        for (const launcher::PollStatus s : statuses)
            EXPECT_EQ(s, launcher::PollStatus::RestartPending);
        EXPECT_TRUE(f.program->restart_pending());
        EXPECT_EQ(f.program->generation(), 1u);
        EXPECT_EQ(count_logs(f.logs, "[hot] restart needed, but the engine has no quit hook"), 1);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart: "), 0);
    }
}

// The first edit changes the script parent and has a syntax error: the build fails (Failed, no
// restart). The second edit fixes the syntax and keeps the new parent: RestartPending and a restart.
// The first run has frames 1 and 2 (the quit ends it after frame 2). The second run runs v2 and
// stops at frame 2: 101 + 102 = 203.
TEST(Launcher, RestartBuildFailureKeepsOldCode)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.request_quit = stop_loop;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        int starts = 0;
        launcher::PollStatus first = launcher::PollStatus::Idle;
        launcher::PollStatus second = launcher::PollStatus::Idle;
        const int total = f.run(2, [&](int frame)
        {
            if (frame == 1)
                ++starts;
            if (starts != 1)
                return;
            if (frame == 1)
            {
                edit_file(f.script, game_script("count = count + 1; return count +;"));
                first = poll_until_settled(*f.program);
            }
            else if (frame == 2)
            {
                edit_file(f.script, reparented_script(kStepV2));
                second = poll_until_settled(*f.program);
            }
        });
        EXPECT_EQ(first, launcher::PollStatus::Failed);
        EXPECT_EQ(second, launcher::PollStatus::RestartPending);
        EXPECT_EQ(starts, 2);
        EXPECT_EQ(total, 203);
        EXPECT_GE(count_logs(f.logs, "[hot] reload failed: "), 1);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart: "), 2);
        EXPECT_EQ(f.program->generation(), 1u);
    }
}

// A failed Fresh build does not forget the full restart. The first edit changes the script parent:
// the build of v2 works, and the loader says that a full restart is needed. While that build runs,
// the script gets a syntax error. The Fresh build reads the new text and fails (Failed). The second
// edit is a valid script with the old parent, so a Reload build would apply it (Applied). Because
// the restart is still needed, the build must be Fresh: RestartPending, not Applied.
// The first run has frames 1 and 2 (the quit ends it after frame 2). The second run runs the
// text of the second edit and stops at frame 2: 101 + 102 = 203.
TEST(Launcher, FailedFreshBuildKeepsFreshNeeded)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.request_quit = stop_loop;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        int starts = 0;
        launcher::PollStatus begin = launcher::PollStatus::Idle;
        launcher::PollStatus first = launcher::PollStatus::Idle;
        launcher::PollStatus second = launcher::PollStatus::Idle;
        const int total = f.run(2, [&](int frame)
        {
            if (frame == 1)
                ++starts;
            if (starts != 1)
                return;
            if (frame == 1)
            {
                edit_file(f.script, reparented_script(kStepV2));
                begin = f.program->poll(); // the Reload build of v2 starts and has the text of v2
                edit_file(f.script, game_script("count = count + 1; return count +;"));
                first = poll_until_settled(*f.program);
            }
            else if (frame == 2)
            {
                edit_file(f.script, game_script(kStepV2));
                second = poll_until_settled(*f.program);
            }
        });
        EXPECT_EQ(begin, launcher::PollStatus::Building);
        EXPECT_EQ(first, launcher::PollStatus::Failed);
        EXPECT_EQ(second, launcher::PollStatus::RestartPending);
        EXPECT_EQ(starts, 2);
        EXPECT_EQ(total, 203);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart build failed: "), 1);
        EXPECT_EQ(count_logs(f.logs, "[hot] reload applied: "), 0);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart: "), 2);
        EXPECT_FALSE(f.program->restart_pending());
        EXPECT_EQ(f.program->generation(), 1u);
    }
}

// A pin on generation 1 makes Loader::reset fail. run_main logs the error and returns 1. The
// program is not restarted and stays pending.
TEST(Launcher, ResetFailureReturns1)
{
    for (const std::string& backend : backends())
    {
        SCOPED_TRACE(backend);
        ReloadFixture f;
        f.request_quit = stop_loop;
        ASSERT_TRUE(f.open(backend, kStepV1)) << f.logs.back();
        int starts = 0;
        const int result = f.run(2, [&](int frame)
        {
            if (frame != 1)
                return;
            ++starts;
            edit_file(f.script, reparented_script(kStepV2));
            EXPECT_EQ(poll_until_settled(*f.program), launcher::PollStatus::RestartPending);
            hot::pin(1); // a closure or a task would do the same
        });
        hot::unpin(1);
        EXPECT_EQ(result, 1);
        EXPECT_EQ(starts, 1);
        EXPECT_EQ(count_logs(f.logs, "[hot] full restart failed: "), 1);
        EXPECT_TRUE(f.program->restart_pending());
        EXPECT_EQ(f.program->generation(), 1u);
    }
}

// Restores an environment variable when the test ends, also after a failed ASSERT.
class EnvGuard
{
public:
    explicit EnvGuard(const char* name)
        : name_(name)
    {
        const char* old = std::getenv(name);
        had_value_ = old != nullptr;
        if (had_value_)
            old_value_ = old;
    }
    ~EnvGuard()
    {
        if (had_value_)
            test_support::set_env(name_.c_str(), old_value_.c_str());
        else
            test_support::unset_env(name_.c_str());
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    std::string name_;
    bool had_value_ = false;
    std::string old_value_;
};

// The cache folder comes from XDG_CACHE_HOME on Linux and from LOCALAPPDATA on Windows.
#ifdef _WIN32
constexpr const char* kCacheHomeVar = "LOCALAPPDATA";
#else
constexpr const char* kCacheHomeVar = "XDG_CACHE_HOME";
#endif

TEST(Launcher, DefaultCacheDirUsesTheCacheHomeVariable)
{
    EnvGuard guard(kCacheHomeVar);
    const fs::path dir = make_temp_dir();
    test_support::set_env(kCacheHomeVar, dir.string().c_str());
    const std::string a = launcher::default_cache_dir((dir / "a.hot").string());
    const std::string b = launcher::default_cache_dir((dir / "b.hot").string());
    EXPECT_NE(a, b);
    EXPECT_EQ(a, launcher::default_cache_dir((dir / "a.hot").string()));
    EXPECT_EQ(fs::path(a).parent_path(), dir / "hot");
    EXPECT_TRUE(fs::is_directory(a));
}

} // namespace
