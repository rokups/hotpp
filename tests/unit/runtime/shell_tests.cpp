// Tests of the shell helpers: a checked access to a shell and a handle from "this".
#include "hot/runtime/shell.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

namespace
{

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
struct Plain_Shell final
    : hot::PlainObject
    , hot::ShellBase
{
};

TEST(ShellOf, NullHandleThrows)
{
    std::shared_ptr<Base> h;
    EXPECT_THROW(hot::shell_of<Cell_Shell>(h, "Cell"), std::runtime_error);
    Cell_Shell* p = nullptr;
    EXPECT_THROW(hot::shell_of<Cell_Shell>(p, "Cell"), std::runtime_error);
}

TEST(HandleFromThis, GivesTheOwningHandle)
{
    auto cell = std::make_shared<Cell_Shell>();
    const auto h = hot::handle_from_this<std::shared_ptr<Base>>(cell.get());
    EXPECT_EQ(h.get(), static_cast<Base*>(cell.get()));
    EXPECT_EQ(cell.use_count(), 2);
}

TEST(HandleFromThis, PlainObjectUsesTheBaseOfTheShell)
{
    auto plain = std::make_shared<Plain_Shell>();
    const auto h = hot::handle_from_this<std::shared_ptr<Plain_Shell>>(plain.get());
    EXPECT_EQ(h, plain);
}

TEST(HandleFromThis, ObjectWithoutAnOwnerThrows)
{
    Cell_Shell cell;
    EXPECT_THROW(hot::handle_from_this<std::shared_ptr<Cell_Shell>>(&cell), std::runtime_error);
}

// A child record names its parent in "super" (design 4.5). "is" and "as" walk the chain (design 7.7).
struct Parent_Shell
    : Base
    , hot::ShellBase
{
};
struct Child_Shell final : Parent_Shell
{
};

TEST(IsAs, ScriptClassesWalkTheParentRecords)
{
    hot::ClassRecord parent{};
    parent.name = "Parent";
    hot::ClassRecord child{};
    child.name = "Child";
    child.super = &parent;
    auto obj = std::make_shared<Child_Shell>();
    obj->cls = &child;
    const std::shared_ptr<Base> h = obj;
    EXPECT_TRUE(hot::record_is(&child, "Parent"));
    EXPECT_FALSE(hot::record_is(&parent, "Child"));
    EXPECT_TRUE(hot::is_script(h, "Child"));
    EXPECT_TRUE(hot::is_script(h, "Parent"));
    EXPECT_FALSE(hot::is_script(h, "Other"));
    EXPECT_EQ(hot::shell_of<Parent_Shell>(h, "Parent"), obj.get());
    const std::shared_ptr<Child_Shell> c = hot::as_script<Child_Shell>(h, "Child");
    EXPECT_EQ(c.get(), obj.get());
    EXPECT_EQ(hot::as_script<Child_Shell>(h, "Other"), nullptr);
    std::shared_ptr<Base> none;
    EXPECT_FALSE(hot::is_script(none, "Child"));
    EXPECT_EQ(hot::as_script<Child_Shell>(none, "Child"), nullptr);
}

TEST(IsAs, NativeTypesUseRtti)
{
    auto cell = std::make_shared<Cell_Shell>();
    const std::shared_ptr<Base> h = cell;
    EXPECT_TRUE(hot::is_native<Cell_Shell>(h));
    EXPECT_FALSE(hot::is_native<Plain_Shell>(cell.get()));
    EXPECT_EQ(hot::as_native<Cell_Shell>(h), cell);
    Base* raw = cell.get();
    EXPECT_EQ(hot::as_native<Cell_Shell>(raw), cell.get());
}

// ---- signature keys of the slots (design 5.3, S17)
// A script state: the first field is the owner pointer. The slots are members of it.
struct TestState
{
    hot::ShellBase* owner = nullptr;
    int add_one(int x) { return x + 1; }
    void touch(int* hits) { ++*hits; }
};

template <class Pmf> hot::SlotFn slot_of(Pmf p)
{
    return reinterpret_cast<hot::SlotFn>(p);
}

// A slot without a key calls the function. A matching key also calls it.
TEST(SlotKeys, MatchingOrMissingKeyCalls)
{
    const hot::SlotFn slots[1] = {slot_of(&TestState::add_one)};
    hot::ClassRecord rec{"Keyed", 1, slots, 1, nullptr, nullptr, "Base", 0};
    TestState state;
    hot::ShellBase shell;
    shell.cls = &rec;
    shell.state = &state;
    state.owner = &shell;
    EXPECT_EQ(hot::call<int(Base*, int)>(&shell, 0, 4), 5);
    const char* keys[1] = {typeid(int(Base*, int)).name()};
    rec.slot_keys = keys;
    EXPECT_EQ(hot::call<int(Base*, int)>(&shell, 0, 4), 5);
}

// A call with another signature than the key detaches: the function does not run, the result
// is the default value and the runtime counts the call.
TEST(SlotKeys, OtherSignatureDetaches)
{
    const hot::SlotFn slots[2] = {slot_of(&TestState::add_one), slot_of(&TestState::touch)};
    const char* keys[2] = {typeid(int(Base*, int, int)).name(), typeid(void(Base*, int*, int)).name()};
    hot::ClassRecord rec{"Detached", 1, slots, 2, nullptr, nullptr, "Base", 0};
    rec.slot_keys = keys;
    TestState state;
    hot::ShellBase shell;
    shell.cls = &rec;
    shell.state = &state;
    state.owner = &shell;
    int hits = 0;
    const uint64_t before = hot::detached_call_count();
    testing::internal::CaptureStderr();
    EXPECT_EQ(hot::call<int(Base*, int)>(&shell, 0, 4), 0);
    EXPECT_EQ(hot::call<int(Base*, int)>(&shell, 0, 4), 0);
    hot::call<void(Base*, int*)>(&shell, 1, &hits);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(hits, 0);
    EXPECT_EQ(hot::detached_call_count(), before + 3);
    // One line for each slot: the second call of slot 0 logs nothing.
    size_t lines = 0;
    for (size_t p = log.find("old signature"); p != std::string::npos; p = log.find("old signature", p + 1))
        ++lines;
    EXPECT_EQ(lines, 2u) << log;
}

} // namespace
