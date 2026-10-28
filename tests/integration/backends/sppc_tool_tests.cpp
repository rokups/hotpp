// Tests of the tool sppc as a child process.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_BINARY_DIR, SPPC_PATH and SPP_CXX.
#include <gtest/gtest.h>

#include <sys/wait.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "support/script_files.h"

namespace {

using test_support::expect_errors_at_script;
using test_support::line_with;
using test_support::read_file;

// sppc on a bad script: exit code 1, an error at the script line, no library.
TEST(SppcTool, SppcReportsErrorAtTheScriptLine)
{
    const std::string out_dir = std::string(SPP_BINARY_DIR) + "/sppc_diag";
    std::filesystem::remove_all(out_dir);
    const std::string script = std::string(SPP_SOURCE_DIR) + "/example/scripts/bad.script";
    const std::string cmd = std::string("'") + SPPC_PATH + "' '" + script + "' --gen 1 --out-dir '" +
                            out_dir + "' -I '" + SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR +
                            "/include' --cxx '" + SPP_CXX + "' 2>&1";
    std::string text;
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    const int status = pclose(p);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1) << text;
    expect_errors_at_script(text, line_with(read_file(script), "\"fast\""));
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/bad.g1.so"));
}

}  // namespace
