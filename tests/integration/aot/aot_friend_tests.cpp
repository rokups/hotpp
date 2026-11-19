// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script method calls a protected non-virtual member of the native base. The AOT C++ must compile.
#include "ProtBase.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{

class AotFriend : public ::testing::Test
{
protected:
    void SetUp() override
    {
        backend = hot::make_default_backend();
        backend->seed_slots(hot::aot_table.slots);
        ASSERT_TRUE(loader.load_aot(hot::aot_table));
    }

    void call(const char* thunk_name, ProtBase& g, int n)
    {
        void* fn = loader.thunk(thunk_name);
        ASSERT_NE(fn, nullptr) << thunk_name;
        reinterpret_cast<void (*)(ProtBase*, int32_t)>(fn)(&g, n);
    }

    hot::Loader loader;
    std::unique_ptr<hot::Backend> backend;
};

TEST_F(AotFriend, ParentMethodsCallProtectedBaseMember)
{
    const hot::ClassEntry* e = loader.find("Parent");
    ASSERT_NE(e, nullptr);
    hot::Instance inst = e->create();
    ProtBase& g = *static_cast<ProtBase*>(inst.native);
    call("Parent::PokeDirect", g, 1);
    call("Parent::PokeThis", g, 2);
    call("Parent::PokeLambda", g, 4);
    EXPECT_EQ(g.Value(), 7);
    delete &g;
}

TEST_F(AotFriend, ChildMethodsCallProtectedBaseMember)
{
    const hot::ClassEntry* e = loader.find("Child");
    ASSERT_NE(e, nullptr);
    hot::Instance inst = e->create();
    ProtBase& g = *static_cast<ProtBase*>(inst.native);
    call("Child::PokeChild", g, 1);
    call("Child::PokeChildLambda", g, 2);
    call("Parent::PokeLambda", g, 4);
    EXPECT_EQ(g.Value(), 7);
    delete &g;
}

} // namespace
