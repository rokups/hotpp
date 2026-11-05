// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the launcher library: load an entry script and run its "main".
// Each test runs for each backend that the build has.
#include "hot/launcher.h"

#include "TestHost.h"
#include "hot/backend.h"
#include "hot/runtime/threads.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
std::string game_script(const std::string& step_body, const std::string& imports = "")
{
    return "using \"TestHost.h\";\n" + imports +
           "class Game : TestHost {\n"
           "    int count = 0;\n"
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

    // Returns false when the load failed. The caller uses ASSERT_TRUE.
    bool open(const std::string& backend, const std::string& step_body, const std::string& imports = "",
              const std::vector<std::pair<std::string, std::string>>& extra_files = {})
    {
        dir = make_temp_dir();
        for (const auto& [name, text] : extra_files)
            write_file(dir, name, text);
        fs::copy_file(fs::path(HOT_LAUNCHER_TEST_DIR) / "TestHost.h", dir / "TestHost.h");
        script = write_file(dir, "entry.hot", game_script(step_body, imports));
        launcher::Options options = options_for(backend, dir, &logs);
        options.include_dirs.push_back(dir.string());
        for (const std::string& d : extra_include_dirs)
            options.include_dirs.push_back((dir / d).string());
        options.poll_interval_ms = 0;
        options.debounce_ms = debounce_ms;
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
            statuses.push_back(f.program->poll());
        });
        ASSERT_EQ(statuses.size(), 6u);
        for (size_t i = 0; i < statuses.size(); ++i)
            EXPECT_EQ(statuses[i], i == 2 ? launcher::PollStatus::Applied : launcher::PollStatus::Idle) << "frame " << i + 1;
        EXPECT_EQ(total, 4021);
        EXPECT_EQ(f.program->generation(), 2u);
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
                first = f.program->poll();
            }
            else if (frame == 4)
                second = f.program->poll();
            else
                f.program->poll();
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
            statuses.push_back(f.program->poll());
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
            statuses.push_back(f.program->poll());
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
            statuses.push_back(f.program->poll());
        });
        ASSERT_EQ(statuses.size(), 3u);
        EXPECT_EQ(statuses[0], launcher::PollStatus::Idle);
        EXPECT_EQ(statuses[1], launcher::PollStatus::Applied);
        EXPECT_EQ(total, 201);
        EXPECT_EQ(f.program->generation(), 2u);
    }
}

#ifndef _WIN32
// Restores the environment variable XDG_CACHE_HOME when the test ends, also after a failed ASSERT.
class XdgCacheHomeGuard
{
public:
    XdgCacheHomeGuard()
    {
        const char* old = std::getenv("XDG_CACHE_HOME");
        had_value_ = old != nullptr;
        if (had_value_)
            old_value_ = old;
    }
    ~XdgCacheHomeGuard()
    {
        if (had_value_)
            setenv("XDG_CACHE_HOME", old_value_.c_str(), 1);
        else
            unsetenv("XDG_CACHE_HOME");
    }
    XdgCacheHomeGuard(const XdgCacheHomeGuard&) = delete;
    XdgCacheHomeGuard& operator=(const XdgCacheHomeGuard&) = delete;

private:
    bool had_value_ = false;
    std::string old_value_;
};

TEST(Launcher, DefaultCacheDirUsesXdgCacheHome)
{
    XdgCacheHomeGuard guard;
    const fs::path dir = make_temp_dir();
    ASSERT_EQ(setenv("XDG_CACHE_HOME", dir.string().c_str(), 1), 0);
    const std::string a = launcher::default_cache_dir((dir / "a.hot").string());
    const std::string b = launcher::default_cache_dir((dir / "b.hot").string());
    EXPECT_NE(a, b);
    EXPECT_EQ(a, launcher::default_cache_dir((dir / "a.hot").string()));
    EXPECT_EQ(fs::path(a).parent_path(), dir / "hot");
    EXPECT_TRUE(fs::is_directory(a));
}
#endif

} // namespace
