// Tests for hot::deref. The translator writes "a.b" as "hot::deref(a).b".
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

struct Item
{
    int m = 0;
    int get() const { return m; }
};

Item* make_ptr(Item& i)
{
    return &i;
}
Item make_value()
{
    return Item{7};
}

TEST(Deref, PointerGivesTheObject)
{
    Item i{3};
    Item* p = &i;
    EXPECT_EQ(hot::deref(p).m, 3);
    EXPECT_EQ(&hot::deref(p), &i);
}

TEST(Deref, ReferenceGivesTheSameObject)
{
    Item i{4};
    Item& r = i;
    EXPECT_EQ(hot::deref(r).m, 4);
    EXPECT_EQ(&hot::deref(r), &i);
}

TEST(Deref, ValueFromACallWorks)
{
    EXPECT_EQ(hot::deref(make_value()).m, 7);
    EXPECT_EQ(hot::deref(make_value()).get(), 7);
}

TEST(Deref, PointerFromACallWorks)
{
    Item i{5};
    EXPECT_EQ(hot::deref(make_ptr(i)).m, 5);
}

TEST(Deref, AssignmentThroughAPointer)
{
    Item i;
    Item* p = &i;
    hot::deref(p).m = 1;
    EXPECT_EQ(i.m, 1);
}

TEST(Deref, AssignmentThroughAReference)
{
    Item i;
    Item& r = i;
    hot::deref(r).m = 2;
    EXPECT_EQ(i.m, 2);
}

TEST(Deref, PointerLvalueUsesThePointerOverload)
{
    Item i{6};
    Item* p = &i;
    Item*& pr = p;
    const Item* cp = &i;
    static_assert(std::is_same_v<decltype(hot::deref(pr)), Item&>);
    static_assert(std::is_same_v<decltype(hot::deref(cp)), const Item&>);
    static_assert(std::is_same_v<decltype(hot::deref(make_ptr(i))), Item&>);
    EXPECT_EQ(hot::deref(pr).m, 6);
    EXPECT_EQ(hot::deref(cp).m, 6);
}

TEST(Deref, ValueKeepsItsCategory)
{
    Item i;
    const Item ci;
    static_assert(std::is_same_v<decltype(hot::deref(i)), Item&>);
    static_assert(std::is_same_v<decltype(hot::deref(ci)), const Item&>);
    static_assert(std::is_same_v<decltype(hot::deref(make_value())), Item>);
}

// A type that counts its live objects. It shows the lifetime of a temporary.
struct Counted
{
    static inline int live = 0;
    std::vector<int> list{1, 2, 3};
    int n = 9;
    Counted() { ++live; }
    Counted(const Counted& o)
        : list(o.list)
        , n(o.n)
    {
        ++live;
    }
    Counted(Counted&& o) noexcept
        : list(std::move(o.list))
        , n(o.n)
    {
        ++live;
    }
    ~Counted() { --live; }
};

Counted make_counted()
{
    return Counted{};
}

TEST(Deref, ForOverAMemberOfATemporaryKeepsTheTemporaryAlive)
{
    Counted::live = 0;
    int sum = 0;
    int live_in_body = 0;
    for (int a : hot::deref(make_counted()).list)
    {
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
        const std::vector<int>& r = hot::deref(make_counted()).list;
        EXPECT_EQ(Counted::live, 1);
        EXPECT_EQ(r.size(), 3u);
        EXPECT_EQ(r[2], 3);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(Deref, RvaluePointerStillGivesTheObject)
{
    Item i{8};
    EXPECT_EQ(hot::deref(make_ptr(i)).m, 8);
    EXPECT_EQ(&hot::deref(make_ptr(i)), &i);
}

TEST(Deref, SharedPtrGivesThePointee)
{
    auto p = std::make_shared<Item>(Item{9});
    EXPECT_EQ(hot::deref(p).m, 9);
    EXPECT_EQ(&hot::deref(p), p.get());
    const auto cp = p;
    EXPECT_EQ(&hot::deref(cp), p.get());
}

TEST(Deref, SharedPtrTemporaryGivesThePointee)
{
    auto make = [] { return std::make_shared<Item>(Item{6}); };
    EXPECT_EQ(hot::deref(make()).m, 6);
    EXPECT_EQ(hot::deref(make()).get(), 6);
}

TEST(Deref, AssignmentThroughASharedPtr)
{
    auto p = std::make_shared<Item>();
    hot::deref(p).m = 12;
    EXPECT_EQ(p->m, 12);
}

// An array has no overload that works:"hot::deref(arr)" is deleted. The compiler
// rejects the call, so a test cannot show it. This check shows the same by SFINAE.
template <class T> concept CanDeref = requires(T & t)
{
    hot::deref(t);
};
static_assert(!CanDeref<Item[2]>);
static_assert(CanDeref<Item>);

} // namespace
