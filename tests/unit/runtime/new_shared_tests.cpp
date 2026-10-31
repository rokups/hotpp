// Tests of make_shared_shell: the "new" of a script class with a std::shared_ptr handle. One block
// holds the shell and the control block. The tests use a hand-made record, as the generated code does.
#include "hot/runtime.h"

#include "../../../src/runtime/runtime_internal.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <typeinfo>
#include <vector>

namespace
{

struct CellState
{
    hot::ShellBase* owner;
    int a;
};
void init_a(void*, void* f)
{
    new (f) int(7);
}
const hot::FieldInfo kFields[] = {{"a", nullptr, typeid(int).name(), offsetof(CellState, a), sizeof(int),
    alignof(int), hot::field_flags<int>(), init_a, hot::move_field<int>, hot::field_destroyer<int>()}};
const hot::StateLayout kLayout = {"CellState", sizeof(CellState), alignof(CellState), 1, kFields, true};

using Args = std::tuple<>;
extern hot::ClassRecord cell_record;
int g_destroyed = 0;

struct Cell_Shell final
    : hot::PlainObject
    , hot::ShellBase
{
    explicit Cell_Shell(hot::Construct) { hot::init_state(this, &cell_record); }
    ~Cell_Shell() override
    {
        hot::destroy_state(this);
        ++g_destroyed;
    }
};

hot::ShellBase* cell_construct(void* mem, const char* key, const void*)
{
    if (key != typeid(Args).name() && std::strcmp(key, typeid(Args).name()) != 0)
        return nullptr;
    return mem ? ::new (mem) Cell_Shell(hot::Construct{}) : new Cell_Shell(hot::Construct{});
}
const hot::SlotFn kSlots[1] = {nullptr};
hot::ClassRecord cell_record = {"Cell", 3, kSlots, 0, &kLayout, nullptr, "hot::PlainObject", 0, nullptr,
    &cell_construct, typeid(Args).name(), nullptr, nullptr, nullptr};

std::shared_ptr<Cell_Shell> make_cell()
{
    Args args;
    return hot::make_shared_shell<Cell_Shell>(&cell_record, typeid(Args).name(), &args);
}

std::vector<hot::ShellBase*> live_list()
{
    std::vector<hot::ShellBase*> v;
    for (const auto& [s, creator] : hot::g_live)
        v.push_back(s);
    return v;
}

// A class with its own operator new. make_shared_shell keeps "new" of the class for it.
int g_class_news = 0;
extern hot::ClassRecord pooled_record;
struct Pooled_Shell final
    : hot::PlainObject
    , hot::ShellBase
{
    explicit Pooled_Shell(hot::Construct) { hot::init_state(this, &pooled_record); }
    ~Pooled_Shell() override { hot::destroy_state(this); }
    static void* operator new(std::size_t n)
    {
        ++g_class_news;
        return ::operator new(n);
    }
    static void operator delete(void* p) { ::operator delete(p); }
};
hot::ShellBase* pooled_construct(void* mem, const char*, const void*)
{
    return mem ? ::new (mem) Pooled_Shell(hot::Construct{}) : new Pooled_Shell(hot::Construct{});
}
hot::ClassRecord pooled_record = {"Pooled", 1, kSlots, 0, &kLayout, nullptr, "hot::PlainObject", 0, nullptr,
    &pooled_construct, typeid(Args).name(), nullptr, nullptr, nullptr};

TEST(RuntimeNewShared, MakesALiveObjectWithStateAndCreator)
{
    ASSERT_TRUE(hot::g_live.empty());
    {
        auto c = make_cell();
        ASSERT_NE(c, nullptr);
        EXPECT_EQ(hot::g_live.size(), 1u);
        EXPECT_EQ(c->cls, &cell_record);
        EXPECT_EQ(c->creator, 3u); // the generation of the record that made the object
        EXPECT_EQ(static_cast<CellState*>(c->state)->a, 7);
        EXPECT_EQ(static_cast<CellState*>(c->state)->owner, static_cast<hot::ShellBase*>(c.get()));
        EXPECT_EQ(c.use_count(), 1);
    }
    EXPECT_TRUE(hot::g_live.empty());
}

TEST(RuntimeNewShared, ThisAsAHandleSharesTheOwnership)
{
    auto c = make_cell();
    const auto h = hot::handle_from_this<std::shared_ptr<Cell_Shell>>(c.get());
    EXPECT_EQ(h, c);
    EXPECT_EQ(c.use_count(), 2);
}

TEST(RuntimeNewShared, AWeakHandleOutlivesTheObject)
{
    const int destroyed = g_destroyed;
    std::weak_ptr<Cell_Shell> w;
    {
        auto c = make_cell();
        w = c;
        EXPECT_FALSE(w.expired());
    }
    // The destructor and the state block go with the last std::shared_ptr. The list forgets the object.
    EXPECT_TRUE(w.expired());
    EXPECT_EQ(w.lock(), nullptr);
    EXPECT_EQ(g_destroyed, destroyed + 1);
    EXPECT_TRUE(hot::g_live.empty());
}

TEST(RuntimeNewShared, TheListKeepsTheCreationOrder)
{
    auto a = make_cell();
    Args args;
    std::shared_ptr<Cell_Shell> b(
        static_cast<Cell_Shell*>(hot::construct_shell(&cell_record, typeid(Args).name(), &args)));
    auto c = make_cell();
    auto d = make_cell();
    EXPECT_EQ(live_list(), (std::vector<hot::ShellBase*>{a.get(), b.get(), c.get(), d.get()}));
    c.reset();
    EXPECT_EQ(live_list(), (std::vector<hot::ShellBase*>{a.get(), b.get(), d.get()}));
    auto e = make_cell();
    EXPECT_EQ(live_list(), (std::vector<hot::ShellBase*>{a.get(), b.get(), d.get(), e.get()}));
    a.reset();
    b.reset();
    d.reset();
    e.reset();
    EXPECT_TRUE(hot::g_live.empty());
}

TEST(RuntimeNewShared, AnUnknownKeyThrowsAndLeavesNoObject)
{
    Args args;
    EXPECT_THROW(hot::make_shared_shell<Cell_Shell>(&cell_record, "not a key", &args), std::runtime_error);
    EXPECT_TRUE(hot::g_live.empty());
}

// A class whose second field initializer throws. The first field holds heap memory, so a leak shows in ASan.
struct BadState
{
    hot::ShellBase* owner;
    std::string s;
    int b;
};
int g_live_strings = 0;
void init_s(void*, void* f)
{
    new (f) std::string(64, 'x');
    ++g_live_strings;
}
void destroy_s(void* f)
{
    static_cast<std::string*>(f)->~basic_string();
    --g_live_strings;
}
void init_b_throws(void*, void*)
{
    throw std::runtime_error("field initializer");
}
const hot::FieldInfo kBadFields[] = {
    {"s", nullptr, typeid(std::string).name(), offsetof(BadState, s), sizeof(std::string), alignof(std::string),
        hot::field_flags<std::string>(), init_s, hot::move_field<std::string>, destroy_s},
    {"b", nullptr, typeid(int).name(), offsetof(BadState, b), sizeof(int), alignof(int), hot::field_flags<int>(),
        init_b_throws, hot::move_field<int>, hot::field_destroyer<int>()}};
const hot::StateLayout kBadLayout = {"BadState", sizeof(BadState), alignof(BadState), 2, kBadFields, true};
extern hot::ClassRecord bad_field_record;
struct BadField_Shell final
    : hot::PlainObject
    , hot::ShellBase
{
    explicit BadField_Shell(hot::Construct) { hot::init_state(this, &bad_field_record); }
    ~BadField_Shell() override { hot::destroy_state(this); }
};
hot::ShellBase* bad_field_construct(void* mem, const char*, const void*)
{
    return mem ? ::new (mem) BadField_Shell(hot::Construct{}) : new BadField_Shell(hot::Construct{});
}
hot::ClassRecord bad_field_record = {"BadField", 1, kSlots, 0, &kBadLayout, nullptr, "hot::PlainObject", 0,
    nullptr, &bad_field_construct, typeid(Args).name(), nullptr, nullptr, nullptr};

// A class whose native base constructor throws before the shell runs init_state.
struct ThrowingBase
{
    ThrowingBase() { throw std::runtime_error("base constructor"); }
    virtual ~ThrowingBase() = default;
};
extern hot::ClassRecord bad_base_record;
struct BadBase_Shell final
    : ThrowingBase
    , hot::ShellBase
{
    explicit BadBase_Shell(hot::Construct) { hot::init_state(this, &bad_base_record); }
    ~BadBase_Shell() override { hot::destroy_state(this); }
};
hot::ShellBase* bad_base_construct(void* mem, const char*, const void*)
{
    return mem ? ::new (mem) BadBase_Shell(hot::Construct{}) : new BadBase_Shell(hot::Construct{});
}
hot::ClassRecord bad_base_record = {"BadBase", 1, kSlots, 0, &kLayout, nullptr, "ThrowingBase", 0, nullptr,
    &bad_base_construct, typeid(Args).name(), nullptr, nullptr, nullptr};

TEST(RuntimeNewShared, AThrowingFieldInitializerLeavesNoObject)
{
    auto keep = make_cell(); // the list is not empty, so the test sees a change of its size
    const size_t before = hot::g_live.size();
    Args args;
    for (int round = 0; round < 3; ++round)
    {
        try
        {
            hot::make_shared_shell<BadField_Shell>(&bad_field_record, typeid(Args).name(), &args);
            ADD_FAILURE() << "the exception did not reach the caller";
        }
        catch (const std::runtime_error& e)
        {
            EXPECT_STREQ(e.what(), "field initializer");
        }
        EXPECT_EQ(hot::g_live.size(), before);
        EXPECT_EQ(g_live_strings, 0); // the first field is destroyed again
    }
    EXPECT_EQ(live_list(), (std::vector<hot::ShellBase*>{keep.get()}));
}

TEST(RuntimeNewShared, AThrowingBaseConstructorLeavesNoObject)
{
    auto keep = make_cell();
    const size_t before = hot::g_live.size();
    Args args;
    for (int round = 0; round < 3; ++round)
    {
        try
        {
            hot::make_shared_shell<BadBase_Shell>(&bad_base_record, typeid(Args).name(), &args);
            ADD_FAILURE() << "the exception did not reach the caller";
        }
        catch (const std::runtime_error& e)
        {
            EXPECT_STREQ(e.what(), "base constructor");
        }
        EXPECT_EQ(hot::g_live.size(), before);
    }
    EXPECT_EQ(live_list(), (std::vector<hot::ShellBase*>{keep.get()}));
}

TEST(RuntimeNewShared, AClassWithItsOwnOperatorNewUsesIt)
{
    const int news = g_class_news;
    Args args;
    {
        auto p = hot::make_shared_shell<Pooled_Shell>(&pooled_record, typeid(Args).name(), &args);
        EXPECT_EQ(g_class_news, news + 1);
        EXPECT_EQ(p->creator, 1u);
        EXPECT_EQ(hot::g_live.size(), 1u);
    }
    EXPECT_TRUE(hot::g_live.empty());
}

// An object made on a worker thread and freed on the main thread. The threads do not overlap:
// the list of live objects has no lock (see LiveObjects in runtime_internal.h).
TEST(RuntimeNewShared, AnObjectCanMoveToAnotherThread)
{
    std::shared_ptr<Cell_Shell> c;
    std::thread t([&] { c = make_cell(); });
    t.join();
    EXPECT_EQ(hot::g_live.size(), 1u);
    c.reset();
    EXPECT_TRUE(hot::g_live.empty());
}

// Many threads create and destroy objects. The host serializes the creation and the destruction
// with its own lock, as the thread rule says (doc/internals.md). The walk on the main thread
// then gives the objects in the order of creation, and the count is exact.
TEST(RuntimeNewShared, ManyThreadsWithTheHostLock)
{
    constexpr int kThreads = 6;
    constexpr int kRounds = 2000;
    constexpr int kKeep = 5; // the objects that each thread keeps to the end
    std::mutex host_lock;
    std::vector<hot::ShellBase*> kept_order; // the order of creation of the kept objects
    std::vector<std::vector<std::shared_ptr<Cell_Shell>>> kept(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t]
        {
            for (int i = 0; i < kRounds; ++i)
            {
                std::shared_ptr<Cell_Shell> c;
                {
                    std::lock_guard<std::mutex> lock(host_lock);
                    c = make_cell();
                    if (i % (kRounds / kKeep) == 0)
                    {
                        kept[t].push_back(c);
                        kept_order.push_back(c.get());
                    }
                }
                static_cast<CellState*>(c->state)->a += i; // a field write needs no lock
                std::lock_guard<std::mutex> lock(host_lock);
                c.reset();
            }
        });
    for (std::thread& th : threads)
        th.join();
    EXPECT_EQ(hot::g_live.size(), size_t(kThreads * kKeep));
    EXPECT_EQ(live_list(), kept_order);
    kept.clear();
    EXPECT_TRUE(hot::g_live.empty());
}

} // namespace
