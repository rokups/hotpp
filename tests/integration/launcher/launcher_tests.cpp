// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the launcher library: load an entry script and run its "main".
// Each test runs for each backend that the build has.
#include "hot/launcher.h"

#include "TestHost.h"
#include "hot/backend.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
