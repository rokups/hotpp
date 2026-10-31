// Reload tests with the backend that HOT_BACKEND selected (make_default_backend).
// They run the F1 reload scenario, a reload with native functions and std containers,
// and the diagnostics of a bad script.
//
// CMake defines the macro HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/f1_scenario.h"
#include "support/native_std_scenario.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

using test_support::expect_errors_at_script;
using test_support::kDt;
using test_support::line_with;
using test_support::Session;
using test_support::source;

TEST_P(BackendTests, DefaultBackendIsTheSelectedOne)
{
    // The environment variable HOT_BACKEND ("jit" or "files") chooses the backend. Else the clang
    // files backend is the default when the build has it.
    const char* env = std::getenv("HOT_BACKEND");
    const std::string choice = env ? env : "";
    std::string expected = hot::backend_available(hot::BackendKind::ClangFiles) ? "CLANG_FILES" : "JIT";
    if (choice == "jit")
        expected = "JIT";
    else if (choice == "files")
        expected = "CLANG_FILES";
    EXPECT_EQ(expected, backend->name());
}

TEST_P(BackendTests, F1ReloadKeepsAddressAndMigratesState)
{
    hot::LoadResult v1 = backend->load(source("player_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("player_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    f1::run_f1_reload(std::move(*v1.module), std::move(*v2.module));
}

// A script calls native functions from Util.cpp and uses std containers. The reload
// migrates a std::string and a std::vector.
TEST_P(BackendTests, NativeFunctionsAndStdContainersMigrate)
{
    hot::LoadResult v1 = backend->load(source("native_std_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("native_std_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    nativestd::run_native_std_reload(std::move(*v1.module), std::move(*v2.module));
}

// A bad script gives an error at the script line. The runtime keeps running.
TEST_P(BackendTests, BadScriptGivesErrorAndRuntimeKeepsRunning)
{
    hot::LoadResult v1 = backend->load(source("player_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module)));
    s.commit_at_frame_boundary();

    const hot::ScriptSource bad = source("bad.hot");
    hot::LoadResult r = backend->load(bad, 1);
    EXPECT_FALSE(r);
    ASSERT_FALSE(r.error.empty());
    expect_errors_at_script(r.error, line_with(bad.text, "\"fast\""));

    // The generation 1 code still runs.
    s.actor->TakeDamage(30.0f);
    s.world.Tick(kDt);
    ASSERT_NE(s.field("hp"), nullptr);
    EXPECT_EQ(*s.field("hp"), 71.0f); // 100 - 30 + 1 (Tick heals 1)
    EXPECT_EQ(hot::active_generation, 1u);

    // The failed build did not damage the slot file: generation 2 still reloads.
    hot::LoadResult v2 = backend->load(source("player_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_NE(s.field("health"), nullptr);
}

INSTANTIATE_TEST_SUITE_P(Selected, BackendTests, ::testing::Values(std::string("default")));

} // namespace
