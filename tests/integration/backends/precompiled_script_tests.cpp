// Tests of scripts that sppc compiled ahead of time.
//
// PrecompiledTests load .so files that sppc made at build time (spp_add_scripts)
// as a Module with module_from_library. They run for every SPP_BACKEND value.
//
// CMake defines these macros: SPP_SCRATCH_DIR, PRE_V1_SO, PRE_V2_SO, PRE_NATIVE_V1_SO,
// PRE_NATIVE_V2_SO, PRE_HANDLE_V1_SO, PRE_HANDLE_V2_SO and PRE_HANDLE_V1B_SO.
#include <gtest/gtest.h>

#include <string>

#include "spp/backend.h"
#include "support/f1_scenario.h"
#include "support/handle_reload_scenario.h"
#include "support/native_std_scenario.h"
#include "support/test_support.h"

namespace {

// The libraries come from spp_add_scripts, which calls sppc.
TEST(PrecompiledTests, F1ReloadWithModulesFromLibraries)
{
    f1::run_f1_reload(test_support::library(PRE_V1_SO), test_support::library(PRE_V2_SO));
}

TEST(PrecompiledTests, NativeFunctionsAndStdContainersWithLibraries)
{
    nativestd::run_native_std_reload(test_support::library(PRE_NATIVE_V1_SO),
                                     test_support::library(PRE_NATIVE_V2_SO));
}

TEST(PrecompiledTests, HandleFieldSurvivesReloadWithLibraries)
{
    handlereload::run_handle_reload(test_support::library(PRE_HANDLE_V1_SO), test_support::library(PRE_HANDLE_V2_SO),
                      test_support::library(PRE_HANDLE_V1B_SO));
}

TEST(PrecompiledTests, MissingLibraryGivesErrorString)
{
    spp::LoadResult r = spp::module_from_library(std::string(SPP_SCRATCH_DIR) + "/none.so");
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.error.empty());
}


}  // namespace
