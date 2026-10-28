// The file watcher of the harness (design 8.1), with a temporary directory. Both the inotify
// mode and the polling fallback must see a change of the watched file and nothing else.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "FileWatcher.h"

namespace {

namespace fs = std::filesystem;

class FileWatcherTests : public ::testing::TestWithParam<harness::FileWatcher::Mode> {
protected:
    void SetUp() override
    {
        dir = fs::temp_directory_path() /
              ("spp_watch_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
               std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
        file = (dir / "a.spp").string();
        write(file, "one");
    }
    void TearDown() override { fs::remove_all(dir); }
    static void write(const std::string& path, const std::string& text)
    {
        std::ofstream f(path, std::ios::trunc);
        f << text;
    }
    fs::path dir;
    std::string file;
};

TEST_P(FileWatcherTests, SeesWriteAndRenameButNotOtherFiles)
{
    harness::FileWatcher w(file, GetParam());
    EXPECT_EQ(w.uses_inotify(), GetParam() == harness::FileWatcher::Mode::Auto);
    EXPECT_FALSE(w.wait_changed(0));  // nothing changed yet

    write((dir / "other.txt").string(), "x");  // another file in the same directory
    EXPECT_FALSE(w.wait_changed(50));

    write(file, "two words");
    EXPECT_TRUE(w.wait_changed(1000));
    EXPECT_FALSE(w.wait_changed(0));  // the change is consumed

    // An editor writes a new file and renames it over the old one.
    write((dir / "a.tmp").string(), "three");
    fs::rename(dir / "a.tmp", file);
    EXPECT_TRUE(w.wait_changed(1000));
    EXPECT_FALSE(w.wait_changed(0));
}

INSTANTIATE_TEST_SUITE_P(Modes, FileWatcherTests,
                         ::testing::Values(harness::FileWatcher::Mode::Auto,
                                           harness::FileWatcher::Mode::Polling));

}  // namespace
