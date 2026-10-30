// Reload tests of the kinds of script classes (S5, design 7.10.1 and 5.3): a [value] class
// that lives inline in the state block, a class without a base that is a handle, and a class
// with a native base that is a handle. They use the backend that SPP_BACKEND selected.
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;
using test_support::source;

template <class T>
T* fld(Session& s, const char* name)
{
    return spp::field_ptr<T>(s.inst.shell, name);
}

// A field of the [value] class that the field "outer" holds inline. The test cannot name the
// struct, so it reads the field table of the value class.
template <class T>
T* inner(Session& s, const char* outer, const char* name)
{
    const spp::FieldInfo* o = spp::find_field(s.inst.shell->cls, outer);
    if (!o || !o->sub) return nullptr;
    for (uint32_t i = 0; i < o->sub->field_count; ++i)
        if (std::string(o->sub->fields[i].name) == name)
            return reinterpret_cast<T*>(static_cast<char*>(s.inst.shell->state) + o->offset + o->sub->fields[i].offset);
    return nullptr;
}

// The shell of the script object that the handle field "name" holds. The handle is a
// std::shared_ptr of the shell. The first base of the shell is spp::PlainObject.
spp::ShellBase* handle_target(Session& s, const char* name)
{
    const spp::FieldInfo* f = spp::find_field(s.inst.shell->cls, name);
    if (!f) return nullptr;
    auto* h = reinterpret_cast<std::shared_ptr<spp::PlainObject>*>(static_cast<char*>(s.inst.shell->state) + f->offset);
    return dynamic_cast<spp::ShellBase*>(h->get());
}

// "stats = Stats(50, 2)" runs the constructor of the value class. "saved = stats" is a real
// copy: "Hurt" on one value does not change the other.
TEST_P(BackendTests, ValueClassHasCopySemantics)
{
    spp::LoadResult v1 = backend->load(source("kinds_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    ASSERT_NE(inner<float>(s, "stats", "hp"), nullptr);
    EXPECT_EQ(*inner<float>(s, "stats", "hp"), 50.0f);   // the constructor of the value class
    EXPECT_EQ(*inner<float>(s, "saved", "hp"), 100.0f);  // the field initializers only
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(*inner<float>(s, "saved", "hp"), 50.0f);  // the copy before Hurt
    EXPECT_EQ(*inner<float>(s, "stats", "hp"), 40.0f);
    EXPECT_EQ(*fld<float>(s, "effective"), 80.0f);  // a method of the value class
}

// "new Counter(10)" of a class without a base gives a handle. The handle owns the object.
TEST_P(BackendTests, PlainClassIsAHandle)
{
    spp::LoadResult v1 = backend->load(source("kinds_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("[kinds_v1] counter 10 made"), std::string::npos) << log;
    EXPECT_EQ(log.find("made", log.find("made") + 1), std::string::npos) << "made once: " << log;
    EXPECT_EQ(log.find("gone"), std::string::npos) << log;  // the field holds the handle
    spp::ShellBase* c = handle_target(s, "counter");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(*spp::field_ptr<int>(c, "count"), 10);
}

// The reload migrates the fields of the inline value by name, also when the order changes
// and a field is new (design 5.3). A handle field keeps its object.
TEST_P(BackendTests, ReloadMigratesInlineValuesRecursively)
{
    spp::LoadResult v1 = backend->load(source("kinds_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("kinds_v2.spp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    testing::internal::GetCapturedStdout();
    ASSERT_EQ(*inner<float>(s, "stats", "hp"), 20.0f);
    ASSERT_EQ(*inner<float>(s, "saved", "hp"), 30.0f);
    spp::ShellBase* before = handle_target(s, "counter");

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);  // the commit happens at the end of this frame
    testing::internal::GetCapturedStdout();
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(spp::active_generation, 2u);
    // Migrated by name (the Tick of the commit frame ran with generation 1 before). The new
    // field "mana" has its initializer.
    EXPECT_EQ(*inner<float>(s, "stats", "hp"), 10.0f);
    EXPECT_EQ(*inner<float>(s, "saved", "hp"), 20.0f);
    EXPECT_EQ(*inner<float>(s, "stats", "armor"), 2.0f);
    EXPECT_EQ(*inner<float>(s, "stats", "mana"), 7.0f);
    EXPECT_EQ(*inner<float>(s, "saved", "mana"), 7.0f);
    EXPECT_EQ(*fld<int>(s, "bonus"), 4);
    EXPECT_EQ(*fld<int>(s, "ticks"), 4);
    // The handle keeps its object. The object of the plain class migrated too.
    spp::ShellBase* after = handle_target(s, "counter");
    ASSERT_EQ(after, before);
    EXPECT_EQ(*spp::field_ptr<int>(after, "count"), 10);
    EXPECT_EQ(*spp::field_ptr<int>(after, "step"), 3);

    testing::internal::CaptureStdout();
    s.world.Tick(kDt);  // the first Tick of generation 2
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(*inner<float>(s, "stats", "hp"), 0.0f);        // 10 - 10
    EXPECT_EQ(*fld<float>(s, "effective"), 7.0f);            // 0 * 2 + mana of generation 2
    EXPECT_EQ(*inner<float>(s, "saved", "hp"), 10.0f);
}

// A shell class that Spawner holds as a handle is destroyed when the last handle goes.
// The destructor of the class runs once.
TEST_P(BackendTests, ShellClassHandleOwnsTheObject)
{
    spp::LoadResult v1 = backend->load(source("ctor_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spawner"));
    s.commit_at_frame_boundary();
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    const std::string first = testing::internal::GetCapturedStdout();
    EXPECT_EQ(first.find("gone"), std::string::npos) << first;
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);  // the field gets a new handle: the first Squad has no handle now
    const std::string second = testing::internal::GetCapturedStdout();
    EXPECT_NE(second.find("[ctor_v1] unit gone"), std::string::npos) << second;
}

// Type-directed access on a script handle: a field, a script function and a property of the
// Link through the handle field "first". "Self" returns "this" as a handle (S5b).
TEST_P(BackendTests, TypedAccessOnScriptHandlesAndReturnThis)
{
    spp::LoadResult v1 = backend->load(source("typed_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Chain"));
    s.commit_at_frame_boundary();
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(*fld<int>(s, "power"), 8);  // Hit(3), then hits = 4: Power is hits * 2
    EXPECT_EQ(*fld<int>(s, "seen"), 5);   // Self() is the same object: Hit(1) changes it
}

// "h?.F();" and "h?.field = v;" do nothing for a null handle. The receiver of a call runs once.
TEST_P(BackendTests, SafeCallRunsOnlyForANonNullHandle)
{
    spp::LoadResult v1 = backend->load(source("safe_nav_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Prober"));
    s.commit_at_frame_boundary();
    testing::internal::CaptureStdout();
    s.world.Tick(kDt);
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(*fld<int>(s, "total"), 12);  // Hit, then += 10, then Hit through Pick()
    EXPECT_EQ(*fld<int>(s, "calls"), 1);   // Pick() runs once
    EXPECT_EQ(handle_target(s, "empty"), nullptr);
}

}  // namespace
