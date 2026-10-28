// Tests for script::ref_arg.
// The script writes "ref e" as "script::ref_arg(e)". The native signature picks the form.
#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>

#include "spp/script_runtime.h"

namespace {

struct Item {
    int m = 0;
    int get() const { return m; }
};

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
