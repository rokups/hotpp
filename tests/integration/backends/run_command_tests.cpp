// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::run_command (hot/clang_files_backend.h): commands of two threads at a time.
#include "hot/clang_files_backend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#ifndef _WIN32

// A command of one thread must not keep the output pipe of a command of another thread open.
// Else the read of the short command waits for the end of the long command.
TEST(RunCommand, PipeOfOneThreadDoesNotLeakIntoAnotherCommand)
{
    std::atomic<bool> stop{false};
    std::thread slow([&] {
        std::string out;
        while (!stop)
            hot::run_command({"sh", "-c", "sleep 1"}, out);
    });
    long long worst_ms = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end)
    {
        std::string out;
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(hot::run_command({"true"}, out), 0);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        worst_ms = std::max<long long>(worst_ms, ms);
    }
    stop = true;
    slow.join();
    EXPECT_LT(worst_ms, 500);
}

#endif
