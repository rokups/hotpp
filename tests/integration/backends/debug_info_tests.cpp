// Experiment F3 (design 4.10, 4.12): the debug info names the .hot files.
//
// The translator writes "#line" directives. Clang copies them into the DWARF line table. These
// tests read the table with llvm-dwarfdump. They skip when the tool is not installed.
// The debugger itself (lldb, gdb) is not started here.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_SCRATCH_DIR, SPP_CXX and PRE_V2_SO.
#include "spp/sppc_lib.h"
#include "support/dwarf_lines.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using test_support::have_dwarfdump;

// The line of the method heal in player_v2.hot that calls Math.Min.
int heal_line()
{
    return test_support::line_with(
        test_support::read_file(std::string(SPP_SOURCE_DIR) + "/tests/scripts/player_v2.hot"), "Math.Min");
}

// A script that the test translated and compiled with "-g" and a given optimization flag.
struct Compiled
{
    std::string script_file; // the base name of the script, for example "player_v1.hot"
    std::string info; // llvm-dwarfdump --debug-info
    std::string lines; // llvm-dwarfdump --debug-line
    std::vector<test_support::DwarfFunction> functions;
};

// Translates the script (generation 1) and compiles it to a shared library with full debug info.
// The result stays in a cache: a test does not compile the same script twice.
// The library has final addresses. The object file has one address range for each section.
const Compiled& compile_with_debug_info(const std::string& script, const std::string& opt)
{
    static std::map<std::string, Compiled> cache;
    const std::string key = script + opt;
    auto found = cache.find(key);
    if (found != cache.end())
        return found->second;
    // CTest starts the tests in parallel as separate processes. A directory for each process
    // stops them from writing the same files. The directory is removed when the process ends.
    const std::string dir = test_support::process_scratch("debug_info").path();
    sppc::TranslateOptions to;
    to.script_name = script + ".hot";
    to.script_text = test_support::read_file(std::string(SPP_SOURCE_DIR) + "/tests/scripts/" + script + ".hot");
    to.output_name = dir + "/" + script + ".cpp";
    to.generation = 1;
    to.include_dirs = {std::string(SPP_SOURCE_DIR) + "/include", std::string(SPP_SOURCE_DIR) + "/tests/engine"};
    const sppc::TranslateResult tr = sppc::translate(to);
    Compiled c;
    c.script_file = to.script_name;
    EXPECT_TRUE(tr.ok) << tr.diagnostics;
    if (!tr.ok)
        return cache[key] = c;
    {
        std::ofstream(to.output_name) << tr.cpp;
    }
    const std::string library = dir + "/" + script + opt + ".so";
    fs::remove(library);
    const std::string cmd = std::string("'") + SPP_CXX + "' -std=c++20 " + opt + " -fPIC -shared -g -I '"
        + SPP_SOURCE_DIR + "/include' -I '" + SPP_SOURCE_DIR + "/tests/engine' '" + to.output_name + "' -o '"
        + library + "'";
    const std::string compile_output = test_support::run_capture(cmd);
    EXPECT_TRUE(fs::exists(library)) << compile_output;
    if (!fs::exists(library))
        return cache[key] = c;
    c.info = test_support::dump_debug_info(library);
    c.lines = test_support::dump_line_table(library);
    c.functions = test_support::functions_of(c.info);
    return cache[key] = c;
}

const test_support::DwarfFunction* find_by_linkage(const Compiled& c, const std::string& linkage)
{
    for (const auto& f : c.functions)
        if (f.linkage == linkage && !f.declaration && f.has_code)
            return &f;
    return nullptr;
}

const test_support::DwarfFunction* find_by_name(const Compiled& c, const std::string& name)
{
    for (const auto& f : c.functions)
        if (f.name == name && !f.declaration && f.has_code)
            return &f;
    return nullptr;
}

// The functions with code that the script defines: the file of the declaration is the script.
std::vector<const test_support::DwarfFunction*> script_functions(const Compiled& c)
{
    std::vector<const test_support::DwarfFunction*> out;
    for (const auto& f : c.functions)
        if (!f.declaration && f.has_code && test_support::file_has_base_name(f.decl_file, c.script_file))
            out.push_back(&f);
    return out;
}

const std::vector<std::string> kOptimizations = {"-O0", "-O2"};

// The library that sppc built ahead of time (full debug info, "-g").
TEST(DebugInfo, LibraryFromSppcHasLinesOfTheScript)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    const std::set<int> lines = test_support::lines_of_file(test_support::dump_line_table(PRE_V2_SO), "player_v2.hot");
    EXPECT_FALSE(lines.empty());
    EXPECT_TRUE(lines.count(heal_line())) << "no row for line " << heal_line();
}

// The flags of the JIT backend: "-O1 -gline-tables-only". The object has the script lines.
TEST(DebugInfo, LineTablesOnlyKeepsTheScriptLines)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    test_support::TestScratch scratch;
    const std::string dir = scratch.path();
    sppc::TranslateOptions to;
    to.script_name = "player_v2.hot";
    to.script_text = test_support::read_file(std::string(SPP_SOURCE_DIR) + "/tests/scripts/player_v2.hot");
    to.output_name = dir + "/player_v2.cpp";
    to.generation = 1;
    to.include_dirs = {std::string(SPP_SOURCE_DIR) + "/include", std::string(SPP_SOURCE_DIR) + "/tests/engine"};
    const sppc::TranslateResult tr = sppc::translate(to);
    ASSERT_TRUE(tr.ok) << tr.diagnostics;
    {
        std::ofstream(to.output_name) << tr.cpp;
    }
    const std::string object = dir + "/player_v2.o";
    const std::string cmd = std::string("'") + SPP_CXX + "' -std=c++20 -O1 -fPIC -gline-tables-only -I '"
        + SPP_SOURCE_DIR + "/include' -I '" + SPP_SOURCE_DIR + "/tests/engine' -c '" + to.output_name + "' -o '"
        + object + "'";
    const std::string compile_output = test_support::run_capture(cmd);
    ASSERT_TRUE(fs::exists(object)) << compile_output;
    const std::string dump = test_support::dump_line_table(object);
    const std::set<int> lines = test_support::lines_of_file(dump, "player_v2.hot");
    EXPECT_TRUE(lines.count(heal_line())) << "no row for line " << heal_line();
    EXPECT_GT(lines.size(), 10u);
}

// The state function of Player.Tick. The shape is "void PlayerState_g1::Tick(float dt)". The locals
// "self" and "st" are [[gnu::nodebug]]. Only "this" and "dt" stay.
TEST(DebugInfo, MemberFunctionHasOnlyThisAndItsParameters)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    for (const std::string& opt : kOptimizations)
    {
        SCOPED_TRACE(opt);
        const Compiled& c = compile_with_debug_info("player_v1", opt);
        const auto* tick = find_by_linkage(c, "_ZN14PlayerState_g14TickEf");
        ASSERT_NE(tick, nullptr) << "no code for PlayerState_g1::Tick";
        EXPECT_EQ(tick->all_names(), (std::vector<std::string>{"this", "dt"}));
        EXPECT_TRUE(tick->names("DW_TAG_variable").empty());
        // Another member has a local variable of the script. It must not show "self" or "st" either.
        const auto* heal = find_by_linkage(c, "_ZN14PlayerState_g14healEf");
        if (heal != nullptr)
        {
            const auto names = heal->all_names();
            EXPECT_EQ(std::count(names.begin(), names.end(), "self"), 0);
            EXPECT_EQ(std::count(names.begin(), names.end(), "st"), 0);
        }
    }
}

// The slot is the state member itself, so no wrapper frame exists for a slot call. The member
// "PlayerState_g1::Tick" has "this" and "dt" and no "a". No subprogram "Player__Tick" has code.
TEST(DebugInfo, SlotIsTheStateMemberWithoutWrapper)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    for (const std::string& opt : kOptimizations)
    {
        SCOPED_TRACE(opt);
        const Compiled& c = compile_with_debug_info("player_v1", opt);
        const auto* tick = find_by_linkage(c, "_ZN14PlayerState_g14TickEf");
        ASSERT_NE(tick, nullptr) << "no code for PlayerState_g1::Tick";
        const auto names = tick->all_names();
        EXPECT_EQ(tick->names("DW_TAG_formal_parameter"), (std::vector<std::string>{"this", "dt"}));
        EXPECT_EQ(std::count(names.begin(), names.end(), "a"), 0);
        EXPECT_EQ(find_by_name(c, "Player__Tick"), nullptr) << "a wrapper with code exists";
    }
}

// The line table of the state member has the row of the "fn" line first and then a row of the
// first body line. So a breakpoint on the first body line does not resolve to the "fn" line.
TEST(DebugInfo, MemberLineTableHasFirstBodyLineAfterFnLine)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    const std::string text = test_support::read_file(std::string(SPP_SOURCE_DIR) + "/tests/scripts/player_v1.hot");
    const int fn_line = test_support::line_with(text, "override void Tick");
    const int body_line = test_support::line_with(text, "this.Move");
    ASSERT_EQ(body_line, fn_line + 1);
    for (const std::string& opt : kOptimizations)
    {
        SCOPED_TRACE(opt);
        const Compiled& c = compile_with_debug_info("player_v1", opt);
        const auto* tick = find_by_linkage(c, "_ZN14PlayerState_g14TickEf");
        ASSERT_NE(tick, nullptr) << "no code for PlayerState_g1::Tick";
        std::vector<int> lines;
        for (const auto& r : test_support::line_rows(c.lines))
        {
            if (r.address < tick->low_pc || r.address >= tick->high_pc)
                continue;
            if (!test_support::file_has_base_name(r.file, c.script_file))
                continue;
            if (r.line == 0)
                continue;
            lines.push_back(r.line);
        }
        ASSERT_FALSE(lines.empty());
        // The entry row is the "fn" line. The row of the first body line comes after it.
        EXPECT_EQ(lines.front(), fn_line);
        const auto body = std::find(lines.begin(), lines.end(), body_line);
        ASSERT_NE(body, lines.end()) << "no row for the first body line";
        EXPECT_LT(std::find(lines.begin(), lines.end(), fn_line), body);
    }
}

// The scripts for the next tests. The first has the shell and the wrapper. The second has an
// intrusive class. The third has a constructor with a reference argument.
const std::vector<std::string> kScripts = {"player_v1", "intrusive_v1", "ctor_ref_v1"};

// The runtime helpers are [[gnu::nodebug]]: the debug info has no subprogram for them.
TEST(DebugInfo, RuntimeHelpersHaveNoSubprogram)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    const std::vector<std::string> helpers = {"deref", "shell_of", "Intrusive", "Closure", "ScriptEntry", "stale_call"};
    for (const std::string& script : kScripts)
    {
        for (const std::string& opt : kOptimizations)
        {
            SCOPED_TRACE(script + " " + opt);
            const Compiled& c = compile_with_debug_info(script, opt);
            ASSERT_FALSE(script_functions(c).empty()) << "no script function in the debug info";
            for (const auto& f : c.functions)
            {
                for (const std::string& h : helpers)
                {
                    // A function template has the arguments in the name: "deref<int>".
                    const bool same = f.name == h || f.name.rfind(h + "<", 0) == 0;
                    EXPECT_FALSE(same) << "subprogram for the runtime helper " << f.name;
                }
            }
        }
    }
}

// Every line of generated code that does something maps to the .hot file. The functions of the
// script (shell, wrapper, member, slot, thunk, build, construct, create) have no row of the
// generated files "x.hot.cpp" and "x.cpp". The module code ("script_register") is not checked.
TEST(DebugInfo, ScriptFunctionsHaveNoRowsOfTheGeneratedFile)
{
    if (!have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    for (const std::string& script : kScripts)
    {
        for (const std::string& opt : kOptimizations)
        {
            SCOPED_TRACE(script + " " + opt);
            const Compiled& c = compile_with_debug_info(script, opt);
            const auto functions = script_functions(c);
            ASSERT_FALSE(functions.empty());
            const auto rows = test_support::line_rows(c.lines);
            size_t rows_in_functions = 0;
            for (const auto* f : functions)
            {
                for (const auto& r : rows)
                {
                    if (r.address < f->low_pc || r.address >= f->high_pc)
                        continue;
                    ++rows_in_functions;
                    EXPECT_FALSE(test_support::file_has_base_name(r.file, c.script_file + ".cpp"))
                        << f->name << f->linkage << " has a row of " << r.file << ":" << r.line;
                    EXPECT_FALSE(test_support::file_has_base_name(r.file, script + ".cpp"))
                        << f->name << f->linkage << " has a row of " << r.file << ":" << r.line;
                }
            }
            EXPECT_GT(rows_in_functions, 0u) << "the check did not see a row";
        }
    }
}

} // namespace
