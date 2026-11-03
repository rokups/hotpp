// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the calls of script interface methods by name (hot/runtime/iface.h): the method ids,
// the call through the class record, the conversions, and the tables that the loader builds.
#include "hot/backend.h"
#include "hot/runtime.h"
#include "hot/runtime/iface.h"
#include "support/reload_assert.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <typeinfo>

namespace
{

struct IAdd; // the tag of the interface "IAdd"
struct IOther;

struct Base
{
    virtual ~Base() = default;
};
struct Cell_Shell final
    : Base
    , hot::ShellBase
    , std::enable_shared_from_this<Cell_Shell>
{
};

// The script state: the first field is the owner pointer. The slots are members of it.
struct TestState
{
    hot::ShellBase* owner = nullptr;
    int add_one(int x) { return x + 1; }
    int add_ten(int x) { return x + 10; }
    void touch(int* hits) { ++*hits; }
};

template <class Pmf> hot::SlotFn slot_of(Pmf p)
{
    return reinterpret_cast<hot::SlotFn>(p);
}

const char* const kAddSig = typeid(int(int)).name();
const char* const kTouchSig = typeid(void(int*)).name();

// A record of a class with the table "iface_slots" that the test builds by hand.
struct Fixture : ::testing::Test
{
    hot::SlotFn slots[3] = {slot_of(&TestState::add_one), slot_of(&TestState::add_ten), slot_of(&TestState::touch)};
    const char* const names[2] = {"IAdd", nullptr};
    std::vector<uint32_t> table;
    hot::ClassRecord rec{};
    hot::ClassRecord child{};
    TestState state;
    std::atomic<uint32_t> site{0};

    Fixture()
    {
        rec.name = "Cell";
        rec.slots = slots;
        rec.slot_count = 3;
        rec.ifaces = names;
        child.name = "Child";
        child.slots = slots;
        child.slot_count = 3;
        child.super = &rec;
        const uint32_t add = hot::iface_method_id("IAdd.Add", kAddSig);
        const uint32_t touch = hot::iface_method_id("IAdd.Touch", kTouchSig);
        table.assign(std::max(add, touch) + 1, hot::kNoIfaceSlot);
        table[add] = 0;
        table[touch] = 2;
        rec.iface_slots = table.data();
        rec.iface_slot_count = static_cast<uint32_t>(table.size());
        // The rows of Child list the inherited methods (the contract of IfaceRow): the same table.
        child.iface_slots = table.data();
        child.iface_slot_count = rec.iface_slot_count;
    }
    std::shared_ptr<Cell_Shell> make_cell(hot::ClassRecord* r)
    {
        auto cell = std::make_shared<Cell_Shell>();
        cell->cls = r;
        cell->state = &state;
        state.owner = cell.get();
        return cell;
    }
};

// A handle of another type: it counts its live copies.
int g_copies = 0;
struct CountedHandle
{
    std::shared_ptr<Cell_Shell> p;
    CountedHandle() { ++g_copies; }
    explicit CountedHandle(std::shared_ptr<Cell_Shell> q) : p(std::move(q)) { ++g_copies; }
    CountedHandle(const CountedHandle& o) : p(o.p) { ++g_copies; }
    ~CountedHandle() { --g_copies; }
    Cell_Shell* get() const { return p.get(); }
};

} // namespace

TEST(IfaceId, IsStableAndNeverZero)
{
    const uint32_t a = hot::iface_method_id("IdTest.A", kAddSig);
    EXPECT_GE(a, 1u);
    EXPECT_EQ(hot::iface_method_id("IdTest.A", kAddSig), a);
    const std::string copy = "IdTest.A"; // another pointer, the same text
    EXPECT_EQ(hot::iface_method_id(copy.c_str(), kAddSig), a);
    const uint32_t b = hot::iface_method_id("IdTest.B", kAddSig);
    const uint32_t c = hot::iface_method_id("IdTest.A", kTouchSig);
    EXPECT_NE(b, a);
    EXPECT_NE(c, a);
    EXPECT_NE(c, b);
    EXPECT_EQ(hot::iface_method_id("IdTest.A", kAddSig), a);
}

TEST_F(Fixture, CallThroughTheTable)
{
    auto cell = make_cell(&rec);
    const hot::Iface<IAdd> i = hot::to_iface<IAdd>(cell);
    ASSERT_TRUE(i);
    EXPECT_EQ(site.load(), 0u);
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 5);
    EXPECT_EQ(site.load(), hot::iface_method_id("IAdd.Add", kAddSig));
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 9)), 10); // the cached id
    std::atomic<uint32_t> site2{0};
    int hits = 0;
    hot::call_iface<void(int*)>(i, site2, "IAdd.Touch", &hits);
    EXPECT_EQ(hits, 1);
}

// A child class lists the inherited methods in its own rows, so the call through a child works.
TEST_F(Fixture, CallThroughAChildClass)
{
    auto kid = make_cell(&child);
    const hot::Iface<IAdd> i = hot::as_iface<IAdd>(kid, "IAdd");
    ASSERT_TRUE(i);
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 5);
    int hits = 0;
    std::atomic<uint32_t> site2{0};
    hot::call_iface<void(int*)>(i, site2, "IAdd.Touch", &hits);
    EXPECT_EQ(hits, 1);
}

TEST_F(Fixture, MissingSlotThrows)
{
    auto cell = make_cell(&rec);
    const hot::Iface<IAdd> i = hot::to_iface<IAdd>(cell);
    // The id is inside the table, but the slot is empty.
    const uint32_t hole = hot::iface_method_id("IAdd.Hole", kAddSig);
    table.resize(std::max<size_t>(table.size(), hole + 1), hot::kNoIfaceSlot);
    rec.iface_slots = table.data();
    rec.iface_slot_count = static_cast<uint32_t>(table.size());
    std::atomic<uint32_t> s1{0};
    try
    {
        hot::call_iface<int(int)>(i, s1, "IAdd.Hole", 1);
        FAIL() << "no exception";
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_EQ(std::string(e.what()), "class 'Cell' does not implement 'IAdd.Hole'");
    }
    // The id is outside the table.
    const uint32_t far = hot::iface_method_id("IAdd.Far", kAddSig);
    ASSERT_GE(far, rec.iface_slot_count);
    std::atomic<uint32_t> s2{0};
    EXPECT_THROW(hot::call_iface<int(int)>(i, s2, "IAdd.Far", 1), hot::ScriptError);
    // A class without a table.
    hot::ClassRecord bare{};
    bare.name = "Bare";
    auto other = make_cell(&bare);
    std::atomic<uint32_t> s3{0};
    EXPECT_THROW(hot::call_iface<int(int)>(hot::to_iface<IAdd>(other), s3, "IAdd.Add", 1), hot::ScriptError);
}

TEST_F(Fixture, NullHandleThrows)
{
    const hot::Iface<IAdd> none;
    EXPECT_FALSE(none);
    try
    {
        hot::call_iface<int(int)>(none, site, "IAdd.Add", 1);
        FAIL() << "no exception";
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_EQ(std::string(e.what()), "null interface handle 'IAdd'");
    }
}

TEST_F(Fixture, IsAsAndBack)
{
    auto cell = make_cell(&rec);
    auto kid = make_cell(&child);
    const std::shared_ptr<Base> base = kid;
    EXPECT_TRUE(hot::is_iface(cell, "IAdd"));
    EXPECT_FALSE(hot::is_iface(cell, "IOther"));
    EXPECT_TRUE(hot::is_iface(kid, "IAdd")); // the parent record lists it
    EXPECT_TRUE(hot::is_iface(base, "IAdd"));
    EXPECT_TRUE(hot::is_iface(kid.get(), "IAdd"));
    EXPECT_FALSE(hot::is_iface(std::shared_ptr<Base>(), "IAdd"));

    const hot::Iface<IAdd> i = hot::as_iface<IAdd>(cell, "IAdd");
    ASSERT_TRUE(i);
    EXPECT_EQ(i.get(), static_cast<hot::ShellCore*>(cell.get()));
    EXPECT_FALSE(hot::as_iface<IOther>(cell, "IOther"));
    EXPECT_TRUE(hot::as_iface<IAdd>(base, "IAdd"));

    const auto c = hot::iface_as<std::shared_ptr<Cell_Shell>, Cell_Shell>(i, "Cell");
    EXPECT_EQ(c, cell);
    EXPECT_EQ((hot::iface_as<std::shared_ptr<Cell_Shell>, Cell_Shell>(i, "Child")), nullptr);
    EXPECT_EQ((hot::iface_as<std::shared_ptr<Cell_Shell>, Cell_Shell>(hot::Iface<IAdd>(), "Cell")), nullptr);
    const hot::Iface<IAdd> ik = hot::to_iface<IAdd>(kid);
    EXPECT_EQ((hot::iface_as<std::shared_ptr<Cell_Shell>, Cell_Shell>(ik, "Cell")), kid); // "Cell" is a parent
}

TEST_F(Fixture, AliasingKeepsTheReferenceCount)
{
    auto cell = make_cell(&rec);
    std::weak_ptr<Cell_Shell> weak = cell;
    EXPECT_EQ(cell.use_count(), 1);
    {
        hot::Iface<IAdd> i = hot::to_iface<IAdd>(cell);
        EXPECT_EQ(cell.use_count(), 2);
        hot::Iface<IAdd> copy = i;
        EXPECT_EQ(cell.use_count(), 3);
        hot::Iface<IAdd> moved = std::move(copy);
        EXPECT_EQ(cell.use_count(), 3);
        EXPECT_FALSE(copy);
        const auto back = hot::iface_as<std::shared_ptr<Cell_Shell>, Cell_Shell>(moved, "Cell");
        EXPECT_EQ(cell.use_count(), 4);
        moved.reset();
        EXPECT_EQ(cell.use_count(), 3);
    }
    EXPECT_EQ(cell.use_count(), 1);
    hot::Iface<IAdd> last = hot::to_iface<IAdd>(cell);
    cell.reset();
    EXPECT_FALSE(weak.expired()); // the interface handle keeps the object
    last.reset();
    EXPECT_TRUE(weak.expired());
}

TEST_F(Fixture, DeleterConversionKeepsTheHandleCopy)
{
    ASSERT_EQ(g_copies, 0);
    {
        auto cell = make_cell(&rec);
        std::weak_ptr<Cell_Shell> weak = cell;
        hot::Iface<IAdd> i;
        {
            CountedHandle h(cell);
            i = hot::to_iface<IAdd>(h);
            EXPECT_EQ(g_copies, 2); // "h" and the copy in the deleter
        }
        EXPECT_EQ(g_copies, 1);
        EXPECT_EQ(cell.use_count(), 2); // "cell" and the copy
        cell.reset();
        EXPECT_FALSE(weak.expired());
        EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 1)), 2);
        hot::Iface<IAdd> i2 = i;
        i.reset();
        EXPECT_EQ(g_copies, 1);
        i2.reset();
        EXPECT_EQ(g_copies, 0);
        EXPECT_TRUE(weak.expired());
    }
    EXPECT_EQ(g_copies, 0);
    // An empty handle gives an empty interface handle.
    EXPECT_FALSE(hot::to_iface<IAdd>(CountedHandle()));
    EXPECT_EQ(g_copies, 0);
}

TEST(IfaceField, MayHoldCode)
{
    EXPECT_NE(hot::field_flags<hot::Iface<IAdd>>() & hot::kMayHoldCode, 0);
    EXPECT_NE(hot::field_flags<hot::Iface<IAdd>>() & 0, 1);
    EXPECT_NE(hot::field_copy<hot::Iface<IAdd>>(), nullptr);
}

// ---- the loader builds the tables

namespace
{

struct Fake : hot::ShellBase
{
    virtual ~Fake() { hot::destroy_state(this); }
};

struct State
{
    int value;
    int add_one(int x) { return x + 1; }
    int add_ten(int x) { return x + 10; }
    void touch(int* hits) { ++*hits; }
};
hot::FieldInfo fields[] = {
    {"value", nullptr, hot::type_key<int>(), offsetof(State, value), sizeof(int), alignof(int),
        hot::field_flags<int>(), [](void*, void* f) { new (f) int(1); }, hot::move_field<int>,
        hot::field_destroyer<int>()},
};
hot::StateLayout layout{"State", sizeof(State), alignof(State), 1, fields};

// Generation 1: Add at slot 0. Generation 2: Add at slot 1 and Touch at slot 2.
hot::SlotFn slots1[3] = {slot_of(&State::add_one), slot_of(&State::add_ten), slot_of(&State::touch)};
hot::SlotFn slots2[3] = {slot_of(&State::add_one), slot_of(&State::add_ten), slot_of(&State::touch)};
const hot::IfaceRow rows1[] = {{"IAdd", "Add", kAddSig, 0}, {nullptr, nullptr, nullptr, 0}};
const hot::IfaceRow rows2[] = {{"IAdd", "Add", kAddSig, 1}, {"IAdd", "Touch", kTouchSig, 2}, {nullptr, nullptr, nullptr, 0}};
const char* const names1[] = {"IAdd", nullptr};
const char* const names2[] = {"IAdd", "IOther", nullptr};
constexpr uint64_t kHash = hot::shell_layout_hash("Base", sizeof(Fake), alignof(Fake), 0);

hot::ClassRecord fake1, fake2;

void reset_records()
{
    fake1 = hot::ClassRecord{"IfaceFake", 1, slots1, 3, &layout, nullptr, "Base", kHash};
    fake1.iface_rows = rows1;
    fake1.ifaces = names1;
    fake2 = hot::ClassRecord{"IfaceFake", 2, slots2, 3, &layout, nullptr, "Base", kHash};
    fake2.iface_rows = rows2;
    fake2.ifaces = names2;
}

hot::Instance create_fake1()
{
    auto* s = new Fake;
    hot::init_state(s, &fake1);
    return {s, s};
}
hot::Instance create_fake2()
{
    auto* s = new Fake;
    hot::init_state(s, &fake2);
    return {s, s};
}
void register_1(hot::Registry& reg) { reg.add(&fake1, &create_fake1); }
void register_2(hot::Registry& reg) { reg.add(&fake2, &create_fake2); }

hot::Module module_of(hot::Module::RegisterFn fn, const char* name)
{
    hot::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

struct LoaderIface : ::testing::Test
{
    LoaderIface() { reset_records(); }
    hot::Loader loader;
};

} // namespace

TEST_F(LoaderIface, LoadBuildsTheTableAndTheNames)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_1, "G1")));
    const hot::ClassRecord* r = loader.find("IfaceFake")->record;
    const uint32_t add = hot::iface_method_id("IAdd.Add", kAddSig);
    ASSERT_NE(r->iface_slots, nullptr);
    ASSERT_GT(r->iface_slot_count, add);
    EXPECT_EQ(r->iface_slots[add], 0u);
    ASSERT_NE(r->ifaces, nullptr);
    EXPECT_STREQ(r->ifaces[0], "IAdd");
    EXPECT_EQ(r->ifaces[1], nullptr);

    auto obj = std::shared_ptr<Fake>(static_cast<Fake*>(loader.find("IfaceFake")->create().shell));
    const hot::Iface<IAdd> i = hot::to_iface<IAdd>(obj);
    std::atomic<uint32_t> site{0};
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 5);
    EXPECT_TRUE(hot::is_iface(obj, "IAdd"));
    EXPECT_FALSE(hot::is_iface(obj, "IOther"));
}

TEST_F(LoaderIface, ASecondAdoptRebuildsTheTable)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_1, "G1")));
    auto obj = std::shared_ptr<Fake>(static_cast<Fake*>(loader.find("IfaceFake")->create().shell));
    const hot::Iface<IAdd> i = hot::to_iface<IAdd>(obj);
    std::atomic<uint32_t> site{0};
    std::atomic<uint32_t> touch_site{0};
    int hits = 0;
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 5);
    EXPECT_THROW(hot::call_iface<void(int*)>(i, touch_site, "IAdd.Touch", &hits), hot::ScriptError);

    ASSERT_RELOAD_OK(loader.request_reload(module_of(register_2, "G2")));
    ASSERT_RELOAD_OK(loader.commit_pending());
    // The same site, the same object: the slot of Add changed, and Touch exists now.
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 14);
    hot::call_iface<void(int*)>(i, touch_site, "IAdd.Touch", &hits);
    EXPECT_EQ(hits, 1);

    const hot::ClassRecord* r = loader.find("IfaceFake")->record;
    ASSERT_NE(r->ifaces, nullptr);
    EXPECT_STREQ(r->ifaces[1], "IOther");
    // The canonical record keeps a copy of the names, not the list of the module.
    EXPECT_NE(r->ifaces, static_cast<const char* const*>(names2));
    EXPECT_NE(r->ifaces, static_cast<const char* const*>(names1));
    EXPECT_EQ(r->iface_rows, nullptr);
    EXPECT_TRUE(hot::is_iface(obj, "IOther"));
}

TEST_F(LoaderIface, ARemovedClassKeepsTheSessionTables)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_2, "G2")));
    auto obj = std::shared_ptr<Fake>(static_cast<Fake*>(loader.find("IfaceFake")->create().shell));
    const hot::Iface<IAdd> i = hot::to_iface<IAdd>(obj);
    auto empty = [](hot::Registry&) {};
    ASSERT_RELOAD_OK(loader.request_reload(module_of(empty, "G3")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(loader.commit_pending());
    testing::internal::GetCapturedStderr();
    const hot::ClassRecord* r = loader.find("IfaceFake")->record;
    EXPECT_EQ(r->iface_rows, nullptr);
    ASSERT_NE(r->ifaces, nullptr);
    EXPECT_STREQ(r->ifaces[0], "IAdd");
    EXPECT_NE(r->ifaces, static_cast<const char* const*>(names2)); // or the pointer is the first record's own list
    ASSERT_NE(r->iface_slots, nullptr);
    EXPECT_TRUE(hot::is_iface(obj, "IAdd"));
    // The slot points to the handler of the removed class: the call is ignored and counted.
    std::atomic<uint32_t> site{0};
    int hits = 0;
    const uint64_t before = hot::removed_class_call_count();
    testing::internal::CaptureStderr();
    hot::call_iface<void(int*)>(i, site, "IAdd.Touch", &hits);
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hits, 0);
    EXPECT_EQ(hot::removed_class_call_count(), before + 1);
}

namespace
{
// A backend that loads nothing: revert_unit needs one for the signature lines of the AOT code.
struct NullBackend : hot::Backend
{
    hot::LoadResult load(const hot::ScriptSource&, uint32_t, hot::LoadMode) override { return {}; }
    const char* name() const override { return "null"; }
};
const hot::AotScript kAot[] = {{"X", &register_1}};
const hot::AotTable kAotTable = {kAot, 1};
} // namespace

// An override gives other rows; a revert gives the AOT slots back.
TEST_F(LoaderIface, ARevertToAotRebuildsTheAotTable)
{
    ASSERT_RELOAD_OK(loader.load_aot(kAotTable));
    auto obj = std::shared_ptr<Fake>(static_cast<Fake*>(loader.find("IfaceFake")->create().shell));
    const hot::Iface<IAdd> i = hot::to_iface<IAdd>(obj);
    std::atomic<uint32_t> site{0};
    std::atomic<uint32_t> touch_site{0};
    int hits = 0;
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 5);

    hot::Module m = module_of(register_2, "O2");
    m.unit = "X";
    ASSERT_RELOAD_OK(loader.request_reload(std::move(m)));
    ASSERT_RELOAD_OK(loader.commit_pending());
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 14);
    hot::call_iface<void(int*)>(i, touch_site, "IAdd.Touch", &hits);
    EXPECT_EQ(hits, 1);

    NullBackend backend;
    ASSERT_RELOAD_OK(loader.revert_unit("X", backend));
    ASSERT_RELOAD_OK(loader.commit_pending());
    EXPECT_EQ((hot::call_iface<int(int)>(i, site, "IAdd.Add", 4)), 5); // slot 0 again
    EXPECT_THROW(hot::call_iface<void(int*)>(i, touch_site, "IAdd.Touch", &hits), hot::ScriptError);
    const hot::ClassRecord* r = loader.find("IfaceFake")->record;
    ASSERT_NE(r->ifaces, nullptr);
    EXPECT_STREQ(r->ifaces[0], "IAdd");
    EXPECT_EQ(r->ifaces[1], nullptr); // the AOT list has one interface
}

TEST_F(LoaderIface, ResetClearsTheTables)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register_2, "G2")));
    std::string error;
    ASSERT_TRUE(loader.reset(error)) << error;
    reset_records();
    ASSERT_RELOAD_OK(loader.load(module_of(register_1, "G1")));
    const hot::ClassRecord* r = loader.find("IfaceFake")->record;
    EXPECT_EQ(r->iface_slots[hot::iface_method_id("IAdd.Add", kAddSig)], 0u);
}
