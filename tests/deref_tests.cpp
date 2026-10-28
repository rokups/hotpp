// Tests for script::deref. The translator writes "a.b" as "script::deref(a).b".
#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "spp/script_runtime.h"

namespace {

struct Item {
    int m = 0;
    int get() const { return m; }
};

Item* make_ptr(Item& i) { return &i; }
Item make_value() { return Item{7}; }

TEST(Deref, PointerGivesTheObject)
{
    Item i{3};
    Item* p = &i;
    EXPECT_EQ(script::deref(p).m, 3);
    EXPECT_EQ(&script::deref(p), &i);
}

TEST(Deref, ReferenceGivesTheSameObject)
{
    Item i{4};
    Item& r = i;
    EXPECT_EQ(script::deref(r).m, 4);
    EXPECT_EQ(&script::deref(r), &i);
}

TEST(Deref, ValueFromACallWorks)
{
    EXPECT_EQ(script::deref(make_value()).m, 7);
    EXPECT_EQ(script::deref(make_value()).get(), 7);
}

TEST(Deref, PointerFromACallWorks)
{
    Item i{5};
    EXPECT_EQ(script::deref(make_ptr(i)).m, 5);
}

TEST(Deref, AssignmentThroughAPointer)
{
    Item i;
    Item* p = &i;
    script::deref(p).m = 1;
    EXPECT_EQ(i.m, 1);
}

TEST(Deref, AssignmentThroughAReference)
{
    Item i;
    Item& r = i;
    script::deref(r).m = 2;
    EXPECT_EQ(i.m, 2);
}

TEST(Deref, PointerLvalueUsesThePointerOverload)
{
    Item i{6};
    Item* p = &i;
    Item*& pr = p;
    const Item* cp = &i;
    static_assert(std::is_same_v<decltype(script::deref(pr)), Item&>);
    static_assert(std::is_same_v<decltype(script::deref(cp)), const Item&>);
    static_assert(std::is_same_v<decltype(script::deref(make_ptr(i))), Item&>);
    EXPECT_EQ(script::deref(pr).m, 6);
    EXPECT_EQ(script::deref(cp).m, 6);
}

TEST(Deref, ValueKeepsItsCategory)
{
    Item i;
    const Item ci;
    static_assert(std::is_same_v<decltype(script::deref(i)), Item&>);
    static_assert(std::is_same_v<decltype(script::deref(ci)), const Item&>);
    static_assert(std::is_same_v<decltype(script::deref(make_value())), Item>);
}

// A type that counts its live objects. It shows the lifetime of a temporary.
struct Counted {
    static inline int live = 0;
    std::vector<int> list{1, 2, 3};
    int n = 9;
    Counted() { ++live; }
    Counted(const Counted& o) : list(o.list), n(o.n) { ++live; }
    Counted(Counted&& o) noexcept : list(std::move(o.list)), n(o.n) { ++live; }
    ~Counted() { --live; }
};

Counted make_counted() { return Counted{}; }

TEST(Deref, ForOverAMemberOfATemporaryKeepsTheTemporaryAlive)
{
    Counted::live = 0;
    int sum = 0;
    int live_in_body = 0;
    for (int a : script::deref(make_counted()).list) {
        sum += a;
        live_in_body = Counted::live;
    }
    EXPECT_EQ(sum, 6);
    EXPECT_EQ(live_in_body, 1);
    EXPECT_EQ(Counted::live, 0);
}

TEST(Deref, ConstReferenceToAMemberOfATemporaryKeepsTheTemporaryAlive)
{
    Counted::live = 0;
    {
        const std::vector<int>& r = script::deref(make_counted()).list;
        EXPECT_EQ(Counted::live, 1);
        EXPECT_EQ(r.size(), 3u);
        EXPECT_EQ(r[2], 3);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(Deref, RvaluePointerStillGivesTheObject)
{
    Item i{8};
    EXPECT_EQ(script::deref(make_ptr(i)).m, 8);
    EXPECT_EQ(&script::deref(make_ptr(i)), &i);
}

TEST(Deref, SharedPtrGivesThePointee)
{
    auto p = std::make_shared<Item>(Item{9});
    EXPECT_EQ(script::deref(p).m, 9);
    EXPECT_EQ(&script::deref(p), p.get());
    const auto cp = p;
    EXPECT_EQ(&script::deref(cp), p.get());
}

TEST(Deref, SharedPtrTemporaryGivesThePointee)
{
    auto make = [] { return std::make_shared<Item>(Item{6}); };
    EXPECT_EQ(script::deref(make()).m, 6);
    EXPECT_EQ(script::deref(make()).get(), 6);
}

TEST(Deref, AssignmentThroughASharedPtr)
{
    auto p = std::make_shared<Item>();
    script::deref(p).m = 12;
    EXPECT_EQ(p->m, 12);
}

// An array has no overload that works:"script::deref(arr)" is deleted. The compiler
// rejects the call, so a test cannot show it. This check shows the same by SFINAE.
template <class T>
concept CanDeref = requires(T& t) { script::deref(t); };
static_assert(!CanDeref<Item[2]>);
static_assert(CanDeref<Item>);

// Tests for script::Intrusive. A new object starts with zero references.
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

using BaseH = script::Intrusive<RcBase, &RcBase::AddRef, &RcBase::Release>;
using DerivedH = script::Intrusive<RcDerived, &RcBase::AddRef, &RcBase::Release>;

static_assert(std::is_nothrow_move_constructible_v<BaseH>);
static_assert(std::is_nothrow_move_assignable_v<BaseH>);
static_assert(script::is_handle_v<BaseH>);
static_assert(script::is_handle_v<const BaseH&>);
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
    script::deref(h).x = 5;
    EXPECT_EQ(h->x, 5);
    EXPECT_EQ(script::deref(h).x, 5);
    EXPECT_EQ((*h).x, 5);
    const BaseH& ch = h;
    EXPECT_EQ(script::deref(ch).x, 5);
    EXPECT_EQ(script::deref(BaseH(h)).x, 5);
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
        script::deref(b).x = 9;
        EXPECT_EQ(d->x, 9);
        BaseH c = std::move(d);  // Move form.
        EXPECT_TRUE(d == nullptr);
        EXPECT_EQ(raw->refs, 2);
        EXPECT_TRUE(c == b);
    }
    EXPECT_EQ(RcBase::live, 0);
}

// ---------------------------------------------------------------- script::ref_arg
// The script writes "ref e" as "script::ref_arg(e)". The native signature picks the form.
int read_ref(Item& i) { return i.m; }
void write_ref(Item& i) { i.m = 99; }
int read_const_ref(const Item& i) { return i.m; }
int read_ptr(Item* p) { return p->m; }
void write_ptr(Item* p) { p->m = 42; }
int read_int_ref(int& v) { return ++v; }

template <class T>
concept CanRefArg = requires(T&& t) { script::ref_arg(static_cast<T&&>(t)); };

struct Holder {
    Item* target = nullptr;
};

TEST(RefArg, BindsToAReferenceParameter)
{
    Item i{5};
    EXPECT_EQ(read_ref(script::ref_arg(i)), 5);
    EXPECT_EQ(read_const_ref(script::ref_arg(i)), 5);
    write_ref(script::ref_arg(i));
    EXPECT_EQ(i.m, 99);
    int n = 1;
    EXPECT_EQ(read_int_ref(script::ref_arg(n)), 2);
    EXPECT_EQ(n, 2);
}

TEST(RefArg, BindsToAPointerParameter)
{
    Item i{6};
    EXPECT_EQ(read_ptr(script::ref_arg(i)), 6);
    write_ptr(script::ref_arg(i));
    EXPECT_EQ(i.m, 42);
}

TEST(RefArg, AssignsToAPointerField)
{
    Item i{8};
    Holder h;
    h.target = script::ref_arg(i);
    EXPECT_EQ(h.target, &i);
    Holder* hp = &h;
    hp->target = nullptr;
    hp->target = script::ref_arg(i);
    EXPECT_EQ(hp->target->m, 8);
}

TEST(RefArg, KeepsConstness)
{
    const Item i{3};
    EXPECT_EQ(read_const_ref(script::ref_arg(i)), 3);
    const Item* p = script::ref_arg(i);
    EXPECT_EQ(p, &i);
    static_assert(!std::is_invocable_v<decltype(&write_ref), decltype(script::ref_arg(i))>);
}

TEST(RefArg, ConstexprAndNoTemporary)
{
    static constexpr int k = 4;
    static_assert(script::ref_arg(k).operator const int&() == 4);
    static_assert(*script::ref_arg(k).operator const int*() == 4);
    static_assert(CanRefArg<Item&>);
    static_assert(!CanRefArg<Item>);
}

}  // namespace
