// Reload tests of a plain C++ script (a ".cpp" script). The file extension selects the parser
// of sppc. The host passes the script as a ScriptSource like any other script. The tests use
// the backend that SPP_BACKEND selected.
#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

template <class T> T* fld(spp::ShellBase* shell, const char* name)
{
    return spp::field_ptr<T>(shell, name);
}

// The module state block of the loader. The test reads a variable by its name.
template <class T> T* modvar(Session& s, const char* name)
{
    const spp::ModuleStateInfo ms = s.loader.module_state();
    if (!ms.layout || !*ms.slot)
        return nullptr;
    for (uint32_t i = 0; i < ms.layout->field_count; ++i)
        if (std::string(ms.layout->fields[i].name) == name)
            return reinterpret_cast<T*>(static_cast<char*>(*ms.slot) + ms.layout->fields[i].offset);
    return nullptr;
}

// Generation 1 as a C++ script. The native virtual Tick runs the script code.
TEST_P(BackendTests, CppScriptRunsThroughNativeVirtual)
{
    spp::LoadResult v1 = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    s.commit_at_frame_boundary();
    EXPECT_EQ(spp::active_generation, 1u);
    ASSERT_NE(fld<float>(s.inst.shell, "hp"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 100.0f);
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 0);

    s.actor->TakeDamage(30.0f);
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 70.0f);
    s.actor->Tick(kDt); // heal 1, total + 1, Move by speed * dt
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 71.0f);
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);
    EXPECT_EQ(*modvar<int>(s, "total"), 1);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 5.0f * kDt);
    EXPECT_EQ(*fld<int>(s.inst.shell, "level"), 1);
}

// Default arguments of a .cpp script: a constructor, a constant and a module variable.
TEST_P(BackendTests, CppScriptDefaultArguments)
{
    spp::LoadResult v1 = backend->load(source("cpp_defaults.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Counter"));
    EXPECT_EQ(*fld<int>(s.inst.shell, "value"), 6); // the default "kStep * 3" of the constructor
    s.actor->Tick(kDt); // Add(10): 10 + 2 + calls(0). Add(10, 5, 100): 115.
    EXPECT_EQ(*fld<int>(s.inst.shell, "value"), 12);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 115);
    EXPECT_EQ(*modvar<int>(s, "calls"), 2);
    s.actor->Tick(kDt); // the default "calls" reads the module variable at each call: 2
    EXPECT_EQ(*fld<int>(s.inst.shell, "value"), 14);
    EXPECT_EQ(*modvar<int>(s, "calls"), 4);
}

// A free function reads and writes the module variable. A method uses the same variable. A reload
// migrates the variable, and the free function then uses the migrated value.
TEST_P(BackendTests, CppScriptFreeFunctionUsesModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_freefn.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_freefn.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Counter"));
    s.actor->Tick(kDt); // add_total(3): total = 6 + 1 = 7. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 7);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 107);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 107); // migrated

    s.actor->Tick(kDt); // add_total(3): 107 + 7 = 114. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 114);
    EXPECT_EQ(*modvar<int>(s, "total"), 214);
}

// A function template uses a module variable. The method and the template share the value, also
// after a reload.
TEST_P(BackendTests, CppScriptTemplateUsesModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_template_fn.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_template_fn.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Scaler"));
    s.actor->Tick(kDt); // total = 2. scaled(3) = 6.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 6);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 2);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 2); // migrated

    s.actor->Tick(kDt); // total = 4. scaled(3) = 12.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 12);
    EXPECT_EQ(*modvar<int>(s, "total"), 4);
}

// A function in a namespace uses a module variable. The method and the function share the value,
// also after a reload.
TEST_P(BackendTests, CppScriptNamespaceFunctionUsesModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_namespace_fn.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_namespace_fn.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Bumper"));
    s.actor->Tick(kDt); // counter::bump(5): total = 5. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 5);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 105);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 105); // migrated

    s.actor->Tick(kDt); // counter::bump(5): total = 110. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 110);
    EXPECT_EQ(*modvar<int>(s, "total"), 210);
}

// The reload to generation 2. The alias moves hp to health. The convert hooks change level and total.
TEST_P(BackendTests, CppScriptReloadMigratesFieldsAndModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_player_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    s.actor->TakeDamage(30.0f);
    s.actor->Tick(kDt); // hp 71, total 1
    ASSERT_EQ(*modvar<int>(s, "total"), 1);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);

    EXPECT_EQ(fld<float>(s.inst.shell, "hp"), nullptr); // the name is "health" now
    ASSERT_NE(fld<float>(s.inst.shell, "health"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "health"), 71.0f); // onReload did not change it
    ASSERT_NE(fld<float>(s.inst.shell, "level"), nullptr);
    EXPECT_FLOAT_EQ(*fld<float>(s.inst.shell, "level"), 10.0f); // convert_level(1)
    ASSERT_NE(modvar<float>(s, "total"), nullptr);
    EXPECT_FLOAT_EQ(*modvar<float>(s, "total"), 2.0f); // convert_total(1)
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);

    s.actor->Tick(kDt); // the Tick of generation 2
    EXPECT_FLOAT_EQ(*modvar<float>(s, "total"), 3.0f);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 5.0f * kDt + 2.0f * 5.0f * kDt);
}

// Static members of a script class. A static data member is a module variable "Counter__s_count".
// A static method is a free function. A reload keeps the value of a static data member. A new
// static data member takes its initializer. The hook convert_count converts the changed type.
TEST_P(BackendTests, CppScriptStaticMembersMigrate)
{
    spp::LoadResult v1 = backend->load(source("cpp_static_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_static_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Counter"));
    ASSERT_NE(modvar<int>(s, "Counter__s_count"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_count"), 10); // the definition outside the class
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 5); // the inline initializer
    EXPECT_EQ(*modvar<int>(s, "snapshot"), 11); // a module variable reads a static data member

    s.actor->Tick(kDt);
    s.actor->Tick(kDt); // count 12, hits 9
    EXPECT_EQ(*modvar<int>(s, "Counter__s_count"), 12);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 9);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 36); // a method calls the static method scale(12)
    EXPECT_EQ(*fld<int>(s.inst.shell, "via_free"), 45); // a free function: scale(12) + hits

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<float>(s, "Counter__s_count"), nullptr);
    EXPECT_FLOAT_EQ(*modvar<float>(s, "Counter__s_count"), 12.5f); // convert_count(12)
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 9); // migrated
    ASSERT_NE(modvar<int>(s, "Counter__s_extra"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_extra"), 7); // the new static takes its initializer
    EXPECT_EQ(*modvar<int>(s, "snapshot"), 11);

    s.actor->Tick(kDt); // count 13.5, hits 11, extra 8
    EXPECT_FLOAT_EQ(*modvar<float>(s, "Counter__s_count"), 13.5f);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 11);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_extra"), 8);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 47); // plus(scale(13), 8): a new static method
    EXPECT_EQ(*fld<int>(s.inst.shell, "via_free"), 50); // scale(13) + hits
}

// A static member can have the name of a function that the generated code has for the class.
TEST_P(BackendTests, CppScriptStaticMembersWithGeneratedNames)
{
    spp::LoadResult v1 = backend->load(source("cpp_static_names.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Names"));
    s.actor->Tick(kDt);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 4321);
}

// A static data member changes its type without a convert hook. It follows the rule of a module
// variable: a warning, and the new variable takes its default value.
TEST_P(BackendTests, CppScriptStaticMemberRetypeWithoutHook)
{
    spp::LoadResult v1 = backend->load(source("cpp_retype_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_retype_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Gauge"));
    s.actor->Tick(kDt);
    EXPECT_EQ(*modvar<int>(s, "Gauge__s_reading"), 11);
    EXPECT_EQ(*modvar<int>(s, "level"), 11);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStderr();
    const spp::ReloadResult res = s.loader.commit_pending();
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(res) << res.error;
    ASSERT_NE(modvar<float>(s, "Gauge__s_reading"), nullptr);
    ASSERT_NE(modvar<float>(s, "level"), nullptr);
    // The static data member and the module variable get the same value.
    EXPECT_FLOAT_EQ(*modvar<float>(s, "Gauge__s_reading"), *modvar<float>(s, "level"));
    EXPECT_NE(log.find("field Gauge__s_reading changed its type"), std::string::npos) << log;
    EXPECT_NE(log.find("field level changed its type"), std::string::npos) << log;
}

// The hook onReload runs at the reload and reads the old value through its old name.
TEST_P(BackendTests, CppScriptOnReloadRuns)
{
    spp::LoadResult v1 = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_player_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    *fld<float>(s.inst.shell, "hp") = 150.0f; // more than the maximum

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    ASSERT_NE(fld<float>(s.inst.shell, "health"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "health"), 100.0f); // onReload set it
}

// A .spp script and a .cpp script live in the same process. Each has its own loader and one object.
// One loader holds one module for each generation, so the two scripts use two loaders.
TEST_P(BackendTests, SppAndCppScriptsRunSideBySide)
{
    spp::LoadResult spp_mod = backend->load(source("echo_v1.spp"), 1);
    ASSERT_TRUE(spp_mod) << spp_mod.error;
    spp::LoadResult cpp_mod = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(cpp_mod) << cpp_mod.error;
    Session echo, player;
    ASSERT_NO_FATAL_FAILURE(echo.start(std::move(*spp_mod.module), "Echo"));
    ASSERT_NO_FATAL_FAILURE(player.start(std::move(*cpp_mod.module), "Player"));

    echo.world.Tick(kDt);
    echo.world.Tick(kDt);
    player.world.Tick(kDt);
    player.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(echo.inst.shell, "ticks"), 2);
    EXPECT_EQ(*fld<float>(player.inst.shell, "hp"), 100.0f); // heal is clamped
    EXPECT_EQ(*modvar<int>(player, "total"), 2);
    EXPECT_FLOAT_EQ(player.actor->Position.x, 2.0f * 5.0f * kDt);
}

} // namespace
