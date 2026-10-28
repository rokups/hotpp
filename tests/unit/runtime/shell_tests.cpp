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

}  // namespace
