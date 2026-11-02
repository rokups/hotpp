// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the "[value]" attribute with a loaded script (hot::Value). The first Tick of the
// holder runs each use once and writes the results to int fields. The tests use the backend that
// HOT_BACKEND selected.
#include "hot/backend.h"
#include "hot/runtime/value.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

// The shell of a script class without a native base and without members has this layout. The
// host uses the type to read the object in a hot::Value.
struct PlainView
    : hot::PlainObject
    , hot::ShellBase
{
};

using Box = hot::Value<PlainView>;

int field_int(Session& s, const char* name)
{
    const int* p = hot::field_ptr<int>(s.inst.shell, name);
    return p ? *p : -1000;
}

// The box of the [value] field "name" of the holder. The host cannot name the type of the field, so
// the function reads the box at the offset of the field.
Box* box_of(Session& s, const char* name)
{
    const hot::FieldInfo* f = hot::find_field(s.inst.shell->cls, name);
    if (!f)
        return nullptr;
    return reinterpret_cast<Box*>(
        static_cast<char*>(static_cast<hot::ShellBase*>(s.inst.shell)->state) + f->offset);
}

// The object of the [value] field "name" of the holder.
PlainView* box_object(Session& s, const char* name)
{
    Box* b = box_of(s, name);
    return b ? b->get() : nullptr;
}

void start(hot::Backend& backend, Session& s, const char* file)
{
    hot::LoadResult v1 = backend.load(source(file), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

const hot::GenerationInfo* find_gen(const hot::LoaderStats& st, uint32_t generation)
{
    for (const hot::GenerationInfo& g : st.generations)
        if (g.generation == generation)
            return &g;
    return nullptr;
}

// A copy of a [value] is independent of the source. The fields keep their own objects.
TEST_P(BackendTests, ValueCopyIsIndependent)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, "value_use_v1.hot"));
    EXPECT_EQ(field_int(s, "r_a"), 10);
    EXPECT_EQ(field_int(s, "r_b"), 20); // "b = a" copied, and "b.x = 20" did not change "a"
    EXPECT_NE(box_object(s, "a"), box_object(s, "b"));
}

// A box of the base class that holds a derived object keeps the class of the object when it is
// copied (no slicing).
TEST_P(BackendTests, ValueCopyKeepsTheRealClass)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, "value_use_v1.hot"));
    PlainView* sub = box_object(s, "sub");
    ASSERT_NE(sub, nullptr);
    EXPECT_STREQ(sub->cls->name, "Sub");
    EXPECT_EQ(*hot::field_ptr<int>(sub, "y"), 7);
    EXPECT_EQ(*hot::field_ptr<int>(sub, "x"), 1); // the copy "s2" changed its own "x" only
}

// A [value] parameter is a copy. The function changes its copy only.
TEST_P(BackendTests, ValueParameterIsACopy)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, "value_use_v1.hot"));
    EXPECT_EQ(field_int(s, "r_param"), 11);
    EXPECT_EQ(field_int(s, "r_param_a"), 10);
}

// A [value] return holds the new object. A return of a field copies the field.
TEST_P(BackendTests, ValueReturnWorks)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, "value_use_v1.hot"));
    EXPECT_EQ(field_int(s, "r_ret"), 7);
    EXPECT_EQ(field_int(s, "r_dup"), 10); // the change of the copy did not reach the field
}

// A handle source gives a copy. A local without an initializer holds a new object. "var" copies.
// A lambda captures a copy.
TEST_P(BackendTests, ValueLocalsCopyTheirSource)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, "value_use_v1.hot"));
    EXPECT_EQ(field_int(s, "r_handle"), 42); // the handle object did not change
    EXPECT_EQ(field_int(s, "r_hv"), 43);
    EXPECT_EQ(field_int(s, "r_z"), 1);
    EXPECT_EQ(field_int(s, "r_lam"), 3); // the capture copied the value before "cap.x = 5"
    EXPECT_EQ(field_int(s, "r_cap"), 5);
    EXPECT_EQ(field_int(s, "r_var"), 10);
}

// A copy of a null handle into a [value] throws. A script cannot name a field that does not
// copy, so the unit tests of hot::Value check the error text of such a field.
TEST_P(BackendTests, ValueCopyOfNullHandleThrows)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s, "value_use_v1.hot"));
    EXPECT_EQ(field_int(s, "caught_null"), 1);
}

// A reload keeps the object of a [value] field. The loader moves it, and it does not copy it.
// The new field gets its initializer.
TEST_P(BackendTests, ValueFieldMigratesOnReload)
{
    hot::LoadResult v1 = backend->load(source("value_use_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("value_use_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    PlainView* before = box_object(s, "a");
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(*hot::field_ptr<int>(before, "x"), 10);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStdout();
    s.world.Tick(kDt); // the commit happens at the end of this frame
    testing::internal::GetCapturedStdout();
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);

    PlainView* after = box_object(s, "a");
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(after, "x"), 10);
    PlainView* added = box_object(s, "added");
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(added, "x"), 1);
    EXPECT_EQ(box_object(s, "b"), nullptr); // the field went away
}

// V7: a copy that the script made keeps its generation alive. The host holds the copy, and the
// state of generation 2 has no field that holds code. Generation 1 must stay retired until the
// host drops the copy.
TEST_P(BackendTests, ValueCopyKeepsItsGenerationAlive)
{
    hot::LoadResult v1 = backend->load(source("value_use_retain_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("value_use_retain_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);

    Box* field = box_of(s, "keep");
    ASSERT_NE(field, nullptr);
    std::shared_ptr<PlainView> kept = field->handle(); // the object of the copy
    ASSERT_TRUE(kept);
    EXPECT_EQ(*hot::field_ptr<int>(kept.get(), "x"), 5);
    EXPECT_EQ(kept->creator, 1u); // the copy knows the generation that made it

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStderr();
    for (int i = 0; i < 4; ++i)
        s.world.Tick(kDt);
    testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);
    EXPECT_FALSE(s.loader.stats().state_holds_code);
    EXPECT_EQ(find_gen(s.loader.stats(), 1)->state, hot::GenState::Retired);
    EXPECT_EQ(*hot::field_ptr<int>(kept.get(), "x"), 5); // the object is intact

    kept.reset();
    testing::internal::CaptureStderr();
    for (int i = 0; i < 4; ++i)
        s.world.Tick(kDt);
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(find_gen(s.loader.stats(), 1)->state, hot::GenState::Unloaded);
}

} // namespace
