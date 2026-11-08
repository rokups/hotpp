// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot::CheckedPtr, the weak pointer to a native object.
#include "hot/runtime.h"

#include "engine/Equip.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>

// A class with no enable_shared_from_this base. The weak source is the helper shared_weak_ref.
struct Gem
{
    int cut = 0;
};

namespace hot
{
template <> struct weak_ref_traits<Gem> : shared_weak_ref<Gem>
{
};
} // namespace hot

namespace
{

// A native function that takes a raw pointer.
int power_of(const Charm* c)
{
    return c ? c->power : -1;
}

TEST(CheckedPtr, TraitsDetectTheWeakSource)
{
    static_assert(hot::has_weak_ref_v<Charm>);
    static_assert(hot::has_weak_ref_v<const Charm>);
    static_assert(hot::has_weak_ref_v<Blade>);
    static_assert(!hot::has_weak_ref_v<Rune>);
    SUCCEED();
}

TEST(CheckedPtr, EmptyPointerIsNull)
{
    hot::CheckedPtr<Charm> p;
    EXPECT_EQ(p.get(), nullptr);
    EXPECT_FALSE(static_cast<bool>(p));
    hot::CheckedPtr<Charm> q = nullptr;
    EXPECT_EQ(q.get(), nullptr);
}

// The cast "(T*)fld" of a weak native field is a checked unwrap: a null target throws.
TEST(CheckedPtr, CastPtrIsACheckedUnwrap)
{
    hot::CheckedPtr<Charm> n;
    EXPECT_THROW(hot::cast_ptr<Charm*>(n), hot::NullReference);
    EXPECT_THROW(hot::cast_ptr<const Charm*>(n), hot::NullReference);
    auto strong = std::make_shared<Charm>();
    hot::CheckedPtr<Charm> p = strong;
    EXPECT_EQ(hot::cast_ptr<Charm*>(p), strong.get());
}

TEST(CheckedPtr, LiveReadFromHandle)
{
    auto strong = std::make_shared<Charm>();
    strong->power = 7;
    hot::CheckedPtr<Charm> p = strong;
    EXPECT_EQ(p.get(), strong.get());
    EXPECT_TRUE(static_cast<bool>(p));
    EXPECT_EQ(p->power, 7);
    EXPECT_EQ((*p).power, 7);
    p->power = 9;
    EXPECT_EQ(strong->power, 9);
    EXPECT_EQ(power_of(p), 9);
}

TEST(CheckedPtr, LiveReadFromRawPointer)
{
    auto strong = std::make_shared<Charm>();
    strong->power = 3;
    hot::CheckedPtr<Charm> p = strong.get();
    EXPECT_EQ(p.get(), strong.get());
    EXPECT_EQ(p->power, 3);
    hot::CheckedPtr<Charm> q;
    q = strong.get();
    EXPECT_EQ(q.get(), strong.get());
}

TEST(CheckedPtr, ReadAfterDeathGivesNull)
{
    hot::CheckedPtr<Charm> p;
    {
        auto strong = std::make_shared<Charm>();
        p = strong;
        EXPECT_NE(p.get(), nullptr);
    }
    EXPECT_EQ(p.get(), nullptr);
    EXPECT_FALSE(static_cast<bool>(p));
    EXPECT_TRUE(p == nullptr);
    EXPECT_EQ(power_of(p), -1);
}

TEST(CheckedPtr, ArrowOnNullThrows)
{
    hot::CheckedPtr<Charm> p;
    EXPECT_THROW((void)p->power, hot::NullReference);
    EXPECT_THROW((void)*p, hot::NullReference);
}

TEST(CheckedPtr, ArrowOnExpiredThrows)
{
    hot::CheckedPtr<Charm> p;
    {
        auto strong = std::make_shared<Charm>();
        p = strong;
    }
    EXPECT_THROW((void)p->power, hot::NullReference);
    EXPECT_THROW((void)*p, hot::NullReference);
}

TEST(CheckedPtr, NoOwnerThrows)
{
    Charm stack_charm;
    hot::CheckedPtr<Charm> p;
    try
    {
        p = &stack_charm;
        FAIL() << "no exception";
    }
    catch (const hot::NullReference& e)
    {
        EXPECT_EQ(std::string(e.what()),
            "the object has no owner: a pointer field needs an object that a handle owns");
    }
    EXPECT_EQ(p.get(), nullptr);
    EXPECT_THROW(hot::CheckedPtr<Charm>{&stack_charm}, hot::NullReference);
}

TEST(CheckedPtr, NullRawPointerGivesEmpty)
{
    Charm* raw = nullptr;
    hot::CheckedPtr<Charm> p = raw;
    EXPECT_EQ(p.get(), nullptr);
    auto strong = std::make_shared<Charm>();
    p = strong;
    p = raw;
    EXPECT_EQ(p.get(), nullptr);
}

TEST(CheckedPtr, ProxyKeepsTheObjectAliveDuringTheStatement)
{
    auto strong = std::make_shared<Charm>();
    std::weak_ptr<Charm> watch = strong;
    hot::CheckedPtr<Charm> p = strong;
    auto drop_and_read = [&](int)
    {
        strong.reset();
        // The proxy of "p->" holds the object, so it is still alive here.
        return !watch.expired();
    };
    bool alive = false;
    // The argument is evaluated while the proxy lives.
    alive = (p->power + 0 == 0) && drop_and_read(p->power);
    EXPECT_TRUE(alive);
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(p.get(), nullptr);
}

TEST(CheckedPtr, CopyAndMove)
{
    auto strong = std::make_shared<Charm>();
    hot::CheckedPtr<Charm> a = strong;
    hot::CheckedPtr<Charm> b = a;
    EXPECT_EQ(b.get(), strong.get());
    hot::CheckedPtr<Charm> c;
    c = a;
    EXPECT_EQ(c.get(), strong.get());
    hot::CheckedPtr<Charm> d = std::move(b);
    EXPECT_EQ(d.get(), strong.get());
    hot::CheckedPtr<Charm> e;
    e = std::move(c);
    EXPECT_EQ(e.get(), strong.get());
    e = nullptr;
    EXPECT_EQ(e.get(), nullptr);
    EXPECT_EQ(a.get(), strong.get());
}

TEST(CheckedPtr, ConstObject)
{
    auto strong = std::make_shared<Charm>();
    strong->power = 4;
    hot::CheckedPtr<const Charm> p = strong;
    EXPECT_EQ(p->power, 4);
    EXPECT_EQ((*p).power, 4);
    static_assert(std::is_same_v<decltype(p.get()), const Charm*>);
    const Charm* raw = strong.get();
    hot::CheckedPtr<const Charm> q = raw;
    EXPECT_EQ(q.get(), strong.get());
    std::shared_ptr<const Charm> cs = strong;
    hot::CheckedPtr<const Charm> r = cs;
    EXPECT_EQ(r.get(), strong.get());
    strong.reset();
    cs.reset();
    EXPECT_EQ(p.get(), nullptr);
    EXPECT_THROW((void)p->power, hot::NullReference);
}

TEST(CheckedPtr, DerivedClass)
{
    auto blade = std::make_shared<Blade>();
    blade->power = 5;
    hot::CheckedPtr<Blade> p = blade.get();
    EXPECT_EQ(p.get(), blade.get());
    EXPECT_EQ(p->power, 5);
    // A derived pointer converts to the base pointer.
    hot::CheckedPtr<Charm> base = static_cast<Charm*>(blade.get());
    EXPECT_EQ(base.get(), static_cast<Charm*>(blade.get()));
    blade.reset();
    EXPECT_EQ(p.get(), nullptr);
}

TEST(CheckedPtr, Comparisons)
{
    auto one = std::make_shared<Charm>();
    auto two = std::make_shared<Charm>();
    hot::CheckedPtr<Charm> p = one;
    hot::CheckedPtr<Charm> empty;
    EXPECT_TRUE(p == one.get());
    EXPECT_TRUE(one.get() == p);
    EXPECT_TRUE(p != two.get());
    EXPECT_TRUE(two.get() != p);
    EXPECT_FALSE(p == nullptr);
    EXPECT_FALSE(nullptr == p);
    EXPECT_TRUE(p != nullptr);
    EXPECT_TRUE(nullptr != p);
    EXPECT_TRUE(empty == nullptr);
    EXPECT_TRUE(nullptr == empty);
    EXPECT_FALSE(empty != nullptr);
    EXPECT_FALSE(nullptr != empty);
    hot::CheckedPtr<Charm> same = one;
    EXPECT_TRUE(p == same);
    EXPECT_TRUE(p != empty);
    const Charm* craw = one.get();
    EXPECT_TRUE(p == craw);
}

// The translator writes "hot::deref(p).M()" for "p.M()", and hot::as_ptr and hot::as_ref for a
// native parameter. A CheckedPtr works as a pointer in all three.
TEST(CheckedPtr, HelpersTreatItAsAPointer)
{
    auto strong = std::make_shared<Charm>();
    strong->power = 3;
    hot::CheckedPtr<Charm> p = strong;
    hot::CheckedPtr<Charm> empty;
    EXPECT_EQ(hot::deref(p).Twice(), 6);
    EXPECT_EQ(&hot::deref(p), strong.get());
    EXPECT_EQ(hot::as_ptr(p), strong.get());
    EXPECT_EQ(hot::as_ptr(empty), nullptr);
    EXPECT_EQ(&hot::as_ref(p), strong.get());
    EXPECT_EQ(&hot::star(p), strong.get());
    EXPECT_EQ(power_of(hot::as_ptr(p)), 3);
    EXPECT_THROW({ (void)hot::deref(empty); }, hot::NullReference);
    EXPECT_THROW({ (void)hot::as_ref(empty); }, hot::NullReference);
    strong.reset();
    EXPECT_THROW({ (void)hot::deref(p); }, hot::NullReference);
    EXPECT_EQ(hot::as_ptr(p), nullptr);
}

// The helper base shared_weak_ref gives a weak source to a class with no shared_from_this. It has
// no from_raw, so a handle is the only source.
TEST(CheckedPtr, SharedWeakRefHelperGivesAWeakSource)
{
    static_assert(hot::has_weak_ref_v<Gem>);
    auto strong = std::make_shared<Gem>();
    strong->cut = 8;
    hot::CheckedPtr<Gem> p = strong;
    EXPECT_EQ(p->cut, 8);
    hot::CheckedPtr<Gem> q;
    q = strong;
    EXPECT_EQ(q.get(), strong.get());
    strong.reset();
    EXPECT_EQ(p.get(), nullptr);
    EXPECT_THROW((void)p->cut, hot::NullReference);
}

// A custom handle pair goes through custom_weak_ref (the stub of a handle type does the same).
struct Pearl
{
    int size = 0;
};
} // namespace

namespace hot
{
template <> struct weak_ref_traits<Pearl> : custom_weak_ref<Pearl, std::shared_ptr<Pearl>, std::weak_ptr<Pearl>>
{
};
} // namespace hot

namespace
{
TEST(CheckedPtr, CustomWeakRefHelperGivesAWeakSource)
{
    auto strong = std::make_shared<Pearl>();
    strong->size = 3;
    hot::CheckedPtr<Pearl> p = strong;
    EXPECT_EQ(p->size, 3);
    strong.reset();
    EXPECT_EQ(p.lock(), nullptr);
}

// The reference of "*" on a const pointer has the const type of the pointee.
TEST(CheckedPtr, DerefOfAConstPointerIsConst)
{
    auto strong = std::make_shared<Charm>();
    hot::CheckedPtr<const Charm> p = strong;
    static_assert(std::is_same_v<decltype(*p), const Charm&>);
    hot::CheckedPtr<Charm> m = strong;
    static_assert(std::is_same_v<decltype(*m), Charm&>);
    EXPECT_EQ(&*p, strong.get());
}

TEST(CheckedPtr, SelfAssignKeepsTheTarget)
{
    auto strong = std::make_shared<Charm>();
    hot::CheckedPtr<Charm> p = strong;
    hot::CheckedPtr<Charm>& alias = p;
    p = alias;
    EXPECT_EQ(p.get(), strong.get());
    p = std::move(alias);
    EXPECT_EQ(p.get(), strong.get());
    p = strong;
    EXPECT_EQ(p.get(), strong.get());
}

// An expired pointer compares equal to null and to another expired or empty pointer.
TEST(CheckedPtr, ExpiredPointerComparesAsNull)
{
    hot::CheckedPtr<Charm> p, empty;
    hot::CheckedPtr<Charm> other;
    {
        auto one = std::make_shared<Charm>();
        auto two = std::make_shared<Charm>();
        p = one;
        other = two;
        EXPECT_TRUE(p != other);
    }
    EXPECT_TRUE(p == nullptr);
    EXPECT_TRUE(nullptr == p);
    EXPECT_FALSE(p != nullptr);
    EXPECT_TRUE(p == empty);
    EXPECT_TRUE(p == other);
    Charm* none = nullptr;
    EXPECT_TRUE(p == none);
}

// A pointer to T converts to a pointer to const T. It shares the weak reference.
TEST(CheckedPtr, ConvertsToAPointerToConst)
{
    auto strong = std::make_shared<Charm>();
    strong->power = 6;
    hot::CheckedPtr<Charm> p = strong;
    hot::CheckedPtr<const Charm> c = p;
    EXPECT_EQ(c->power, 6);
    c = p;
    EXPECT_EQ(c.get(), strong.get());
    static_assert(!std::is_convertible_v<hot::CheckedPtr<const Charm>, hot::CheckedPtr<Charm>>);
    hot::CheckedPtr<Charm> empty;
    hot::CheckedPtr<const Charm> ce = empty;
    EXPECT_EQ(ce.get(), nullptr);
    strong.reset();
    EXPECT_EQ(c.get(), nullptr);
}

// The raw pointer conversion lets "delete p" compile (the object belongs to a handle): the test
// only records that the conversion gives the raw pointer. No test deletes through it.
TEST(CheckedPtr, ConversionToTheRawPointerIsBorrowed)
{
    auto strong = std::make_shared<Charm>();
    hot::CheckedPtr<Charm> p = strong;
    Charm* raw = p;
    EXPECT_EQ(raw, strong.get());
}

} // namespace
