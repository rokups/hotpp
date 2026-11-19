// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Optimized AOT: a unit with [fn_invoker] keeps its function table. The other units have an empty table.
#include "hot/aot.h"
#include "hot/runtime/registry.h"
#include "hot/runtime/shell.h"
#include "engine/Invoker.h"

#include <gtest/gtest.h>

namespace
{
int call(const hot::FnTable& t, const char* name, int in)
{
    const hot::FnEntry* e = t.find(name);
    EXPECT_NE(e, nullptr) << name;
    if (!e)
        return -1;
    EXPECT_EQ(e->sig, typeid(TestInvoker).name());
    const TestInvokeArgs a{in};
    EXPECT_TRUE(reinterpret_cast<TestInvoker>(e->fn)(a));
    return a.out;
}
} // namespace

TEST(AotKeepTable, TheTableIsOptimizedAndTheKeepingUnitHasEntries)
{
    EXPECT_TRUE(hot::aot_table.optimized);
    hot::AotRegistry reg(hot::aot_table);
    const hot::UnitInfo keep = hot::unit_ref("aot_keep").info;
    ASSERT_NE(keep.fns, nullptr);
    EXPECT_NE(keep.fns->find("KeepAdd"), nullptr);
    EXPECT_NE(keep.fns->find("KeepViaBase"), nullptr);
    ASSERT_NE(keep.fns->find("KeepAdd@call"), nullptr);
    EXPECT_EQ(keep.fns->find("KeepAdd@call")->sig, typeid(TestInvoker).name());
    EXPECT_EQ(call(*keep.fns, "KeepViaBase@call", 5), 11) << "a call into a function of another unit";
}

TEST(AotKeepTable, OtherUnitsHaveNoFunctionTable)
{
    hot::AotRegistry reg(hot::aot_table);
    for (const char* name : {"aot_demo", "module_v1", "aot_keep_base"})
    {
        const hot::UnitInfo u = hot::unit_ref(name).info;
        if (u.fns)
            EXPECT_TRUE(u.fns->entries.empty()) << name;
    }
}
