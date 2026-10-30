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

// The reload to generation 2. The convert hooks change level and total.
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

    ASSERT_NE(fld<float>(s.inst.shell, "hp"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 71.0f); // the field keeps its name and value
    ASSERT_NE(fld<float>(s.inst.shell, "level"), nullptr);
    EXPECT_FLOAT_EQ(*fld<float>(s.inst.shell, "level"), 10.0f); // convert_level(1)
    ASSERT_NE(modvar<float>(s, "total"), nullptr);
    EXPECT_FLOAT_EQ(*modvar<float>(s, "total"), 2.0f); // convert_total(1)
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);

    s.actor->Tick(kDt); // the Tick of generation 2
    EXPECT_FLOAT_EQ(*modvar<float>(s, "total"), 3.0f);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 5.0f * kDt + 2.0f * 5.0f * kDt);
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
