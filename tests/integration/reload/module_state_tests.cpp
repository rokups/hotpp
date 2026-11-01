// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of the module-level declarations (S8a, design 5.3): a module variable, a script
// enum and a script struct. They use the backend that HOT_BACKEND selected.
#include "hot/backend.h"
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

template <class T> T* fld(hot::ShellCore* shell, const char* name)
{
    return hot::field_ptr<T>(shell, name);
}

// The module state block of the loader. The test reads a variable by its name.
template <class T> T* modvar(Session& s, const char* name)
{
    const hot::ModuleStateInfo ms = s.loader.module_state();
    if (!ms.layout || !*ms.slot)
        return nullptr;
    for (uint32_t i = 0; i < ms.layout->field_count; ++i)
        if (std::string(ms.layout->fields[i].name) == name)
            return reinterpret_cast<T*>(static_cast<char*>(*ms.slot) + ms.layout->fields[i].offset);
    return nullptr;
}

// Both classes of the module read and write the same variable. An enum works in a field,
// a local, a parameter, a switch and a compare.
TEST_P(BackendTests, ModuleVariableIsSharedByAllClasses)
{
    hot::LoadResult v1 = backend->load(source("module_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    hot::Instance helper = s.loader.find("Helper")->create();
    s.world.Add(static_cast<Actor*>(helper.native));
    s.commit_at_frame_boundary();
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 0);
    s.world.Tick(kDt);
    // Hero adds the weight of Walk (1). Helper adds 100.
    EXPECT_EQ(*modvar<int>(s, "total"), 101);
    EXPECT_EQ(*fld<int>(helper.shell, "seen"), 101);
    // Hero: mode is Run (10) and the local is Walk (1).
    EXPECT_EQ(*fld<int>(s.inst.shell, "score"), 11);
    EXPECT_EQ(*modvar<float>(s, "last"), 1.0f); // the struct begins with x
    s.world.Tick(kDt);
    EXPECT_EQ(*modvar<int>(s, "total"), 202);
}

// The reload keeps the values of the module variables. The alias moves "total" to "sum". The
// new variable "bonus" has its initializer. The struct and the enum change.
TEST_P(BackendTests, ReloadKeepsModuleVariables)
{
    hot::LoadResult v1 = backend->load(source("module_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("module_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    hot::Instance helper = s.loader.find("Helper")->create();
    s.world.Add(static_cast<Actor*>(helper.native));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_EQ(*modvar<int>(s, "total"), 202);
    ASSERT_EQ(*modvar<int>(s, "mode"), 2); // Mode.Run

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);
    // The Tick of the commit frame ran with generation 1: total is 303.
    EXPECT_EQ(modvar<int>(s, "total"), nullptr); // the name is "sum" now
    ASSERT_NE(modvar<int>(s, "sum"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "sum"), 303);
    EXPECT_EQ(*modvar<int>(s, "mode"), 2); // the enum value migrates as an integer
    EXPECT_EQ(*modvar<int>(s, "bonus"), 7); // new variable: the initializer
    // The struct kept x and y. The new field z has its initializer.
    const hot::ModuleStateInfo ms = s.loader.module_state();
    const hot::FieldInfo* last = nullptr;
    for (uint32_t i = 0; i < ms.layout->field_count; ++i)
        if (std::string(ms.layout->fields[i].name) == "last")
            last = &ms.layout->fields[i];
    ASSERT_NE(last, nullptr);
    ASSERT_NE(last->sub, nullptr);
    float x = -1.0f, z = -1.0f;
    for (uint32_t i = 0; i < last->sub->field_count; ++i)
    {
        const hot::FieldInfo& f = last->sub->fields[i];
        const float* p = reinterpret_cast<const float*>(static_cast<char*>(*ms.slot) + last->offset + f.offset);
        if (std::string(f.name) == "x")
            x = *p;
        if (std::string(f.name) == "z")
            z = *p;
    }
    EXPECT_EQ(x, 3.0f);
    EXPECT_EQ(z, 5.0f);

    s.world.Tick(kDt); // the first Tick of generation 2
    // Hero: Walk (1) + bonus (7). Helper: 100.
    EXPECT_EQ(*modvar<int>(s, "sum"), 303 + 8 + 100);
    EXPECT_EQ(*fld<int>(s.inst.shell, "score"), 10); // Weight(Run) in generation 2
}

// An initializer of a module variable reads an earlier variable (S16). A convert hook keeps the
// value of a variable whose type changes. A new variable reads the migrated earlier variable.
TEST_P(BackendTests, ModuleVariableOrderAndConvertHook)
{
    hot::LoadResult v1 = backend->load(source("modconv_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("modconv_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Probe"));
    s.commit_at_frame_boundary();
    ASSERT_NE(modvar<int>(s, "doubled"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "doubled"), 10); // seed * 2
    s.world.Tick(kDt);
    EXPECT_EQ(*modvar<int>(s, "doubled"), 11);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // doubled is 12; the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    ASSERT_NE(modvar<float>(s, "level"), nullptr);
    EXPECT_FLOAT_EQ(*modvar<float>(s, "level"), 4.5f); // convert_level(3)
    ASSERT_NE(modvar<int>(s, "bonus"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "bonus"), 112); // the migrated doubled + 100
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s.inst.shell, "seen"), 12);
}

} // namespace
