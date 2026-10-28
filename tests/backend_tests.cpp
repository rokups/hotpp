// Tests for the script backend and for scripts that sppc compiled ahead of time.
//
// BackendTests use the backend that SPP_BACKEND selected (make_default_backend).
// They run the F1 reload scenario and check the diagnostics of a bad script.
// PrecompiledTests load .so files that sppc made at build time (spp_add_scripts)
// as a Module with module_from_library. They run for every SPP_BACKEND value.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_BINARY_DIR, SPP_BACKEND_NAME,
// SPPC_PATH, SPP_CXX, PRE_V1_SO, PRE_V2_SO, PRE_NATIVE_V1_SO and PRE_NATIVE_V2_SO.
#include <gtest/gtest.h>

#include <sys/wait.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "spp/backend.h"
#include "f1_scenario.h"
#include "native_std_scenario.h"

namespace {

using test_support::kDt;
using test_support::Session;

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

script::ScriptSource source(const char* file)
{
    const std::string path = std::string(SPP_SOURCE_DIR) + "/example/scripts/" + file;
    return {path, read_file(path)};
}

// The line number of the first line that has the text.
int line_with(const std::string& text, const std::string& needle)
{
    int n = 0;
    std::istringstream src(text);
    for (std::string l; std::getline(src, l);)
        if (++n, l.find(needle) != std::string::npos) return n;
    return 0;
}

// Each "error:" line must name the script, not a generated file.
void expect_errors_at_script(const std::string& text, int bad_line)
{
    const std::string expect = "bad.script:" + std::to_string(bad_line) + ":";
    EXPECT_NE(text.find(expect), std::string::npos) << "no '" << expect << "' in:\n" << text;
    std::istringstream ls(text);
    int errors = 0;
    for (std::string l; std::getline(ls, l);) {
        if (l.find(" error:") == std::string::npos) continue;
        ++errors;
        EXPECT_NE(l.find("bad.script:"), std::string::npos) << "error not at the script: " << l;
        EXPECT_EQ(l.find(".cpp"), std::string::npos) << l;
    }
    EXPECT_GT(errors, 0) << text;
}

class BackendTests : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override { backend = script::make_default_backend(); }
    std::unique_ptr<script::Backend> backend;
};

TEST_P(BackendTests, DefaultBackendIsTheSelectedOne)
{
    EXPECT_EQ(GetParam(), backend->name());
}

TEST_P(BackendTests, F1ReloadKeepsAddressAndMigratesState)
{
    script::LoadResult v1 = backend->load(source("player_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    script::LoadResult v2 = backend->load(source("player_v2.script"), 2);
    ASSERT_TRUE(v2) << v2.error;
    f1::run_f1_reload(std::move(*v1.module), std::move(*v2.module));
}

// A script calls native functions from Util.cpp and uses std containers. The reload
// migrates a std::string and a std::vector.
TEST_P(BackendTests, NativeFunctionsAndStdContainersMigrate)
{
    script::LoadResult v1 = backend->load(source("native_std_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    script::LoadResult v2 = backend->load(source("native_std_v2.script"), 2);
    ASSERT_TRUE(v2) << v2.error;
    nativestd::run_native_std_reload(std::move(*v1.module), std::move(*v2.module));
}

// A bad script gives an error at the script line. The runtime keeps running.
TEST_P(BackendTests, BadScriptGivesErrorAndRuntimeKeepsRunning)
{
    script::LoadResult v1 = backend->load(source("player_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module)));
    s.commit_at_frame_boundary();

    const script::ScriptSource bad = source("bad.script");
    script::LoadResult r = backend->load(bad, 1);
    EXPECT_FALSE(r);
    ASSERT_FALSE(r.error.empty());
    expect_errors_at_script(r.error, line_with(bad.text, "\"fast\""));

    // The generation 1 code still runs.
    s.actor->TakeDamage(30.0f);
    s.world.Tick(kDt);
    ASSERT_NE(s.field("hp"), nullptr);
    EXPECT_EQ(*s.field("hp"), 71.0f);  // 100 - 30 + 1 (Tick heals 1)
    EXPECT_EQ(script::active_generation, 1u);

    // The failed build did not damage the slot file: generation 2 still reloads.
    script::LoadResult v2 = backend->load(source("player_v2.script"), 2);
    ASSERT_TRUE(v2) << v2.error;
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(std::move(*v2.module), error)) << error;
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(script::active_generation, 2u);
    EXPECT_NE(s.field("health"), nullptr);
}

// A member of a temporary in a for-of loop: "this.mk().first". The temporary must live
// during the whole loop. The generated code compiles and runs with the selected backend.
TEST_P(BackendTests, ForOfOverAMemberOfATemporaryRuns)
{
    const script::ScriptSource src = {"temp.script", R"(
import "Actor.h";
import <utility>;
import <vector>;

class Player : Actor {
    total: float = 0.0;

    function mk() -> std.pair<std.vector<int>, int> {
        let p = std.pair<std.vector<int>, int>();
        p.first.push_back(1);
        p.first.push_back(2);
        p.first.push_back(3);
        return p;
    }

    function Tick(dt: float) {
        for (let a of this.mk().first) {
            this.total = this.total + a;
        }
    }

    function TakeDamage(amount: float) {}
    function TakeDamage(source: Actor&, amount: float) {}
}
)"};
    script::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    ASSERT_NE(s.field("total"), nullptr);
    EXPECT_EQ(*s.field("total"), 6.0f);
}

// The statements throw, try and catch translate to C++ that compiles and runs.
TEST_P(BackendTests, TryCatchThrowRun)
{
    const script::ScriptSource src = {"exc.script", R"(
import "Actor.h";
import <stdexcept>;

class Player : Actor {
    total: float = 0.0;

    function Tick(dt: float) {
        try {
            throw std.runtime_error("boom");
        } catch (e: const std.exception&) {
            let w = e.what();
            if (w[0] == 'b') {
                this.total = this.total + 1.0;
            }
        }
        try {
            try {
                throw 3;
            } catch (...) {
                throw;
            }
        } catch (v: double) {
            this.total = 100.0;
        } catch (v: int) {
            this.total = this.total + v;
        }
    }

    function TakeDamage(amount: float) {}
    function TakeDamage(source: Actor&, amount: float) {}
}
)"};
    script::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    ASSERT_NE(s.field("total"), nullptr);
    EXPECT_EQ(*s.field("total"), 4.0f);
}

INSTANTIATE_TEST_SUITE_P(Selected, BackendTests, ::testing::Values(std::string(SPP_BACKEND_NAME)));

// ---------------------------------------------------------------- precompiled

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

TEST(PrecompiledTests, MissingLibraryGivesErrorString)
{
    script::LoadResult r = script::module_from_library(std::string(SPP_BINARY_DIR) + "/none.so");
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.error.empty());
}

// sppc on a bad script: exit code 1, an error at the script line, no library.
TEST(PrecompiledTests, SppcReportsErrorAtTheScriptLine)
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
