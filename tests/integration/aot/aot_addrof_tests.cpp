// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script method passes a field of a native value type to a native "T*" parameter. The AOT C++
// must compile. The native host must get the address of the real field, not of a copy.
#include "VarHost.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{

class AotAddrOf : public ::testing::Test
{
protected:
    void SetUp() override
    {
        backend = hot::make_default_backend();
        backend->seed_slots(hot::aot_table.slots);
        ASSERT_TRUE(loader.load_aot(hot::aot_table));
    }

    void call0(const char* thunk_name, Urho3D::VarHost& h)
    {
        void* fn = loader.thunk(thunk_name);
        ASSERT_NE(fn, nullptr) << thunk_name;
        reinterpret_cast<void (*)(Urho3D::VarHost*)>(fn)(&h);
    }

    hot::Loader loader;
    std::unique_ptr<hot::Backend> backend;
};

TEST_F(AotAddrOf, FieldAddressReachesNativeHost)
{
    const hot::ClassEntry* e = loader.find("Panel");
    ASSERT_NE(e, nullptr);
    hot::Instance inst = e->create();
    auto& h = *static_cast<Urho3D::VarHost*>(inst.native);
    call0("Panel::BindPlain", h);
    Urho3D::Variant* first = h.Bound();
    ASSERT_NE(first, nullptr);
    call0("Panel::BindThis", h);
    EXPECT_EQ(h.Bound(), first);
    void* set = loader.thunk("Panel::SetValue");
    ASSERT_NE(set, nullptr);
    reinterpret_cast<void (*)(Urho3D::VarHost*, int32_t)>(set)(&h, 42);
    EXPECT_EQ(first->GetInt(), 42);
    delete &h;
}

TEST_F(AotAddrOf, MarkerReachesKnownVoidAndTemplateParameters)
{
    const hot::ClassEntry* e = loader.find("Panel");
    ASSERT_NE(e, nullptr);
    hot::Instance inst = e->create();
    auto& h = *static_cast<Urho3D::VarHost*>(inst.native);
    call0("Panel::BindPlain", h);
    Urho3D::Variant* field = h.Bound();
    h.KeepRaw(nullptr);
    call0("Panel::BindMarked", h);
    EXPECT_EQ(h.Bound(), field);
    EXPECT_EQ(h.Raw(), static_cast<void*>(field));
    EXPECT_EQ(h.Any(), static_cast<void*>(field));
    call0("Panel::KeepLocal", h);
    EXPECT_NE(h.Raw(), nullptr);
    EXPECT_NE(h.Raw(), static_cast<void*>(field));
    delete &h;
}

} // namespace
