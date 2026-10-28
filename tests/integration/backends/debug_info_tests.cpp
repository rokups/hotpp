// Experiment F3 (design 4.10, 4.12): the debug info names the .spp files.
//
// The translator writes "#line" directives. Clang copies them into the DWARF line table. These
// tests read the table with llvm-dwarfdump. They skip when the tool is not installed.
// The debugger itself (lldb, gdb) is not started here.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_BINARY_DIR, SPP_CXX and PRE_V2_SO.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "spp/sppc_lib.h"
#include "support/dwarf_lines.h"
#include "support/script_files.h"

namespace {

namespace fs = std::filesystem;
using test_support::have_dwarfdump;

// The line of the method heal in player_v2.spp that calls Math.Min.
int heal_line()
{
    return test_support::line_with(test_support::read_file(std::string(SPP_SOURCE_DIR) + "/example/scripts/player_v2.spp"),
                                   "Math.Min");
}

// The library that sppc built ahead of time (full debug info, "-g").
TEST(DebugInfo, LibraryFromSppcHasLinesOfTheScript)
{
    if (!have_dwarfdump()) GTEST_SKIP() << "llvm-dwarfdump is not installed";
    const std::set<int> lines = test_support::lines_of_file(test_support::dump_line_table(PRE_V2_SO), "player_v2.spp");
    EXPECT_FALSE(lines.empty());
    EXPECT_TRUE(lines.count(heal_line())) << "no row for line " << heal_line();
}

// The flags of the JIT backend: "-O1 -gline-tables-only". The object has the script lines.
TEST(DebugInfo, LineTablesOnlyKeepsTheScriptLines)
{
    if (!have_dwarfdump()) GTEST_SKIP() << "llvm-dwarfdump is not installed";
    const std::string dir = std::string(SPP_BINARY_DIR) + "/debug_info";
    fs::create_directories(dir);
    sppc::TranslateOptions to;
    to.script_name = "player_v2.spp";
    to.script_text = test_support::read_file(std::string(SPP_SOURCE_DIR) + "/example/scripts/player_v2.spp");
    to.output_name = dir + "/player_v2.cpp";
    to.generation = 1;
    to.include_dirs = {std::string(SPP_SOURCE_DIR) + "/include", std::string(SPP_SOURCE_DIR) + "/example/engine"};
    const sppc::TranslateResult tr = sppc::translate(to);
    ASSERT_TRUE(tr.ok) << tr.diagnostics;
    {
        std::ofstream(to.output_name) << tr.cpp;
    }
    const std::string object = dir + "/player_v2.o";
    const std::string cmd = std::string("'") + SPP_CXX + "' -std=c++20 -O1 -fPIC -gline-tables-only -I '" + SPP_SOURCE_DIR +
                            "/include' -I '" + SPP_SOURCE_DIR + "/example/engine' -c '" + to.output_name + "' -o '" + object + "'";
    const std::string compile_output = test_support::run_capture(cmd);
    ASSERT_TRUE(fs::exists(object)) << compile_output;
    const std::string dump = test_support::dump_line_table(object);
    const std::set<int> lines = test_support::lines_of_file(dump, "player_v2.spp");
    EXPECT_TRUE(lines.count(heal_line())) << "no row for line " << heal_line();
    EXPECT_GT(lines.size(), 10u);
}

}  // namespace
