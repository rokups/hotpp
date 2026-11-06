// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// AOT compilation (S12, design M7), part 1. The loader runs the demo script in this process.
// The AOT program aot_demo runs the same steps in another process. The text must be the
// same. A symbol check shows that aot_demo has no loader and no Clang code.
#include "demo_run.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/process.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// The output of the demo with the loader: the text that the script prints, then the log.
std::string run_with_loader()
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    hot::LoadResult r = backend->load(test_support::source("aot_demo.hot"), 1);
    if (!r)
    {
        ADD_FAILURE() << r.error;
        return {};
    }
    testing::internal::CaptureStdout();
    std::string log;
    {
        hot::Loader loader;
        const hot::ReloadResult loaded = loader.load(std::move(*r.module));
        if (!loaded)
        {
            ADD_FAILURE() << loaded.error;
        }
        else
        {
            log = aot_demo::run([&](const char* name) { return loader.find(name)->create(); });
        }
    }
    std::fflush(stdout);
    return testing::internal::GetCapturedStdout() + log;
}

TEST(AotParity, AotProgramBehavesLikeTheLoader)
{
    const std::string loaded = run_with_loader();
    const test_support::RunResult aot_run = test_support::run_process({AOT_DEMO_PATH});
    const std::string aot = test_support::lf_only(aot_run.output);
    EXPECT_EQ(aot_run.exit_code, 0);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(aot, loaded);
}

// The demo prints the fields of a plain AOT object from its layout. An optimized object has no
// layout. This function cuts each line of the AOT text after "pos.x=<number>".
static std::string without_fields(const std::string& text)
{
    std::string out, line;
    std::istringstream in(text);
    while (std::getline(in, line))
    {
        const size_t at = line.find("pos.x=");
        if (at != std::string::npos)
            line.resize(std::min(line.size(), line.find(' ', at) == std::string::npos ? line.size() : line.find(' ', at)));
        out += line + "\n";
    }
    return out;
}

// The optimized AOT program (MODE AOT OPTIMIZED) prints the same text as the AOT program.
TEST(AotParity, OptimizedProgramBehavesLikeTheAotProgram)
{
    const test_support::RunResult aot_run = test_support::run_process({AOT_DEMO_PATH});
    const test_support::RunResult opt_run = test_support::run_process({AOT_DEMO_OPT_PATH});
    const std::string aot = test_support::lf_only(aot_run.output);
    const std::string opt = test_support::lf_only(opt_run.output);
    EXPECT_EQ(aot_run.exit_code, 0);
    EXPECT_EQ(opt_run.exit_code, 0);
    ASSERT_FALSE(aot.empty());
    // The optimized objects have no field table, so the object lines of the demo have no fields.
    // The script destructors print the fields, so the lines of the two programs are the same.
    EXPECT_EQ(opt, without_fields(aot));
    EXPECT_NE(opt.find("[aot_demo] Fighter dtor 100 ticks=3 level=4 shield=1.000\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("[aot_demo] Veteran dtor 3 parent_power=1\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("[aot_demo] Spawner dtor power=7 attack=70 is_veteran=1\n"), std::string::npos) << opt;
}

// The cover script has a noexcept override, a const virtual, an interface base and a script
// virtual through a base handle. The optimized program must print the same text. The text has
// the facts, so a wrong run in both programs cannot pass.
TEST(AotParity, OptimizedCoverProgramBehavesLikeTheAotProgram)
{
    const test_support::RunResult aot_run = test_support::run_process({AOT_COVER_PATH});
    const test_support::RunResult opt_run = test_support::run_process({AOT_COVER_OPT_PATH});
    const std::string aot = test_support::lf_only(aot_run.output);
    const std::string opt = test_support::lf_only(opt_run.output);
    EXPECT_EQ(aot_run.exit_code, 0) << aot;
    EXPECT_EQ(opt_run.exit_code, 0) << opt;
    EXPECT_EQ(opt, aot);
    // Elite::Power = Grunt::Power + 100; Attack uses it; Mix goes to Elite; hp 50 - 3 - 4, and Armor adds 2 to each hit.
    EXPECT_NE(opt.find("Boss seen_power=103 seen_attack=1030 seen_mix=5\n"), std::string::npos) << opt;
    // The class of the ".cpp" script: Power 2 + 100, Attack 1020, one caught error, two ticks.
    EXPECT_NE(opt.find("[aot_cover_cpp] CppBoss power=102 attack=1020 caught=1 ticks=2 label=cpp\n"), std::string::npos)
        << opt;
    // A std::atomic field, a pointer to an array and a function pointer in the ".cpp" script.
    EXPECT_NE(opt.find("[aot_cover_cpp] CppBoss hits=2 row=5 op=14\n"), std::string::npos) << opt;
    // A constructor and a method with such parameters: seed = 2 * 2, Run = (6 + 1) + seed.
    EXPECT_NE(opt.find("[aot_cover_cpp] CppBoss applied=11 owned=7\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("hit=49 hit=45\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("guard=20 guard_bad=0 reports=hot::IndexOutOfRange;"), std::string::npos) << opt;
    // "this" as a handle gives the same object. Each of the three "new" that throw is caught (1, 10, 100).
    // A "new" of a class with a native base and a constructor argument works. The objects that the
    // throwing "new" made are destroyed: the constructor body ran for Fussy and Hurt, not for Broken.
    EXPECT_NE(opt.find("Boss same=1 caught=111 tally_n=7 hurt_n=1 hurt_name=hurt hp=43\n"), std::string::npos) << opt;
    // "new" with null for a "[weak]" parameter, and a script enum in "${}" (the underlying integer).
    EXPECT_NE(opt.find("[aot_cover] Boss watcher=0 phase=2\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("[aot_cover] Fussy 1 gone\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("[aot_cover] Hurt 2 gone\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("[aot_cover] Tally 7 gone\n"), std::string::npos) << opt;
    // The hooks onReload and convert_level are in the class and never run.
    EXPECT_NE(opt.find("[aot_cover] Hooked level=3.0 reloads=0\n"), std::string::npos) << opt;
    EXPECT_EQ(opt.find("Broken gone"), std::string::npos) << opt;
}

// Loader::load_aot runs the code of the binary under a loader. The text must be the same as
// the text of the AOT program.
TEST(AotParity, LoadAotBehavesLikeTheAotProgram)
{
    testing::internal::CaptureStdout();
    std::string log;
    {
        hot::Loader loader;
        const hot::ReloadResult loaded = loader.load_aot(hot::aot_table);
        if (!loaded)
        {
            ADD_FAILURE() << loaded.error;
        }
        else
        {
            log = aot_demo::run([&](const char* name) { return loader.find(name)->create(); });
        }
    }
    std::fflush(stdout);
    const std::string via_loader = testing::internal::GetCapturedStdout() + log;
    const test_support::RunResult aot_run = test_support::run_process({AOT_DEMO_PATH});
    const std::string aot = test_support::lf_only(aot_run.output);
    EXPECT_EQ(aot_run.exit_code, 0);
    ASSERT_FALSE(via_loader.empty());
    EXPECT_EQ(aot, via_loader);
}

// The text has the facts that the task names, so an empty or a wrong run cannot pass.
TEST(AotParity, AotProgramShowsTheLanguageFeatures)
{
    const std::string out = test_support::lf_only(test_support::run_process({AOT_DEMO_PATH}).output);
    // create and tick: three frames of 0.5 move the Fighter to x=1.5
    EXPECT_NE(out.find("frame 3: Fighter fighter pos.x=1.500 hp=100 ticks=3"), std::string::npos) << out;
    // constructor: Fighter(int) adds 100 to the start value and Veteran passes start + 1 to it
    EXPECT_NE(out.find("Veteran fighter pos.x=0.500 hp=101"), std::string::npos) << out;
    // property: Level starts at 1 and counts the ticks, also in the child through base.Tick
    EXPECT_NE(
        out.find("frame 3: Veteran fighter pos.x=1.500 hp=101 ticks=3 shield_raw=6.000 __p_Level=4"), std::string::npos)
        << out;
    // virtual override: the script call of Power() reaches Veteran, "new Attack" adds one
    EXPECT_NE(out.find("Spawner Spawner pos.x=0.000 kid=? power=7 attack=70 is_veteran=true"), std::string::npos)
        << out;
    // inheritance: base.Power() is the parent implementation
    EXPECT_NE(out.find("parent_power=1"), std::string::npos) << out;
    // native virtual call to the override in the script: the damage goes to the shield
    EXPECT_NE(out.find("hit: Fighter fighter pos.x=1.500 hp=100 ticks=3 shield_raw=1.000"), std::string::npos) << out;
    // destructors: the child first, then the parent
    const size_t child = out.find("[aot_demo] Veteran dtor"), parent = out.find("[aot_demo] Fighter dtor", child);
    EXPECT_NE(child, std::string::npos) << out;
    EXPECT_NE(parent, std::string::npos) << out;
}

// The path of this executable.
std::string self_exe_path()
{
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
    return std::string(buf, n);
#else
    return std::filesystem::read_symlink("/proc/self/exe").string();
#endif
}

#ifdef _WIN32
// The command of an LLVM tool for run_process (Windows). The tool is in HOT_LLVM_TOOLS_DIR when
// the folder is known and has the tool. Else the command is the name with ".exe", and PATH finds it.
std::string llvm_tool(const std::string& name)
{
    #ifdef HOT_LLVM_TOOLS_DIR
    const std::filesystem::path in_dir = std::filesystem::path(HOT_LLVM_TOOLS_DIR) / (name + ".exe");
    std::error_code ec;
    if (std::filesystem::exists(in_dir, ec))
        return in_dir.string();
    #endif
    return name + ".exe";
}
#endif

// A PE image from MSVC has no symbols. They are in the PDB file. MinGW writes a COFF symbol table
// into the image, so llvm-nm reads it. The check text of MSVC has the mangled names of the PDB.
#if defined(_WIN32) && !defined(__MINGW32__)
constexpr bool kMangledSymbols = true;
#else
constexpr bool kMangledSymbols = false;
#endif

// Makes the MSVC mangled form of a qualified name. "a::b::c" gives "c@b@a@". "a::" gives "@a@".
// The MSVC mangling writes the name and then its scopes in the reverse order, each one before "@".
std::string msvc_form(const std::string& mark)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at; (at = mark.find("::", start)) != std::string::npos; start = at + 2)
        parts.push_back(mark.substr(start, at - start));
    const std::string last = mark.substr(start);
    // A constructor name is not repeated in the MSVC mangling ("??0AotRegistry@hot@@").
    if (!parts.empty() && parts.back() == last)
        parts.pop_back();
    std::string out = last.empty() ? std::string("@") : last + "@";
    for (size_t i = parts.size(); i-- > 0;)
        out += parts[i] + "@";
    return out;
}

// True when the symbol text has the mark. For the PDB text of MSVC the mangled form of a qualified
// name counts too. A plain name (no "::") is in a mangled name as text, so the mark itself works.
bool has_symbol(const std::string& syms, const std::string& mark)
{
    if (syms.find(mark) != std::string::npos)
        return true;
    return kMangledSymbols && mark.find("::") != std::string::npos && syms.find(msvc_form(mark)) != std::string::npos;
}

// The symbols of an executable as text. Empty when the tool does not run or fails.
// Linux: nm -C. MinGW: llvm-nm -C (the COFF symbol table). MSVC: the public symbols of the PDB
// file next to the executable (llvm-pdbutil dump --publics). The MSVC branch is untested.
std::string symbols_of(const std::string& path)
{
#if defined(_WIN32) && !defined(__MINGW32__)
    std::filesystem::path pdb = path;
    pdb.replace_extension(".pdb");
    std::error_code ec;
    if (!std::filesystem::exists(pdb, ec))
        return {};
    const test_support::RunResult r =
        test_support::run_process({llvm_tool("llvm-pdbutil"), "dump", "--publics", pdb.string()});
#elif defined(_WIN32)
    const test_support::RunResult r = test_support::run_process({llvm_tool("llvm-nm"), "-C", path});
#else
    const test_support::RunResult r = test_support::run_process({"nm", "-C", path});
#endif
    return r.exit_code == 0 ? test_support::lf_only(r.output) : std::string();
}

// The sanitizer runtime of the compiler has dlopen and the LLVM symbolizer. A sanitizer build
// does not check these two names.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    #define HOT_TEST_SANITIZER 1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
        #define HOT_TEST_SANITIZER 1
    #endif
#endif
#ifdef HOT_TEST_SANITIZER
const char* const kLoaderMarks[] = {"hot::Loader", "hot::module_from_library", "migrate_block", "ClangFilesBackend",
    "FileWatcher", "removed_class_handler", "hotc::translate", "clang_createIndex", "clang_parseTranslationUnit",
    "clang::"};
#else
const char* const kLoaderMarks[] = {"hot::Loader", "hot::module_from_library", "migrate_block", "ClangFilesBackend",
    "FileWatcher", "removed_class_handler", "hotc::translate", "dlopen", "clang_createIndex",
    "clang_parseTranslationUnit", "clang::", "LLVM", "llvm::"};
#endif

TEST(AotLink, AotProgramHasNoLoaderOrClangSymbol)
{
    const std::string syms = symbols_of(AOT_DEMO_PATH);
    if (syms.empty())
        GTEST_SKIP() << "the symbol tool (nm, llvm-nm or llvm-pdbutil with a PDB file) is not available";
    // The scripts and the AOT runtime are in the binary.
    EXPECT_TRUE(has_symbol(syms, "hot::AotRegistry::AotRegistry"));
    EXPECT_TRUE(has_symbol(syms, "Veteran_record"));
    EXPECT_TRUE(has_symbol(syms, "hot::aot_table"));
    for (const char* mark : kLoaderMarks)
        EXPECT_FALSE(has_symbol(syms, mark)) << mark;
}

// A control: the same scan finds the loader in this test program, which links it.
TEST(AotLink, ScanFindsTheLoaderInAProgramThatLinksIt)
{
    const std::string syms = symbols_of(self_exe_path());
    if (syms.empty())
        GTEST_SKIP() << "the symbol tool (nm, llvm-nm or llvm-pdbutil with a PDB file) is not available";
    EXPECT_TRUE(has_symbol(syms, "hot::Loader"));
    EXPECT_TRUE(has_symbol(syms, "migrate_block"));
}

TEST(AotLink, AotProgramNeedsNoClangLibrary)
{
    // The libraries that the program needs, one on each line. Empty when the tool does not run or fails.
    // Linux: the NEEDED lines of "readelf -d". Windows: the "Name:" lines of the imports
    // ("llvm-readobj --coff-imports"), for MinGW and for MSVC programs.
    std::string needed;
#ifdef _WIN32
    const test_support::RunResult imports =
        test_support::run_process({llvm_tool("llvm-readobj"), "--coff-imports", AOT_DEMO_PATH});
    if (imports.exit_code == 0)
    {
        std::istringstream lines(test_support::lf_only(imports.output));
        for (std::string line; std::getline(lines, line);)
            if (line.find("Name:") != std::string::npos)
                needed += line + "\n";
        // A program always imports a system library, so a tool output without one is a wrong output.
        ASSERT_NE(needed.find(".dll"), std::string::npos) << imports.output;
    }
    // The names of the DLL files have any case: the check uses the lower case.
    std::string checked = needed;
    std::transform(checked.begin(), checked.end(), checked.begin(), [](unsigned char c) { return std::tolower(c); });
    if (needed.empty())
        GTEST_SKIP() << "llvm-readobj is not available: " << imports.output;
    EXPECT_EQ(checked.find("clang"), std::string::npos) << needed;
    EXPECT_EQ(checked.find("llvm"), std::string::npos) << needed;
#else
    const test_support::RunResult dynamic = test_support::run_process({"readelf", "-d", AOT_DEMO_PATH});
    if (dynamic.exit_code == 0)
    {
        std::istringstream lines(dynamic.output);
        for (std::string line; std::getline(lines, line);)
            if (line.find("NEEDED") != std::string::npos)
                needed += line + "\n";
    }
    if (needed.empty())
        GTEST_SKIP() << "readelf is not available";
    EXPECT_EQ(needed.find("clang"), std::string::npos) << needed;
    EXPECT_EQ(needed.find("LLVM"), std::string::npos) << needed;
#endif
}

} // namespace
