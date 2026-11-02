// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::clone_shell: the host copies a script object of a class without a native base.
// The copy has the real class of the object and it does not run the script constructor. The tests
// use the backend that HOT_BACKEND selected.
#include "hot/backend.h"
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

// The host cannot name the shell of a script class. The shell of a class without a native base
// and without members has this layout: PlainObject, then ShellBase. So the test uses this type
// to give clone_shell a std::shared_ptr handle. The destructor is virtual (PlainObject).
struct PlainView
    : hot::PlainObject
    , hot::ShellBase
{
};

// The object that the handle field "name" of the holder keeps, as a PlainView.
PlainView* handle_view(Session& s, const char* name)
{
    const hot::FieldInfo* f = hot::find_field(s.inst.shell->cls, name);
    if (!f)
        return nullptr;
    auto* h = reinterpret_cast<std::shared_ptr<hot::PlainObject>*>(
        static_cast<char*>(static_cast<hot::ShellBase*>(s.inst.shell)->state) + f->offset);
    return static_cast<PlainView*>(dynamic_cast<hot::ShellBase*>(h->get()));
}

void start(hot::Backend& backend, Session& s)
{
    hot::LoadResult v1 = backend.load(source("clone_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

// The copy has the fields of the object at the time of the copy. It does not run the script
// constructor ("n" is not 100 again). A change of the copy does not change the object.
TEST_P(BackendTests, CloneShellCopiesTheStateWithoutTheConstructor)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    PlainView* a = handle_view(s, "plain");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(a, "n"), 100);
    *hot::field_ptr<int>(a, "n") = 5;
    *hot::field_ptr<std::string>(a, "label") = "changed";

    std::shared_ptr<PlainView> b = hot::clone_shell(*a);
    ASSERT_TRUE(b);
    EXPECT_NE(b.get(), a);
    EXPECT_EQ(b->cls, a->cls);
    EXPECT_EQ(*hot::field_ptr<int>(b.get(), "n"), 5);
    EXPECT_EQ(*hot::field_ptr<std::string>(b.get(), "label"), "changed");
    *hot::field_ptr<int>(b.get(), "n") = 6;
    EXPECT_EQ(*hot::field_ptr<int>(a, "n"), 5);
}

// The script destructor runs when the copy dies.
TEST_P(BackendTests, CloneShellRunsTheScriptDestructorWhenTheCopyDies)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    PlainView* a = handle_view(s, "plain");
    ASSERT_NE(a, nullptr);
    *hot::field_ptr<int>(a, "n") = 5;
    std::shared_ptr<PlainView> b = hot::clone_shell(*a);
    *hot::field_ptr<int>(b.get(), "n") = 77;
    testing::internal::CaptureStdout();
    b.reset();
    const std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("[clone_v1] base 77 gone"), std::string::npos) << log;
}

// An object of Derived, copied through the type of the base class: the copy is a Derived object.
TEST_P(BackendTests, CloneShellKeepsTheRealClassOfADerivedObject)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    PlainView* d = handle_view(s, "derived");
    ASSERT_NE(d, nullptr);
    ASSERT_STREQ(d->cls->name, "Derived");
    *hot::field_ptr<int>(d, "extra") = 9;
    std::shared_ptr<PlainView> c = hot::clone_shell(*d);
    ASSERT_TRUE(c);
    EXPECT_STREQ(c->cls->name, "Derived");
    EXPECT_EQ(c->cls, d->cls);
    EXPECT_EQ(*hot::field_ptr<int>(c.get(), "extra"), 9);
    EXPECT_EQ(*hot::field_ptr<int>(c.get(), "n"), 100);
    EXPECT_TRUE(hot::record_is(c->cls, "Base"));
}

// A copy after a reload has the layout of the new generation: the old fields keep their values and
// the new field has its initializer. The copy is independent of the object.
TEST_P(BackendTests, CloneShellAfterReloadUsesTheNewLayout)
{
    hot::LoadResult v1 = backend->load(source("clone_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("clone_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    PlainView* a = handle_view(s, "plain");
    ASSERT_NE(a, nullptr);
    *hot::field_ptr<int>(a, "n") = 5;
    ASSERT_EQ(hot::field_ptr<int>(a, "added"), nullptr); // generation 1 has no such field

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStdout();
    s.world.Tick(kDt); // the commit happens at the end of this frame
    testing::internal::GetCapturedStdout();
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);

    a = handle_view(s, "plain");
    ASSERT_NE(a, nullptr);
    std::shared_ptr<PlainView> b = hot::clone_shell(*a);
    ASSERT_TRUE(b);
    EXPECT_NE(b.get(), a);
    EXPECT_EQ(b->cls->layout->field_count, a->cls->layout->field_count);
    ASSERT_NE(hot::field_ptr<int>(b.get(), "added"), nullptr);
    EXPECT_EQ(*hot::field_ptr<int>(b.get(), "added"), 55);
    EXPECT_EQ(*hot::field_ptr<int>(b.get(), "n"), 5);
    *hot::field_ptr<int>(b.get(), "added") = 1;
    EXPECT_EQ(*hot::field_ptr<int>(a, "added"), 55);
}

// A host copy has no creator generation. A caller record gives its generation.
TEST_P(BackendTests, CloneShellNotesTheCreatorOfTheCaller)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    PlainView* a = handle_view(s, "plain");
    ASSERT_NE(a, nullptr);
    std::shared_ptr<PlainView> host = hot::clone_shell(*a);
    EXPECT_EQ(host->creator, 0u);
    hot::ClassRecord caller = *a->cls;
    caller.generation = 9;
    std::shared_ptr<PlainView> noted = hot::clone_shell(*a, &caller);
    EXPECT_EQ(noted->creator, 9u);
}

} // namespace
