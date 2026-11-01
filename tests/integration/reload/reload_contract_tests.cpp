// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of the rows of the design 5.3 table that S17 adds, with the backend that
// HOT_BACKEND selected: a changed method signature, a native container of a [value] class,
// the first use of "this" as a handle, and a raw function pointer in a state field (4.9).
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{

using hot::GenState;
using test_support::kDt;
using test_support::Session;
using test_support::source;

int* int_field(Session& s, const char* name)
{
    return hot::field_ptr<int>(s.inst.shell, name);
}

const hot::GenerationInfo* find_gen(const hot::LoaderStats& st, uint32_t generation)
{
    for (const auto& g : st.generations)
        if (g.generation == generation)
            return &g;
    return nullptr;
}

// Validates and commits one module at the next frame boundary. Returns the captured stderr.
std::string reload(Session& s, hot::LoadResult& r)
{
    testing::internal::CaptureStderr();
    const hot::ReloadResult res = s.loader.request_reload(std::move(*r.module));
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_RELOAD_OK(res);
    return log;
}

using Heal1 = void (*)(Actor*, int32_t);
using Heal2 = void (*)(Actor*, int32_t, int32_t);

// "Change a method signature": the delegate on the thunk of the old signature detaches with a
// warning. A new thunk serves the new signature. A reload with the same signature keeps it.
TEST_P(BackendTests, SignatureChangeDetachesTheOldDelegate)
{
    // The slot state changes when the Loader accepts a module. So build each generation after the
    // Loader accepted the one before it.
    hot::LoadResult v1 = backend->load(source("sigchange_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Medic"));
    s.commit_at_frame_boundary();
    hot::LoadResult v2 = backend->load(source("sigchange_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    auto old_heal = reinterpret_cast<Heal1>(s.loader.thunk("Medic::heal"));
    ASSERT_NE(old_heal, nullptr);
    old_heal(s.actor, 5);
    EXPECT_EQ(*int_field(s, "hp"), 15);

    const std::string log2 = reload(s, v2);
    EXPECT_NE(log2.find("the signature of Medic::heal changed"), std::string::npos) << log2;
    auto new_heal = reinterpret_cast<Heal2>(s.loader.thunk("Medic::heal"));
    ASSERT_NE(new_heal, nullptr);
    EXPECT_NE(reinterpret_cast<void*>(new_heal), reinterpret_cast<void*>(old_heal));
    const uint64_t before = hot::detached_call_count();
    testing::internal::CaptureStderr();
    old_heal(s.actor, 5); // detached: no call of heal(int, int) with a missing argument
    old_heal(s.actor, 5);
    const std::string log_calls = testing::internal::GetCapturedStderr();
    EXPECT_EQ(*int_field(s, "hp"), 15);
    EXPECT_EQ(hot::detached_call_count(), before + 2);
    EXPECT_NE(log_calls.find("old signature"), std::string::npos) << log_calls;
    EXPECT_EQ(log_calls.find("old signature"), log_calls.rfind("old signature")) << log_calls; // logs once
    new_heal(s.actor, 5, 1);
    EXPECT_EQ(*int_field(s, "hp"), 21);

    // The same signature: no new thunk, no warning. The new body runs through the same thunk.
    hot::LoadResult v3 = backend->load(source("sigchange_v3.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    const std::string log3 = reload(s, v3);
    EXPECT_EQ(log3.find("signature"), std::string::npos) << log3;
    EXPECT_EQ(s.loader.thunk("Medic::heal"), reinterpret_cast<void*>(new_heal));
    new_heal(s.actor, 5, 1);
    EXPECT_EQ(*int_field(s, "hp"), 28);
    old_heal(s.actor, 5);
    EXPECT_EQ(*int_field(s, "hp"), 28);
    ASSERT_FALSE(s.commit_failed);
}

// "Script value type inside a native container": the container migrates when the layout of
// the value class stays. A changed layout resets the container with a warning.
TEST_P(BackendTests, ContainerOfValueTypeResetsOnLayoutChange)
{
    hot::LoadResult v1 = backend->load(source("bag_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("bag_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v3 = backend->load(source("bag_v3.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Bag"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "count"), 2);
    const hot::FieldInfo* items = hot::find_field(s.inst.shell->cls, "items");
    ASSERT_NE(items, nullptr);
    EXPECT_NE(std::strchr(items->type_key, '#'), nullptr) << items->type_key;

    // The same layout of Entry: the vector moves. A tick runs before the commit and one after it.
    const std::string log2 = reload(s, v2);
    EXPECT_EQ(log2.find("container resets"), std::string::npos) << log2;
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "count"), 4);

    // Entry gets a field: the vector resets. The tick after the commit adds one item.
    const std::string log3 = reload(s, v3);
    EXPECT_NE(log3.find("field items"), std::string::npos) << log3;
    EXPECT_NE(log3.find("container resets"), std::string::npos) << log3;
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "count"), 1);
    ASSERT_FALSE(s.commit_failed);
}

// A fixed-size array field is a std::array. The values move when the type stays. A new size is
// a new type, so the field resets to its default value.
TEST_P(BackendTests, ArrayFieldMigratesAndResetsOnSizeChange)
{
    hot::LoadResult v1 = backend->load(source("array_mig_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("array_mig_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v3 = backend->load(source("array_mig_v3.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Grid"));
    s.commit_at_frame_boundary();
    using Three = std::array<int, 3>;
    using Four = std::array<int, 4>;
    Three* a3 = hot::field_ptr<Three>(s.inst.shell, "a");
    ASSERT_NE(a3, nullptr);
    EXPECT_EQ(*a3, (Three{1, 2, 3})); // the default value
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    EXPECT_EQ(*a3, (Three{21, 2, 3}));
    EXPECT_EQ(*int_field(s, "count"), 2);

    // A new field: the array keeps its values. A tick runs before the commit and one after it.
    reload(s, v2);
    s.world.Tick(kDt);
    a3 = hot::field_ptr<Three>(s.inst.shell, "a");
    ASSERT_NE(a3, nullptr);
    EXPECT_EQ(*a3, (Three{31, 102, 3}));
    EXPECT_EQ(*int_field(s, "count"), 4);
    ASSERT_NE(int_field(s, "extra"), nullptr);
    EXPECT_EQ(*int_field(s, "extra"), 5);

    // The size changes from 3 to 4: the field is of a new type and resets. Other fields move.
    reload(s, v3);
    EXPECT_EQ(hot::field_ptr<Three>(s.inst.shell, "a"), nullptr);
    Four* a4 = hot::field_ptr<Four>(s.inst.shell, "a");
    ASSERT_NE(a4, nullptr);
    EXPECT_EQ(*a4, (Four{7, 8, 9, 10}));
    EXPECT_EQ(*int_field(s, "count"), 5);
    ASSERT_FALSE(s.commit_failed);
}

// The first use of "this" as a handle adds std::enable_shared_from_this to the shell. The
// loader rejects the reload with a message that names the cause. The old generation stays.
TEST_P(BackendTests, FirstHandleUseOfThisIsRejected)
{
    hot::LoadResult v1 = backend->load(source("echo_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("echo_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Echo"));
    s.commit_at_frame_boundary();
    EXPECT_FALSE(s.inst.shell->cls->shares_this);
    const hot::ReloadResult rejected = s.loader.request_reload(std::move(*v2.module));
    const std::string& error = rejected.error;
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status, hot::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(rejected.reason, hot::ReloadReason::ShellLayoutChanged);
    EXPECT_NE(error.find("uses 'this' as a handle"), std::string::npos) << error;
    EXPECT_NE(error.find("enable_shared_from_this"), std::string::npos) << error;
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "ticks"), 1);
    EXPECT_EQ(s.loader.stats().active_generation, 1u);
}

// A raw function pointer in a state field (design 4.9): the field has kMayHoldCode, so no
// retired generation unloads while the object lives.
TEST_P(BackendTests, FunctionPointerFieldBlocksTheUnload)
{
    hot::LoadResult v1 = backend->load(source("codeptr_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("codeptr_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Relay"));
    s.commit_at_frame_boundary();
    const hot::FieldInfo* op = hot::find_field(s.inst.shell->cls, "op");
    ASSERT_NE(op, nullptr);
    EXPECT_TRUE(op->flags & hot::kMayHoldCode);
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "result"), 2);
    reload(s, v2);
    for (int i = 0; i < 4; ++i)
        s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(*int_field(s, "result"), 4); // the pointer of generation 1 still runs
    const hot::LoaderStats st = s.loader.stats();
    EXPECT_TRUE(st.state_holds_code);
    ASSERT_NE(find_gen(st, 1), nullptr);
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired);
}

// A trivially copyable native struct with a function pointer field (S26): the translator marks
// the type as a code holder. The field has kMayHoldCode, so no retired generation unloads.
TEST_P(BackendTests, StructWithFunctionPointerFieldBlocksTheUnload)
{
    hot::LoadResult v1 = backend->load(source("codestruct_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("codestruct_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    const hot::FieldInfo* cb = hot::find_field(s.inst.shell->cls, "cb");
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(cb->flags & hot::kMayHoldCode);
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "result"), 2);
    reload(s, v2);
    for (int i = 0; i < 4; ++i)
        s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(*int_field(s, "result"), 4);
    const hot::LoaderStats st = s.loader.stats();
    EXPECT_TRUE(st.state_holds_code);
    ASSERT_NE(find_gen(st, 1), nullptr);
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired);
}

} // namespace
