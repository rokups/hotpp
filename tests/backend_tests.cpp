// Tests for the script backend and for scripts that sppc compiled ahead of time.
//
// BackendTests use the backend that SPP_BACKEND selected (make_default_backend).
// They run the F1 reload scenario and check the diagnostics of a bad script.
// PrecompiledTests load .so files that sppc made at build time (spp_add_scripts)
// as a Module with module_from_library. They run for every SPP_BACKEND value.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_BINARY_DIR, SPP_BACKEND_NAME,
// SPPC_PATH, SPP_CXX, PRE_V1_SO, PRE_V2_SO, PRE_NATIVE_V1_SO and PRE_NATIVE_V2_SO and the PRE_HANDLE_*_SO paths.
#include <gtest/gtest.h>

#include <sys/wait.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "spp/backend.h"
#include "Object.h"
#include "Weapon.h"
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

// A handle field (std::shared_ptr<Weapon>) must keep its object across reloads.
void run_handle_reload(script::Module v1, script::Module v2, script::Module v1_again)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(v1), "Knight"));
    s.commit_at_frame_boundary();
    auto handle = [&] { return script::field_ptr<std::shared_ptr<Weapon>>(s.inst.shell, "weapon"); };

    s.world.Tick(kDt);
    ASSERT_NE(handle(), nullptr);
    ASSERT_NE(handle()->get(), nullptr);
    const Weapon* raw = handle()->get();
    EXPECT_EQ(handle()->use_count(), 1);
    EXPECT_EQ((*handle())->dmg, 7);
    EXPECT_EQ((*handle())->name, "sword");

    // The reload commits after the next Tick, which is still generation 1.
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(std::move(v2), error)) << error;
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(script::active_generation, 2u);
    ASSERT_NE(handle(), nullptr);
    EXPECT_EQ(handle()->get(), raw);
    EXPECT_EQ(handle()->use_count(), 1);
    EXPECT_EQ((*handle())->dmg, 7);
    ASSERT_NE(s.field("rage"), nullptr);
    EXPECT_FLOAT_EQ(*s.field("rage"), 1.0f);

    s.world.Tick(kDt);
    EXPECT_EQ((*handle())->dmg, 8);
    EXPECT_EQ(handle()->get(), raw);

    // Back to generation 1: the handle is the same object.
    ASSERT_TRUE(s.loader.request_reload(std::move(v1_again), error)) << error;
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    ASSERT_NE(handle(), nullptr);
    EXPECT_EQ(handle()->get(), raw);
    EXPECT_EQ(handle()->use_count(), 1);
    // The Tick before the commit still ran generation 2.
    EXPECT_EQ((*handle())->dmg, 9);
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

TEST_P(BackendTests, HandleFieldSurvivesReload)
{
    script::LoadResult v1 = backend->load(source("handle_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    script::LoadResult v2 = backend->load(source("handle_v2.script"), 2);
    ASSERT_TRUE(v2) << v2.error;
    script::LoadResult v1b = backend->load(source("handle_v1.script"), 3);
    ASSERT_TRUE(v1b) << v1b.error;
    run_handle_reload(std::move(*v1.module), std::move(*v2.module), std::move(*v1b.module));
}

// A field or a local without an initializer has the zero value.
TEST_P(BackendTests, DefaultValues)
{
    script::LoadResult v1 = backend->load(source("default_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*script::field_ptr<int>(s.inst.shell, "n"), 0);
    EXPECT_EQ(*script::field_ptr<float>(s.inst.shell, "f"), 0.0f);
    EXPECT_FALSE(*script::field_ptr<bool>(s.inst.shell, "b"));
    EXPECT_EQ(script::field_ptr<std::shared_ptr<Weapon>>(s.inst.shell, "weapon")->get(), nullptr);
    EXPECT_EQ(*script::field_ptr<int>(s.inst.shell, "seen"), 0);
}

// A change of the field type from a handle to int is rejected. The old generation stays.
TEST_P(BackendTests, HandleTypeChangeIsRejected)
{
    script::LoadResult v1 = backend->load(source("handle_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    script::LoadResult v3 = backend->load(source("handle_v3.script"), 2);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    std::string error;
    EXPECT_FALSE(s.loader.request_reload(std::move(*v3.module), error));
    EXPECT_FALSE(error.empty());
    s.world.Tick(kDt);
    EXPECT_EQ(script::active_generation, 1u);
    auto handle = script::field_ptr<std::shared_ptr<Weapon>>(s.inst.shell, "weapon");
    ASSERT_NE(handle, nullptr);
    EXPECT_NE(handle->get(), nullptr);
}

// An intrusive handle in a field of a subclass type survives reloads. The count is right:
// no leak, no early delete and no double release.
TEST_P(BackendTests, IntrusiveHandleSurvivesReload)
{
    using H = script::Intrusive<Item, &Object::AddRef, &Object::Release>;
    script::LoadResult v1 = backend->load(source("intrusive_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    script::LoadResult v2 = backend->load(source("intrusive_v2.script"), 2);
    ASSERT_TRUE(v2) << v2.error;
    script::LoadResult v1b = backend->load(source("intrusive_v1.script"), 3);
    ASSERT_TRUE(v1b) << v1b.error;
    ASSERT_EQ(Object::live, 0);
    {
        Session s;
        ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Smith"));
        s.commit_at_frame_boundary();
        auto item = [&] { return script::field_ptr<H>(s.inst.shell, "item"); };

        s.world.Tick(kDt);
        ASSERT_NE(item(), nullptr);
        Item* raw = item()->get();
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(Object::live, 1);
        EXPECT_EQ(raw->value, 5);

        // The reload commits after the next Tick, which is still generation 1.
        std::string error;
        ASSERT_TRUE(s.loader.request_reload(std::move(*v2.module), error)) << error;
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        EXPECT_EQ(script::active_generation, 2u);
        ASSERT_NE(item(), nullptr);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(Object::live, 1);

        // Generation 2: the field "spare" shares the object.
        s.world.Tick(kDt);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 2);
        EXPECT_EQ(raw->value, 6);
        EXPECT_EQ(Object::live, 1);

        // Back to generation 1: the commit removes the field "spare" and releases it.
        ASSERT_TRUE(s.loader.request_reload(std::move(*v1b.module), error)) << error;
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        EXPECT_EQ(script::active_generation, 3u);
        ASSERT_NE(item(), nullptr);
        EXPECT_EQ(item()->get(), raw);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(raw->value, 7);
        EXPECT_EQ(Object::live, 1);
    }
    EXPECT_EQ(Object::live, 0);
}

// The stub names a method that the native class does not have. The load gives an error.
TEST_P(BackendTests, IntrusiveMissingMethodIsAnError)
{
    script::LoadResult r = backend->load(source("intrusive_bad.script"), 1);
    EXPECT_FALSE(r);
    EXPECT_NE(r.error.find("Grab"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("Object"), std::string::npos) << r.error;
}

// A script with properties: an auto property, a read-only property, a property with a private
// set accessor and a custom getter and setter. The hidden fields are normal state fields.
TEST_P(BackendTests, PropertiesReadAndWrite)
{
    script::LoadResult v1 = backend->load(source("props_v1.script"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Gauge"));
    ASSERT_NE(s.field("__p_Hp"), nullptr);
    ASSERT_NE(s.field("__p_MaxHp"), nullptr);
    ASSERT_NE(s.field("shield_raw"), nullptr);
    EXPECT_EQ(s.field("__p_Shield"), nullptr);  // a custom property has no hidden field
    EXPECT_EQ(*s.field("__p_Hp"), 50.0f);
    EXPECT_EQ(*s.field("__p_MaxHp"), 100.0f);

    s.world.Tick(kDt);  // Hp += 1, Shield = Shield + 2, Level++
    EXPECT_EQ(*s.field("__p_Hp"), 51.0f);
    EXPECT_EQ(*s.field("shield_raw"), 2.0f);
    const int* level = script::field_ptr<int>(s.inst.shell, "__p_Level");
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(*level, 2);

    // The custom setter clamps the shield at 0. Then the damage reaches Hp.
    s.actor->TakeDamage(5.0f);
    EXPECT_EQ(*s.field("shield_raw"), 0.0f);
    EXPECT_EQ(*s.field("__p_Hp"), 46.0f);
}

// One script object reads and writes the properties of another script object of the class.
// The accessors run for the other object. An object of another class gives an exception.
TEST_P(BackendTests, PropertiesOfOtherObjectReadAndWrite)
{
    const script::ScriptSource src = {"props_other.script", R"(
using "Actor.h";

class Gauge : Actor {
    float Hp { get; set; } = 50.0;
    float MaxHp { get; } = 100.0;
    int Hits { get; private set; } = 0;
    float raw = 0.0;
    float Shield {
        get { return this.raw; }
        set { this.raw = value < 0.0 ? 0.0 : value; }
    }
    float seen = 0.0;
    float chained = 0.0;
    float old_hits = 0.0;

    override void Tick(float dt) {}
    override void TakeDamage(float amount) {}
    override void TakeDamage(Actor& source, float amount) {
        this.seen = source.Hp + source.MaxHp;
        source.Hp -= amount;
        source.Hits++;
        source.Shield = -5.0;
        var p = ref source;
        p.Shield += 3.0;
        this.chained = source.Hp = this.Hp = 30.0;
        this.old_hits = source.Hits++;
    }
}
)"};
    script::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Gauge"));
    const script::ClassEntry* entry = s.loader.find("Gauge");
    ASSERT_NE(entry, nullptr);
    const script::Instance other = entry->create();
    Actor* source = static_cast<Actor*>(other.native);
    s.world.Add(source);

    s.actor->TakeDamage(*source, 5.0f);
    auto f = [](script::ShellBase* sh, const char* name) { return *script::field_ptr<float>(sh, name); };
    EXPECT_EQ(f(s.inst.shell, "seen"), 150.0f);  // 50 + 100, read on the other object
    EXPECT_EQ(f(s.inst.shell, "__p_Hp"), 30.0f);
    EXPECT_EQ(f(s.inst.shell, "chained"), 30.0f);
    EXPECT_EQ(f(s.inst.shell, "old_hits"), 1.0f);  // "Hits++" gives the old value
    EXPECT_EQ(f(other.shell, "__p_Hp"), 30.0f);     // 50 - 5, then 30
    EXPECT_EQ(f(other.shell, "raw"), 3.0f);         // the setter clamps -5 to 0, then 0 + 3
    EXPECT_EQ(*script::field_ptr<int>(other.shell, "__p_Hits"), 2);
    // The accessors did not touch the state of the caller.
    EXPECT_EQ(*script::field_ptr<int>(s.inst.shell, "__p_Hits"), 0);
    EXPECT_EQ(f(s.inst.shell, "raw"), 0.0f);

    // A native actor is not a Gauge. The property access throws and changes nothing.
    Actor native;
    EXPECT_THROW(s.actor->TakeDamage(native, 1.0f), std::runtime_error);
    EXPECT_EQ(f(other.shell, "__p_Hp"), 30.0f);
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
using "Actor.h";
using <utility>;
using <vector>;

class Player : Actor {
    float total = 0.0;

    std.pair<std.vector<int>, int> mk() {
        var p = std.pair<std.vector<int>, int>();
        p.first.push_back(1);
        p.first.push_back(2);
        p.first.push_back(3);
        return p;
    }

    override void Tick(float dt) {
        foreach (var a in this.mk().first) {
            this.total = this.total + a;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(Actor& source, float amount) {}
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
using "Actor.h";
using <stdexcept>;

class Player : Actor {
    float total = 0.0;

    override void Tick(float dt) {
        try {
            throw std.runtime_error("boom");
        } catch (const std.exception& e) {
            var w = e.what();
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
        } catch (double v) {
            this.total = 100.0;
        } catch (int v) {
            this.total = this.total + v;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(Actor& source, float amount) {}
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

// A finally block runs on the normal path and on the throw path. An exception from
// a finally body is dropped. Nested finally blocks run from the inside out.
TEST_P(BackendTests, TryFinallyRun)
{
    const script::ScriptSource src = {"fin.script", R"(
using "Actor.h";

class Player : Actor {
    float total = 0.0;

    override void Tick(float dt) {
        try {
            this.total = this.total + 1.0;
        } finally {
            this.total = this.total + 10.0;
        }
        try {
            try {
                throw 5;
            } finally {
                this.total = this.total + 100.0;
            }
        } catch (int v) {
            this.total = this.total + v;
        }
        try {
            this.total = this.total + 1000.0;
        } catch (...) {
            this.total = -1.0;
        } finally {
            throw 1;
        }
        try {
            try {
                this.total = this.total + 0.0;
            } finally {
                this.total = this.total + 2.0;
            }
        } finally {
            this.total = this.total * 2.0;
        }
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(Actor& source, float amount) {}
}
)"};
    script::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(kDt);
    ASSERT_NE(s.field("total"), nullptr);
    // (1 + 10 + 100 + 5 + 1000 + 2) * 2
    EXPECT_EQ(*s.field("total"), 2236.0f);
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

TEST(PrecompiledTests, HandleFieldSurvivesReloadWithLibraries)
{
    run_handle_reload(test_support::library(PRE_HANDLE_V1_SO), test_support::library(PRE_HANDLE_V2_SO),
                      test_support::library(PRE_HANDLE_V1B_SO));
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
