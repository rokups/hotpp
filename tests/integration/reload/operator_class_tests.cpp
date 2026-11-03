// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the operators of a script class with a loaded script. The host cannot name the shell
// of the class, so the test has a view with the same operator members as the generated shell. A
// member reads the slot of the operator, like the generated member does. The tests use the
// backend that HOT_BACKEND selected. The slot numbers come from the slot file of the script: the
// order of the operators in the class (op_add, op_eq, op_lt, op_gt).
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

struct MoneyView
    : hot::PlainObject
    , hot::ShellBase
{
    using Box = hot::Value<MoneyView>;
    using In = hot::In<Box>;

    Box operator+(const MoneyView& o) const
    {
        return hot::call<Box(hot::PlainObject*, In)>(const_cast<MoneyView*>(this), 0, hot::borrow_value(o));
    }
    bool operator==(const MoneyView& o) const
    {
        return hot::call<bool(hot::PlainObject*, In)>(const_cast<MoneyView*>(this), 1, hot::borrow_value(o));
    }
    bool operator<(const MoneyView& o) const
    {
        return hot::call<bool(hot::PlainObject*, In)>(const_cast<MoneyView*>(this), 2, hot::borrow_value(o));
    }
    bool operator>(const MoneyView& o) const
    {
        return hot::call<bool(hot::PlainObject*, In)>(const_cast<MoneyView*>(this), 3, hot::borrow_value(o));
    }
    // The same text as the member that the translator generates for "+=".
    MoneyView& operator+=(const MoneyView& o)
    {
        hot::assign_value(*this, (*this + o));
        return *this;
    }
    int cents() const { return *hot::field_ptr<int>(const_cast<MoneyView*>(this), "cents"); }
};

// The box of the field "name" of the holder. The host reads the box at the offset of the field.
MoneyView& money(Session& s, const char* name)
{
    const hot::FieldInfo* f = hot::find_field(s.inst.shell->cls, name);
    EXPECT_NE(f, nullptr) << name;
    auto* b = reinterpret_cast<MoneyView::Box*>(
        static_cast<char*>(static_cast<hot::ShellBase*>(s.inst.shell)->state) + f->offset);
    return **b;
}

void start(hot::Backend& backend, Session& s)
{
    hot::LoadResult v1 = backend.load(source("operators_class_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
}

TEST_P(BackendTests, ClassOperatorsRunTheScriptBodies)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    MoneyView& a = money(s, "a");
    MoneyView& b = money(s, "b");
    MoneyView& c = money(s, "c");
    EXPECT_EQ((a + b)->cents(), 350);
    EXPECT_TRUE(a == c);
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_TRUE(b > a);
    // The operands keep their state: the call borrows the argument and does not change it.
    EXPECT_EQ(a.cents(), 100);
    EXPECT_EQ(b.cents(), 250);
}

TEST_P(BackendTests, ClassCompoundOperatorTakesTheStateOfTheResult)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    MoneyView& a = money(s, "a");
    MoneyView& b = money(s, "b");
    a += b;
    EXPECT_EQ(a.cents(), 350);
    EXPECT_EQ(b.cents(), 250);
    a += b;
    EXPECT_EQ(a.cents(), 600);
}

TEST_P(BackendTests, ClassOperatorsUseTheNewBodyAfterReload)
{
    hot::LoadResult v2 = backend->load(source("operators_class_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(*backend, s));
    ASSERT_EQ((money(s, "a") + money(s, "b"))->cents(), 350);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    ASSERT_EQ(hot::active_generation, 2u);

    MoneyView& a = money(s, "a");
    MoneyView& b = money(s, "b");
    MoneyView& c = money(s, "c");
    // The old objects keep their state. The new body of operator+ adds a fee of one cent.
    EXPECT_EQ(a.cents(), 100);
    EXPECT_EQ(b.cents(), 250);
    EXPECT_EQ((a + b)->cents(), 351);
    EXPECT_TRUE(a == c);
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(a > b);
}

} // namespace
