// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// AOT compilation (S12, design M7), part 1. The loader runs the demo script in this process.
// The AOT program aot_demo runs the same steps in another process. The text must be the
// same. A symbol check shows that aot_demo has no loader and no Clang code.
#include "demo_run.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

namespace
{

std::string run_command(const std::string& command, int* status = nullptr)
{
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, pipe)) > 0)
        out.append(buf, n);
    const int st = pclose(pipe);
    if (status)
        *status = st;
    return out;
}

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
    int status = -1;
    const std::string aot = run_command(std::string(AOT_DEMO_PATH) + " 2>&1", &status);
    EXPECT_EQ(status, 0);
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
    int status = -1, opt_status = -1;
    const std::string aot = run_command(std::string(AOT_DEMO_PATH) + " 2>&1", &status);
    const std::string opt = run_command(std::string(AOT_DEMO_OPT_PATH) + " 2>&1", &opt_status);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(opt_status, 0);
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
    int status = -1, opt_status = -1;
    const std::string aot = run_command(std::string(AOT_COVER_PATH) + " 2>&1", &status);
    const std::string opt = run_command(std::string(AOT_COVER_OPT_PATH) + " 2>&1", &opt_status);
    EXPECT_EQ(status, 0) << aot;
    EXPECT_EQ(opt_status, 0) << opt;
    EXPECT_EQ(opt, aot);
    // Elite::Power = Grunt::Power + 100; Attack uses it; Mix goes to Elite; hp 50 - 3 - 4, and Armor adds 2 to each hit.
    EXPECT_NE(opt.find("Boss seen_power=103 seen_attack=1030 seen_mix=5\n"), std::string::npos) << opt;
    // The class of the ".cpp" script: Power 2 + 100, Attack 1020, one caught error, two ticks.
    EXPECT_NE(opt.find("[aot_cover_cpp] CppBoss power=102 attack=1020 caught=1 ticks=2 label=cpp\n"), std::string::npos)
        << opt;
    EXPECT_NE(opt.find("hit=49 hit=45\n"), std::string::npos) << opt;
    EXPECT_NE(opt.find("guard=20 guard_bad=0 reports=hot::IndexOutOfRange;"), std::string::npos) << opt;
    // "this" as a handle gives the same object. Each of the three "new" that throw is caught (1, 10, 100).
    // A "new" of a class with a native base and a constructor argument works. The objects that the
    // throwing "new" made are destroyed: the constructor body ran for Fussy and Hurt, not for Broken.
    EXPECT_NE(opt.find("Boss same=1 caught=111 tally_n=7 hurt_n=1 hurt_name=hurt hp=43\n"), std::string::npos) << opt;
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
    int status = -1;
    const std::string aot = run_command(std::string(AOT_DEMO_PATH) + " 2>&1", &status);
    EXPECT_EQ(status, 0);
    ASSERT_FALSE(via_loader.empty());
    EXPECT_EQ(aot, via_loader);
}

// The text has the facts that the task names, so an empty or a wrong run cannot pass.
TEST(AotParity, AotProgramShowsTheLanguageFeatures)
{
    const std::string out = run_command(std::string(AOT_DEMO_PATH) + " 2>&1");
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

// The symbols of an executable (nm). Empty when nm does not run.
std::string symbols_of(const std::string& path)
{
    return run_command("nm -C '" + path + "' 2>/dev/null");
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
        GTEST_SKIP() << "nm is not available";
    // The scripts and the AOT runtime are in the binary.
    EXPECT_NE(syms.find("hot::AotRegistry::AotRegistry"), std::string::npos);
    EXPECT_NE(syms.find("Veteran_record"), std::string::npos);
    EXPECT_NE(syms.find("hot::aot_table"), std::string::npos);
    for (const char* mark : kLoaderMarks)
        EXPECT_EQ(syms.find(mark), std::string::npos) << mark;
}

// A control: the same scan finds the loader in this test program, which links it.
TEST(AotLink, ScanFindsTheLoaderInAProgramThatLinksIt)
{
    const std::string syms = symbols_of(std::filesystem::read_symlink("/proc/self/exe").string());
    if (syms.empty())
        GTEST_SKIP() << "nm is not available";
    EXPECT_NE(syms.find("hot::Loader"), std::string::npos);
    EXPECT_NE(syms.find("migrate_block"), std::string::npos);
}

TEST(AotLink, AotProgramNeedsNoClangLibrary)
{
    const std::string needed = run_command(std::string("readelf -d '") + AOT_DEMO_PATH + "' 2>/dev/null | grep NEEDED");
    if (needed.empty())
        GTEST_SKIP() << "readelf is not available";
    EXPECT_EQ(needed.find("clang"), std::string::npos) << needed;
    EXPECT_EQ(needed.find("LLVM"), std::string::npos) << needed;
}

} // namespace
