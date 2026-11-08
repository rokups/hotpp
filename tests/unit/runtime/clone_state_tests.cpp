// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of clone_state, clone_shell and hot::Value<T>. The tests use hand-made records and
// layouts, as the generated code does.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <typeinfo>
#include <utility>

namespace
{

int g_script_ctor = 0;
int g_dtor = 0;
int g_throw_after = -1; // the Counted copy throws when the number of copies reaches this value
int g_copies = 0;

// A field type that counts its copies and its destructions. It can throw in a copy.
struct Counted
{
    int v;
    explicit Counted(int x) : v(x) {}
    Counted(const Counted& o) : v(o.v)
    {
        if (++g_copies == g_throw_after)
            throw std::runtime_error("copy failed");
        ++g_live_counted;
    }
    ~Counted() { --g_live_counted; }
    static int g_live_counted;
};
int Counted::g_live_counted = 0;

// A field type that cannot be copied.
struct Unique
{
    std::unique_ptr<int> p;
};

struct CellState
{
    hot::ShellBase* owner;
    std::string name;
    Counted a;
    Counted b;
};

void init_name(void*, void* f)
{
    new (f) std::string("hello");
}
void init_a(void*, void* f)
{
    new (f) Counted(1);
    ++Counted::g_live_counted;
}
void init_b(void*, void* f)
{
    new (f) Counted(2);
    ++Counted::g_live_counted;
}
const hot::FieldInfo kFields[] = {
    {"name", nullptr, "name", offsetof(CellState, name), sizeof(std::string), alignof(std::string),
        hot::field_flags<std::string>(), init_name, hot::move_field<std::string>,
        hot::field_destroyer<std::string>(), nullptr, nullptr, nullptr, hot::field_copy<std::string>()},
    {"a", nullptr, "a", offsetof(CellState, a), sizeof(Counted), alignof(Counted), hot::field_flags<Counted>(),
        init_a, hot::move_field<Counted>, hot::field_destroyer<Counted>(), nullptr, nullptr, nullptr,
        hot::field_copy<Counted>()},
    {"b", nullptr, "b", offsetof(CellState, b), sizeof(Counted), alignof(Counted), hot::field_flags<Counted>(),
        init_b, hot::move_field<Counted>, hot::field_destroyer<Counted>(), nullptr, nullptr, nullptr,
        hot::field_copy<Counted>()},
};
const hot::StateLayout kLayout = {"CellState", sizeof(CellState), alignof(CellState), 3, kFields, true};

extern hot::ClassRecord cell_record;
void cell_dtor(hot::ShellCore*)
{
    ++g_dtor;
}

struct Cell_Shell
    : hot::PlainObject
    , hot::ShellBase
{
    explicit Cell_Shell(hot::Construct)
    {
        ++g_script_ctor;
        hot::init_state(this, &cell_record);
    }
    explicit Cell_Shell(hot::ConstructChild) {}
    explicit Cell_Shell(hot::CloneFrom, const hot::ShellBase* src) { hot::clone_state(this, &cell_record, src); }
    ~Cell_Shell() override { hot::destroy_state(this); }
    CellState& st() { return *static_cast<CellState*>(state); }
};

hot::ShellBase* cell_construct(void* mem, const char* key, const void* args)
{
    if (std::strcmp(key, hot::kCloneKey) == 0)
    {
        const hot::ShellBase* src = *static_cast<const hot::ShellBase* const*>(args);
        return mem ? ::new (mem) Cell_Shell(hot::CloneFrom{}, src) : new Cell_Shell(hot::CloneFrom{}, src);
    }
    if (std::strcmp(key, typeid(std::tuple<>).name()) == 0)
        return mem ? ::new (mem) Cell_Shell(hot::Construct{}) : new Cell_Shell(hot::Construct{});
    return nullptr;
}
const hot::SlotFn kSlots[1] = {nullptr};
const char* const kKeys[] = {hot::kCloneKey, nullptr};
hot::ClassRecord cell_record = {"Cell", 1, kSlots, 0, &kLayout, nullptr, "hot::PlainObject", 0, nullptr,
    &cell_construct, nullptr, nullptr, nullptr, &cell_dtor};

// A derived class: the state is a Cell state, and the shell type is the same. The record differs.
extern hot::ClassRecord sub_record;
struct Sub_Shell final : Cell_Shell
{
    explicit Sub_Shell(hot::Construct) : Cell_Shell(hot::ConstructChild{})
    {
        ++g_script_ctor;
        hot::init_state(this, &sub_record);
    }
    explicit Sub_Shell(hot::CloneFrom, const hot::ShellBase* src) : Cell_Shell(hot::ConstructChild{})
    {
        hot::clone_state(this, &sub_record, src);
    }
};
hot::ShellBase* sub_construct(void* mem, const char* key, const void* args)
{
    if (std::strcmp(key, hot::kCloneKey) == 0)
    {
        const hot::ShellBase* src = *static_cast<const hot::ShellBase* const*>(args);
        return mem ? ::new (mem) Sub_Shell(hot::CloneFrom{}, src) : new Sub_Shell(hot::CloneFrom{}, src);
    }
    if (std::strcmp(key, typeid(std::tuple<>).name()) == 0)
        return mem ? ::new (mem) Sub_Shell(hot::Construct{}) : new Sub_Shell(hot::Construct{});
    return nullptr;
}
hot::ClassRecord sub_record = {"Sub", 1, kSlots, 0, &kLayout, &cell_record, "hot::PlainObject", 0, nullptr,
    &sub_construct, nullptr, nullptr, nullptr, &cell_dtor};

std::shared_ptr<Cell_Shell> make_cell()
{
    std::tuple<> args;
    return hot::make_shared_shell<Cell_Shell>(&cell_record, typeid(std::tuple<>).name(), &args);
}
std::shared_ptr<Cell_Shell> make_sub()
{
    std::tuple<> args;
    auto p = hot::make_shared_shell<Sub_Shell>(&sub_record, typeid(std::tuple<>).name(), &args);
    return p;
}

class CloneState : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_script_ctor = 0;
        g_dtor = 0;
        g_throw_after = -1;
        g_copies = 0;
        cell_record.construct_keys = kKeys;
        sub_record.construct_keys = kKeys;
    }
    void TearDown() override { g_throw_after = -1; }
};

TEST_F(CloneState, CopyIsIndependent)
{
    auto a = make_cell();
    a->st().name = "changed";
    a->st().a.v = 40;
    ASSERT_EQ(g_script_ctor, 1);
    auto b = hot::clone_shell(*a);
    ASSERT_TRUE(b);
    EXPECT_EQ(g_script_ctor, 1) << "a copy must not run the script constructor";
    EXPECT_NE(b->state, a->state);
    EXPECT_EQ(b->cls, a->cls);
    EXPECT_EQ(b->st().name, "changed");
    EXPECT_EQ(b->st().a.v, 40);
    EXPECT_EQ(b->st().owner, static_cast<hot::ShellBase*>(b.get()));
    b->st().name = "other";
    b->st().b.v = 99;
    EXPECT_EQ(a->st().name, "changed");
    EXPECT_EQ(a->st().b.v, 2);
}

TEST_F(CloneState, ScriptDestructorRunsWhenTheCopyDies)
{
    auto a = make_cell();
    {
        auto b = hot::clone_shell(*a);
        EXPECT_EQ(g_dtor, 0);
    }
    EXPECT_EQ(g_dtor, 1);
    a.reset();
    EXPECT_EQ(g_dtor, 2);
    EXPECT_EQ(Counted::g_live_counted, 0);
}

TEST_F(CloneState, ThrowingCopyRollsBack)
{
    auto a = make_cell();
    const int live = Counted::g_live_counted;
    ASSERT_EQ(live, 2);
    g_copies = 0;
    g_throw_after = 2; // the copy of field "b" throws; field "a" and "name" exist already
    EXPECT_THROW(hot::clone_shell(*a), std::runtime_error);
    g_throw_after = -1;
    EXPECT_EQ(Counted::g_live_counted, live) << "the fields that exist must be destroyed";
    EXPECT_EQ(g_dtor, 0) << "the script destructor must not run for an object that never existed";
    // The runtime still works.
    auto c = hot::clone_shell(*a);
    EXPECT_EQ(c->st().b.v, 2);
}

// A layout with a field that has no copy function.
struct UState
{
    hot::ShellBase* owner;
    Unique u;
};
void init_u(void*, void* f)
{
    new (f) Unique{std::make_unique<int>(5)};
}
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
const hot::FieldInfo kUFields[] = {{"u", nullptr, "u", offsetof(UState, u), sizeof(Unique), alignof(Unique),
    hot::field_flags<Unique>(), init_u, hot::move_field<Unique>, hot::field_destroyer<Unique>(), nullptr, nullptr,
    nullptr, hot::field_copy<Unique>()}};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
const hot::StateLayout kULayout = {"UState", sizeof(UState), alignof(UState), 1, kUFields, true};
extern hot::ClassRecord u_record;
struct U_Shell final
    : hot::PlainObject
    , hot::ShellBase
{
    explicit U_Shell(hot::Construct) { hot::init_state(this, &u_record); }
    explicit U_Shell(hot::CloneFrom, const hot::ShellBase* src) { hot::clone_state(this, &u_record, src); }
    ~U_Shell() override { hot::destroy_state(this); }
};
hot::ShellBase* u_construct(void* mem, const char* key, const void* args)
{
    if (std::strcmp(key, hot::kCloneKey) == 0)
    {
        const hot::ShellBase* src = *static_cast<const hot::ShellBase* const*>(args);
        return mem ? ::new (mem) U_Shell(hot::CloneFrom{}, src) : new U_Shell(hot::CloneFrom{}, src);
    }
    if (std::strcmp(key, typeid(std::tuple<>).name()) == 0)
        return mem ? ::new (mem) U_Shell(hot::Construct{}) : new U_Shell(hot::Construct{});
    return nullptr;
}
hot::ClassRecord u_record = {"UCell", 1, kSlots, 0, &kULayout, nullptr, "hot::PlainObject", 0, nullptr,
    &u_construct, nullptr, nullptr, nullptr, nullptr};

TEST_F(CloneState, FieldWithoutCopyThrowsWithItsName)
{
    static_assert(hot::field_copy<Unique>() == nullptr, "a type that cannot be copied has no copy function");
    std::tuple<> args;
    auto u = hot::make_shared_shell<U_Shell>(&u_record, typeid(std::tuple<>).name(), &args);
    try
    {
        hot::clone_shell(*u);
        FAIL() << "the copy must throw";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_STREQ(e.what(), "class UCell is not copyable: field 'u'");
    }
}

TEST_F(CloneState, ModuleWithoutCloneKeyThrows)
{
    auto a = make_cell();
    // A module built before kCloneKey: "construct" does not know the key.
    hot::ClassRecord old = cell_record;
    old.construct = [](void*, const char*, const void*) -> hot::ShellBase* { return nullptr; };
    old.canonical = nullptr;
    a->cls = &old;
    try
    {
        hot::clone_shell(*a);
        FAIL() << "the copy must throw";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_STREQ(e.what(), "class Cell is not copyable: it has no clone constructor; restart the program");
    }
    a->cls = &cell_record;
}

TEST_F(CloneState, CallerRecordSetsTheCreatorGeneration)
{
    auto a = make_cell();
    EXPECT_EQ(hot::clone_shell(*a)->creator, 0u);
    hot::ClassRecord caller = cell_record;
    caller.generation = 7;
    EXPECT_EQ(hot::clone_shell(*a, &caller)->creator, 7u);
}

TEST_F(CloneState, ClassOfTheCopyIsTheRealClass)
{
    auto d = make_sub();
    ASSERT_EQ(d->cls, &sub_record);
    d->st().name = "derived";
    std::shared_ptr<Cell_Shell> c = hot::clone_shell<Cell_Shell>(*d); // through the base type
    EXPECT_EQ(c->cls, &sub_record);
    EXPECT_NE(dynamic_cast<Sub_Shell*>(c.get()), nullptr);
    EXPECT_EQ(c->st().name, "derived");
}

TEST(ValueBox, CopyIsIndependentAndMoveKeepsTheObject)
{
    g_dtor = 0;
    hot::Value<Cell_Shell> a(make_cell());
    a->st().name = "one";
    hot::Value<Cell_Shell> b = a; // a copy
    EXPECT_NE(a.handle().get(), b.handle().get());
    EXPECT_EQ(b->st().name, "one");
    b->st().name = "two";
    EXPECT_EQ(a->st().name, "one");
    EXPECT_EQ((*a).st().name, "one");

    Cell_Shell* raw = a.handle().get();
    hot::Value<Cell_Shell> m = std::move(a);
    EXPECT_EQ(m.handle().get(), raw) << "a move keeps the object";
    hot::Value<Cell_Shell> n(make_cell());
    n = std::move(m);
    EXPECT_EQ(n.handle().get(), raw);

    hot::Value<Cell_Shell> c(make_cell());
    c = b; // copy assignment
    EXPECT_NE(c.handle().get(), b.handle().get());
    EXPECT_EQ(c->st().name, "two");
    const hot::Value<Cell_Shell>& same = c;
    c = same; // self assignment keeps a valid object
    EXPECT_EQ(c->st().name, "two");
}

TEST(ValueBox, NullHandleThrowsNullReference)
{
    EXPECT_THROW((hot::Value<Cell_Shell>{std::shared_ptr<Cell_Shell>()}), hot::NullReference);
    EXPECT_THROW(hot::Value<Cell_Shell>::copy_of(nullptr), hot::NullReference);
}

TEST(ValueBox, CopyOfMakesACopyOfTheHandleObject)
{
    auto h = make_cell();
    h->st().name = "src";
    auto v = hot::Value<Cell_Shell>::copy_of(h);
    EXPECT_NE(v.handle().get(), h.get());
    v->st().name = "dst";
    EXPECT_EQ(h->st().name, "src");
}

TEST(ValueBox, CopyOfDerivedKeepsTheRealClass)
{
    auto d = make_sub();
    hot::Value<Cell_Shell> a(d);
    hot::Value<Cell_Shell> b = a;
    EXPECT_EQ(b->cls, &sub_record);
}

TEST(ValueBox, FailedCopyAssignmentKeepsTheTarget)
{
    std::tuple<> args;
    hot::Value<U_Shell> u(hot::make_shared_shell<U_Shell>(&u_record, typeid(std::tuple<>).name(), &args));
    hot::Value<U_Shell> t(hot::make_shared_shell<U_Shell>(&u_record, typeid(std::tuple<>).name(), &args));
    U_Shell* before = t.handle().get();
    EXPECT_THROW(t = u, std::runtime_error);
    EXPECT_EQ(t.handle().get(), before);
}

// The generated script_register sets the caller record. Then a copy of a Value has the creator
// generation of the module. Without the record, the creator is 0 (V7).
TEST(ValueBox, CopyNotesTheCreatorOfTheCallerRecord)
{
    hot::Value<Cell_Shell> a(make_cell());
    EXPECT_EQ(hot::Value<Cell_Shell>(a)->creator, 0u);
    hot::ClassRecord caller = cell_record;
    caller.generation = 5;
    hot::set_value_caller(&caller);
    hot::Value<Cell_Shell> b = a;
    auto c = hot::Value<Cell_Shell>::copy_of(a.handle());
    hot::set_value_caller(nullptr);
    EXPECT_EQ(b->creator, 5u);
    EXPECT_EQ(c->creator, 5u);
}

// A module with no script class has no record: it names the generation.
TEST(ValueBox, CopyNotesTheCreatorOfTheCallerGeneration)
{
    hot::Value<Cell_Shell> a(make_cell());
    hot::set_value_generation(9);
    hot::Value<Cell_Shell> b = a;
    hot::set_value_generation(0);
    EXPECT_EQ(b->creator, 9u);
    EXPECT_EQ(hot::Value<Cell_Shell>(a)->creator, 0u);
}

} // namespace
