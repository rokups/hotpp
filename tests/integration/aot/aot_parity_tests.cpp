// AOT compilation (S12, design M7), part 1. The loader runs the demo script in this process.
// The AOT program aot_demo runs the same steps in another process. The text must be the
// same. A symbol check shows that aot_demo has no loader and no Clang code.
#include "demo_run.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
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
    #define SPP_TEST_SANITIZER 1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
        #define SPP_TEST_SANITIZER 1
    #endif
#endif
#ifdef SPP_TEST_SANITIZER
const char* const kLoaderMarks[] = {"hot::Loader", "hot::module_from_library", "migrate_block", "ClangFilesBackend",
    "FileWatcher", "removed_class_handler", "sppc::translate", "clang_createIndex", "clang_parseTranslationUnit",
    "clang::"};
#else
const char* const kLoaderMarks[] = {"hot::Loader", "hot::module_from_library", "migrate_block", "ClangFilesBackend",
    "FileWatcher", "removed_class_handler", "sppc::translate", "dlopen", "clang_createIndex",
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
