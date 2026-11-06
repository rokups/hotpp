// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The process helper of the tests (support/process.h). It starts the program process_probe.
#include "support/process.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using test_support::run_process;

TEST(RunProcess, ExitCodeZero)
{
    const auto r = run_process({PROCESS_PROBE_PATH, "exit", "0"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "");
}

TEST(RunProcess, ExitCodeTwo)
{
    EXPECT_EQ(run_process({PROCESS_PROBE_PATH, "exit", "2"}).exit_code, 2);
}

TEST(RunProcess, OutputMergesStdoutAndStderr)
{
    const auto r = run_process({PROCESS_PROBE_PATH, "both"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.output.find("out-line"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("err-line"), std::string::npos) << r.output;
}

TEST(RunProcess, ArgumentsSurviveUnchanged)
{
    const std::string odd = "a b \"c\" 'd' \\ e\\\\\"";
    const auto r = run_process({PROCESS_PROBE_PATH, "args", "plain", odd, "", "$HOME *"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.output.find("[plain]\n"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("[" + odd + "]\n"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("[]\n"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("[$HOME *]\n"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("args=4"), std::string::npos) << r.output;
}

TEST(RunProcess, MissingProgramGivesMinusOne)
{
    const auto r = run_process({"hot_no_such_program_for_the_test"});
    EXPECT_EQ(r.exit_code, -1);
    EXPECT_FALSE(r.output.empty());
}

TEST(RunProcess, EmptyArgumentListGivesMinusOne)
{
    EXPECT_EQ(run_process({}).exit_code, -1);
}

// The process writes more than the buffer of a pipe. The helper must read while it runs.
TEST(RunProcess, LargeOutputDoesNotBlock)
{
    const size_t bytes = 3u * 1024 * 1024;
    const auto r = run_process({PROCESS_PROBE_PATH, "big", std::to_string(bytes)});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_GE(r.output.size(), bytes);
    EXPECT_LT(r.output.size(), bytes + 4096);
}

} // namespace
