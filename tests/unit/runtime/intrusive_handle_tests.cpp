// Tests for spp::Intrusive. A new object starts with zero references.
#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>

#include "spp/runtime.h"

namespace {

struct RcBase {
    static inline int live = 0;
    int refs = 0;
    int x = 0;
    RcBase() { ++live; }
    virtual ~RcBase() { --live; }
    void AddRef() { ++refs; }
    void Release()
    {
        if (--refs == 0)
            delete this;
    }
};
struct RcDerived : RcBase {
    int y = 0;
};

using BaseH = spp::Intrusive<RcBase, &RcBase::AddRef, &RcBase::Release>;
using DerivedH = spp::Intrusive<RcDerived, &RcBase::AddRef, &RcBase::Release>;

static_assert(std::is_nothrow_move_constructible_v<BaseH>);
static_assert(std::is_nothrow_move_assignable_v<BaseH>);
static_assert(spp::is_handle_v<BaseH>);
static_assert(spp::is_handle_v<const BaseH&>);
static_assert(std::is_convertible_v<DerivedH, BaseH>);
static_assert(!std::is_convertible_v<BaseH, DerivedH>);

struct IntrusiveFixture : ::testing::Test {
    void SetUp() override { RcBase::live = 0; }
    void TearDown() override { EXPECT_EQ(RcBase::live, 0); }
};

TEST_F(IntrusiveFixture, NullHandle)
{
    BaseH a;
    BaseH b = nullptr;
    EXPECT_TRUE(a == nullptr);
    EXPECT_FALSE(a != nullptr);
    EXPECT_FALSE(static_cast<bool>(b));
    EXPECT_EQ(a.get(), nullptr);
    EXPECT_TRUE(a == b);
}

TEST_F(IntrusiveFixture, ConstructCopyMoveDestroy)
{
    RcBase* raw = new RcBase;
    EXPECT_EQ(raw->refs, 0);
    {
        BaseH a(raw);
        EXPECT_EQ(raw->refs, 1);
        EXPECT_TRUE(a != nullptr);
        EXPECT_FALSE(a == nullptr);
        EXPECT_TRUE(static_cast<bool>(a));
        {
            BaseH b = a;
            EXPECT_EQ(raw->refs, 2);
            EXPECT_TRUE(a == b);
            BaseH c = std::move(b);
            EXPECT_EQ(raw->refs, 2);
            EXPECT_TRUE(b == nullptr);
            EXPECT_EQ(c.get(), raw);
        }
        EXPECT_EQ(raw->refs, 1);
        EXPECT_EQ(RcBase::live, 1);
    }
    EXPECT_EQ(RcBase::live, 0);
}

TEST_F(IntrusiveFixture, Assignment)
{
    BaseH a(new RcBase);
    BaseH b(new RcBase);
    RcBase* pa = a.get();
    RcBase* pb = b.get();
    a = b;  // The first object dies. The second has two references.
    EXPECT_EQ(RcBase::live, 1);
    EXPECT_EQ(pb->refs, 2);
    EXPECT_EQ(a.get(), pb);
    BaseH c(new RcBase);
    EXPECT_EQ(RcBase::live, 2);
    a = std::move(c);
    EXPECT_EQ(RcBase::live, 2);
    EXPECT_TRUE(c == nullptr);
    EXPECT_EQ(pb->refs, 1);
    (void)pa;
    a = nullptr;
    EXPECT_TRUE(a == nullptr);
    EXPECT_EQ(RcBase::live, 1);
}

TEST_F(IntrusiveFixture, SelfAssignment)
{
    BaseH a(new RcBase);
    BaseH& r = a;
    a = r;
    EXPECT_EQ(a->refs, 1);
    a = std::move(r);
    ASSERT_TRUE(a != nullptr);
    EXPECT_EQ(a->refs, 1);
    EXPECT_EQ(RcBase::live, 1);
}

TEST_F(IntrusiveFixture, ConstHandleCopy)
{
    const BaseH a(new RcBase);
    RcBase* raw = a.get();
    {
        BaseH b = a;
        EXPECT_EQ(raw->refs, 2);
        const BaseH c = a;
        EXPECT_EQ(raw->refs, 3);
        EXPECT_TRUE(c == b);
    }
    EXPECT_EQ(raw->refs, 1);
    EXPECT_EQ(RcBase::live, 1);
}

TEST_F(IntrusiveFixture, Reset)
{
    BaseH a(new RcBase);
    BaseH b = a;
    a.reset();
    EXPECT_TRUE(a == nullptr);
    EXPECT_EQ(b->refs, 1);
    b.reset();
    EXPECT_EQ(RcBase::live, 0);
}

TEST_F(IntrusiveFixture, DerefReadWrite)
{
    BaseH h(new RcBase);
    spp::deref(h).x = 5;
    EXPECT_EQ(h->x, 5);
    EXPECT_EQ(spp::deref(h).x, 5);
    EXPECT_EQ((*h).x, 5);
    const BaseH& ch = h;
    EXPECT_EQ(spp::deref(ch).x, 5);
    EXPECT_EQ(spp::deref(BaseH(h)).x, 5);
    EXPECT_EQ(h->refs, 1);
    RcBase* raw = h;  // The implicit conversion gives the raw pointer.
    EXPECT_EQ(raw, h.get());
}

TEST_F(IntrusiveFixture, BaseConversion)
{
    DerivedH d(new RcDerived);
    d->y = 3;
    RcBase* raw = d.get();
    {
        BaseH b = d;  // Copy form.
        EXPECT_EQ(raw->refs, 2);
        EXPECT_EQ(b.get(), raw);
        spp::deref(b).x = 9;
        EXPECT_EQ(d->x, 9);
        BaseH c = std::move(d);  // Move form.
        EXPECT_TRUE(d == nullptr);
        EXPECT_EQ(raw->refs, 2);
        EXPECT_TRUE(c == b);
    }
    EXPECT_EQ(RcBase::live, 0);
}

}  // namespace
