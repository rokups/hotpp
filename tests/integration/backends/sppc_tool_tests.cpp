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
    const std::string script = std::string(SPP_SOURCE_DIR) + "/example/scripts/bad.spp";
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

namespace {

// Runs a shell command. Gives the exit code and the output.
int run_command(const std::string& cmd, std::string& text)
{
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) return -1;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string player_args()
{
    return std::string("'") + SPP_SOURCE_DIR + "/example/scripts/player_v1.spp' --gen 1 -I '" +
           SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR + "/include'";
}

// sppc --emit-cpp prints the C++ to the standard output, or writes it with -o.
TEST(SppcTool, EmitCppModeTranslatesWithoutCompiling)
{
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp " + player_args() + " 2>&1", text), 0)
        << text;
    EXPECT_NE(text.find("spp::deref"), std::string::npos);

    const std::string out = std::string(SPP_BINARY_DIR) + "/sppc_emit.cpp";
    std::filesystem::remove(out);
    std::string text2;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp " + player_args() + " -o '" + out +
                              "' 2>&1",
                          text2),
              0)
        << text2;
    EXPECT_NE(read_file(out).find("spp::deref"), std::string::npos);  // the #line names differ
}

// sppc -o x.so builds one library. A mode flag of the other mode gives a usage error.
TEST(SppcTool, OutputOptionBuildsOneLibrary)
{
    const std::string dir = std::string(SPP_BINARY_DIR) + "/sppc_one_so";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' " + player_args() + " -o '" + dir +
                              "/x.so' --cxx '" + SPP_CXX + "' 2>&1",
                          text),
              0)
        << text;
    EXPECT_TRUE(std::filesystem::exists(dir + "/x.so"));

    std::string text2;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp " + player_args() + " --out-dir '" + dir +
                              "' 2>&1",
                          text2),
              2);
}

}  // namespace
