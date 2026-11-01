// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Scratch directories for the tests. CMake defines the macro HOT_SCRATCH_DIR for the test target.
//
// CTest starts the tests as separate processes, and it can run them in parallel. A directory
// for each test and each process stops two tests from writing the same files.
#pragma once

#include "hot/runtime/process.h"
#include "hot/scratch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace test_support
{

// The path HOT_SCRATCH_DIR/<name>/p<pid>. The character '/' in the name becomes '_'.
inline std::string scratch_path(std::string name)
{
    std::replace(name.begin(), name.end(), '/', '_');
    return std::string(HOT_SCRATCH_DIR) + "/" + name + "/p" + std::to_string(hot::detail::process_id());
}

// The scratch directory of the running test. The directory is empty at the start. The
// destructor removes it, unless hot::keep_temp() is true.
class TestScratch : public hot::ScratchDir
{
public:
    TestScratch()
        : hot::ScratchDir(scratch_path(current_test_name()))
    {
    }

private:
    static std::string current_test_name()
    {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        if (!info)
            return "no_test";
        return std::string(info->test_suite_name()) + "." + info->name();
    }
};

// A scratch directory for a cache that lives across the tests of the process. The directory
// exists until the process ends. Each name gives one directory.
inline hot::ScratchDir& process_scratch(const char* name)
{
    static std::map<std::string, std::unique_ptr<hot::ScratchDir>> dirs;
    std::unique_ptr<hot::ScratchDir>& dir = dirs[name];
    if (!dir)
        dir = std::make_unique<hot::ScratchDir>(scratch_path(name));
    return *dir;
}

} // namespace test_support
