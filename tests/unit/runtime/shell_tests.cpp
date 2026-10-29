// Tests of the shell helpers: a checked access to a shell and a handle from "this".
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include "spp/runtime/shell.h"

namespace {

struct Base {
    virtual ~Base() = default;
};
struct Cell_Shell final : Base, spp::ShellBase, std::enable_shared_from_this<Cell_Shell> {};
struct Plain_Shell final : spp::PlainObject, spp::ShellBase {};

TEST(ShellOf, NullHandleThrows)
{
    std::shared_ptr<Base> h;
    EXPECT_THROW(spp::shell_of<Cell_Shell>(h, "Cell"), std::runtime_error);
    Cell_Shell* p = nullptr;
    EXPECT_THROW(spp::shell_of<Cell_Shell>(p, "Cell"), std::runtime_error);
}

TEST(HandleFromThis, GivesTheOwningHandle)
{
    auto cell = std::make_shared<Cell_Shell>();
    const auto h = spp::handle_from_this<std::shared_ptr<Base>>(cell.get());
    EXPECT_EQ(h.get(), static_cast<Base*>(cell.get()));
    EXPECT_EQ(cell.use_count(), 2);
}

TEST(HandleFromThis, PlainObjectUsesTheBaseOfTheShell)
{
    auto plain = std::make_shared<Plain_Shell>();
    const auto h = spp::handle_from_this<std::shared_ptr<Plain_Shell>>(plain.get());
    EXPECT_EQ(h, plain);
}

TEST(HandleFromThis, ObjectWithoutAnOwnerThrows)
{
    Cell_Shell cell;
    EXPECT_THROW(spp::handle_from_this<std::shared_ptr<Cell_Shell>>(&cell), std::runtime_error);
}

// A child record names its parent in "super" (design 4.5). "is" and "as" walk the chain (design 7.7).
struct Parent_Shell : Base, spp::ShellBase {};
struct Child_Shell final : Parent_Shell {};

TEST(IsAs, ScriptClassesWalkTheParentRecords)
{
    spp::ClassRecord parent{};
    parent.name = "Parent";
    spp::ClassRecord child{};
    child.name = "Child";
    child.super = &parent;
    auto obj = std::make_shared<Child_Shell>();
    obj->cls = &child;
    const std::shared_ptr<Base> h = obj;
    EXPECT_TRUE(spp::record_is(&child, "Parent"));
    EXPECT_FALSE(spp::record_is(&parent, "Child"));
    EXPECT_TRUE(spp::is_script(h, "Child"));
    EXPECT_TRUE(spp::is_script(h, "Parent"));
    EXPECT_FALSE(spp::is_script(h, "Other"));
    EXPECT_EQ(spp::shell_of<Parent_Shell>(h, "Parent"), obj.get());
    const std::shared_ptr<Child_Shell> c = spp::as_script<Child_Shell>(h, "Child");
    EXPECT_EQ(c.get(), obj.get());
    EXPECT_EQ(spp::as_script<Child_Shell>(h, "Other"), nullptr);
    std::shared_ptr<Base> none;
    EXPECT_FALSE(spp::is_script(none, "Child"));
    EXPECT_EQ(spp::as_script<Child_Shell>(none, "Child"), nullptr);
}

TEST(IsAs, NativeTypesUseRtti)
{
    auto cell = std::make_shared<Cell_Shell>();
    const std::shared_ptr<Base> h = cell;
    EXPECT_TRUE(spp::is_native<Cell_Shell>(h));
    EXPECT_FALSE(spp::is_native<Plain_Shell>(cell.get()));
    EXPECT_EQ(spp::as_native<Cell_Shell>(h), cell);
    Base* raw = cell.get();
    EXPECT_EQ(spp::as_native<Cell_Shell>(raw), cell.get());
}

// ---- signature keys of the slots (design 5.3, S17)
// A script state: the first field is the owner pointer. The slots are members of it.
struct TestState {
    spp::ShellBase* owner = nullptr;
    int add_one(int x) { return x + 1; }
    void touch(int* hits) { ++*hits; }
};

template <class Pmf>
spp::SlotFn slot_of(Pmf p)
{
    return reinterpret_cast<spp::SlotFn>(p);
}

// A slot without a key calls the function. A matching key also calls it.
TEST(SlotKeys, MatchingOrMissingKeyCalls)
{
    const spp::SlotFn slots[1] = {slot_of(&TestState::add_one)};
    spp::ClassRecord rec{"Keyed", 1, slots, 1, nullptr, nullptr, "Base", 0};
    TestState state;
    spp::ShellBase shell;
    shell.cls = &rec;
    shell.state = &state;
    state.owner = &shell;
    EXPECT_EQ(spp::call<int(Base*, int)>(&shell, 0, 4), 5);
    const char* keys[1] = {typeid(int(Base*, int)).name()};
    rec.slot_keys = keys;
    EXPECT_EQ(spp::call<int(Base*, int)>(&shell, 0, 4), 5);
}

// A call with another signature than the key detaches: the function does not run, the result
// is the default value and the runtime counts the call.
TEST(SlotKeys, OtherSignatureDetaches)
{
    const spp::SlotFn slots[2] = {slot_of(&TestState::add_one), slot_of(&TestState::touch)};
    const char* keys[2] = {typeid(int(Base*, int, int)).name(), typeid(void(Base*, int*, int)).name()};
    spp::ClassRecord rec{"Detached", 1, slots, 2, nullptr, nullptr, "Base", 0};
    rec.slot_keys = keys;
    TestState state;
    spp::ShellBase shell;
    shell.cls = &rec;
    shell.state = &state;
    state.owner = &shell;
    int hits = 0;
    const uint64_t before = spp::detached_call_count();
    testing::internal::CaptureStderr();
    EXPECT_EQ(spp::call<int(Base*, int)>(&shell, 0, 4), 0);
    EXPECT_EQ(spp::call<int(Base*, int)>(&shell, 0, 4), 0);
    spp::call<void(Base*, int*)>(&shell, 1, &hits);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(hits, 0);
    EXPECT_EQ(spp::detached_call_count(), before + 3);
    // One line for each slot: the second call of slot 0 logs nothing.
    size_t lines = 0;
    for (size_t p = log.find("old signature"); p != std::string::npos; p = log.find("old signature", p + 1)) ++lines;
    EXPECT_EQ(lines, 2u) << log;
}

}  // namespace
