// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script calls "x?.M<T>()" on a native pointer. The AOT C++ must compile and give the right result.
#include "SafeHost.h"
#include "Actor.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{

class AotSafeTemplate : public ::testing::Test
{
protected:
    void SetUp() override
    {
        backend = hot::make_default_backend();
        backend->seed_slots(hot::aot_table.slots);
        ASSERT_TRUE(loader.load_aot(hot::aot_table));
    }

    int call(const char* thunk_name, Actor& self, SafeHost* x)
    {
        void* fn = loader.thunk(thunk_name);
        EXPECT_NE(fn, nullptr) << thunk_name;
        return fn ? reinterpret_cast<int32_t (*)(Actor*, SafeHost*)>(fn)(&self, x) : 0;
    }

    hot::Loader loader;
    std::unique_ptr<hot::Backend> backend;
};

TEST_F(AotSafeTemplate, NonNullAndNullReceiver)
{
    const hot::ClassEntry* e = loader.find("Prober");
    ASSERT_NE(e, nullptr);
    hot::Instance inst = e->create();
    Actor& self = *static_cast<Actor*>(inst.native);
    SafeHost host;
    EXPECT_EQ(call("Prober::Size", self, &host), static_cast<int>(sizeof(double)));
    EXPECT_EQ(call("Prober::Size", self, nullptr), -1);
    EXPECT_EQ(call("Prober::Sum", self, &host), 5 + static_cast<int>(sizeof(char)));
    EXPECT_EQ(call("Prober::Sum", self, nullptr), -1);
    delete &self;
}

} // namespace
