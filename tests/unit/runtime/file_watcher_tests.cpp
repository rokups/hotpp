// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The file watcher of the harness (design 8.1), with a temporary directory. Both the inotify
// mode and the polling fallback must see a change of the watched file and nothing else.
#include "FileWatcher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{

namespace fs = std::filesystem;

class FileWatcherTests : public ::testing::TestWithParam<harness::FileWatcher::Mode>
{
protected:
    void SetUp() override
    {
        dir = fs::temp_directory_path()
            / ("hot_watch_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_"
                + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
        file = (dir / "a.hot").string();
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

// Waits for a change until the timeout. A machine under load can need more than one second.
bool changed_within(harness::FileWatcher& w, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do
    {
        if (w.wait_changed(100))
            return true;
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

TEST_P(FileWatcherTests, SeesWriteAndRenameButNotOtherFiles)
{
    harness::FileWatcher w(file, GetParam());
#ifdef __linux__
    EXPECT_EQ(w.uses_inotify(), GetParam() == harness::FileWatcher::Mode::Auto);
#else
    EXPECT_FALSE(w.uses_inotify()); // only Linux has inotify
#endif
    EXPECT_FALSE(w.wait_changed(0)); // nothing changed yet

    write((dir / "other.txt").string(), "x"); // another file in the same directory
    EXPECT_FALSE(w.wait_changed(50));

    write(file, "two words");
    EXPECT_TRUE(changed_within(w, 15000));
    EXPECT_FALSE(w.wait_changed(0)); // the change is consumed

    // An editor writes a new file and renames it over the old one.
    write((dir / "a.tmp").string(), "three");
    fs::rename(dir / "a.tmp", file);
    EXPECT_TRUE(changed_within(w, 15000));
    EXPECT_FALSE(w.wait_changed(0));
}

// An edit sets the old modification time back and keeps the size. The polling mode sees it
// through the ctime and the hash of the content.
TEST_P(FileWatcherTests, PollingSeesAnEditThatRestoresTheTimeAndTheSize)
{
    if (GetParam() != harness::FileWatcher::Mode::Polling)
        GTEST_SKIP() << "inotify sees every write";
    write(file, "aaaa");
    const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
    fs::last_write_time(file, old_time);
    harness::FileWatcher w(file, harness::FileWatcher::Mode::Polling);
    EXPECT_FALSE(w.wait_changed(0));

    write(file, "bbbb");
    fs::last_write_time(file, old_time);
    EXPECT_TRUE(w.wait_changed(1000));
    EXPECT_FALSE(w.wait_changed(0));

    // A touch with the same content is no change.
    write(file, "bbbb");
    fs::last_write_time(file, old_time);
    EXPECT_FALSE(w.wait_changed(50));

    // A new file with another inode, the old time and the same size.
    write((dir / "a.tmp").string(), "cccc");
    fs::last_write_time(dir / "a.tmp", old_time);
    fs::rename(dir / "a.tmp", file);
    EXPECT_TRUE(w.wait_changed(1000));
}

INSTANTIATE_TEST_SUITE_P(
    Modes, FileWatcherTests, ::testing::Values(harness::FileWatcher::Mode::Auto, harness::FileWatcher::Mode::Polling));

} // namespace
