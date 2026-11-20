// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the runtime helpers with an optional of a host (fake::optional) and with std::optional.
#include "hot/runtime.h"

#include "engine/FakeOptional.h"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <optional>
#include <variant>

namespace
{

struct Box
{
    int v = 7;
};

template <class T> class OptionalLike : public ::testing::Test
{
};
using Optionals = ::testing::Types<std::optional<Box>, fake::optional<Box>>;
TYPED_TEST_SUITE(OptionalLike, Optionals);

TEST(OptionalTrait, Detection)
{
    static_assert(hot::is_optional_like_v<std::optional<int>>);
    static_assert(hot::is_optional_like_v<const fake::optional<int>&>);
    static_assert(!hot::is_optional_like_v<int*>);
    static_assert(!hot::is_optional_like_v<std::shared_ptr<int>>);
    static_assert(!hot::is_optional_like_v<std::variant<int, float>>);
    static_assert(!hot::is_optional_like_v<int>);
}

TYPED_TEST(OptionalLike, DerefGivesValueOrThrows)
{
    TypeParam o = Box{};
    EXPECT_EQ(hot::deref(o).v, 7);
    hot::deref(o).v = 9;
    EXPECT_EQ((*o).v, 9);
    EXPECT_EQ(hot::deref(TypeParam(Box{})).v, 7);
    TypeParam e;
    EXPECT_THROW(hot::deref(e), hot::NullReference);
    EXPECT_THROW(hot::deref(TypeParam()), hot::NullReference);
}

TYPED_TEST(OptionalLike, NonnullGivesValueOrThrows)
{
    TypeParam o = Box{};
    EXPECT_EQ(hot::nonnull(o).v, 7);
    EXPECT_EQ(hot::nonnull(o, "msg").v, 7);
    TypeParam e;
    EXPECT_THROW(hot::nonnull(e), hot::NullReference);
    EXPECT_THROW(hot::nonnull(e, "msg"), hot::NullReference);
    EXPECT_THROW(hot::nonnull(TypeParam(), "msg"), hot::NullReference);
}

TYPED_TEST(OptionalLike, CompareWithNullptr)
{
    TypeParam o = Box{};
    TypeParam e;
    EXPECT_TRUE(e == nullptr);
    EXPECT_TRUE(nullptr == e);
    EXPECT_FALSE(o == nullptr);
    EXPECT_TRUE(o != nullptr);
    EXPECT_TRUE(nullptr != o);
    EXPECT_FALSE(e != nullptr);
}

TYPED_TEST(OptionalLike, CoalesceGivesValueOrDefault)
{
    TypeParam o = Box{};
    TypeParam e;
    EXPECT_EQ(hot::coalesce(o, [] { return Box{1}; }).v, 7);
    EXPECT_EQ(hot::coalesce(e, [] { return Box{1}; }).v, 1);
}

TEST(OptionalLikeSafeAccess, DefaultIsStdOptional)
{
    Box b;
    Box* p = &b;
    auto r = hot::safe_access(p, [](auto&& s) { return hot::deref(s).v; });
    static_assert(std::is_same_v<decltype(r), std::optional<int>>);
    EXPECT_EQ(*r, 7);
    Box* n = nullptr;
    EXPECT_FALSE(hot::safe_access(n, [](auto&& s) { return hot::deref(s).v; }));
}

TEST(OptionalLikeSafeAccess, FakeOptionalResultAndReceiver)
{
    Box b;
    Box* p = &b;
    auto r = hot::safe_access<fake::optional>(p, [](auto&& s) { return hot::deref(s).v; });
    static_assert(std::is_same_v<decltype(r), fake::optional<int>>);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7);
    Box* n = nullptr;
    EXPECT_FALSE(hot::safe_access<fake::optional>(n, [](auto&& s) { return hot::deref(s).v; }).has_value());

    fake::optional<Box> o = Box{};
    auto r2 = hot::safe_access<fake::optional>(o, [](auto&& s) { return hot::deref(s).v; });
    EXPECT_EQ(*r2, 7);
    fake::optional<Box> e = fake::nullopt;
    EXPECT_FALSE(hot::safe_access<fake::optional>(e, [](auto&& s) { return hot::deref(s).v; }).has_value());

    // A chain: the inner result is a fake::optional, so it keeps its type.
    auto chain = hot::safe_access<fake::optional>(
        hot::safe_access<fake::optional>(p, [](auto&& s) { return fake::optional<Box>(Box{hot::deref(s).v}); }),
        [](auto&& s) { return hot::deref(s).v; });
    EXPECT_EQ(*chain, 7);
}

// A value type that converts implicitly from "const char*" (like StringHash and ea::string). Its
// comparison with a pointer compiles, so the comparison with nullptr must not be ambiguous.
struct Name
{
    Name() = default;
    Name(const char* s) : s_(s) {}
    bool operator==(const Name& o) const { return s_ == o.s_; }
    const char* s_ = nullptr;
};

template <class T> class OptionalNullptr : public ::testing::Test
{
};
using NameOptionals = ::testing::Types<std::optional<Name>, fake::optional<Name>, std::optional<std::string_view>>;
TYPED_TEST_SUITE(OptionalNullptr, NameOptionals);

TYPED_TEST(OptionalNullptr, AllFourFormsWithAPointerConvertibleValue)
{
    TypeParam e;
    TypeParam o = typename TypeParam::value_type("a");
    EXPECT_TRUE(e == nullptr);
    EXPECT_TRUE(nullptr == e);
    EXPECT_FALSE(e != nullptr);
    EXPECT_FALSE(nullptr != e);
    EXPECT_FALSE(o == nullptr);
    EXPECT_FALSE(nullptr == o);
    EXPECT_TRUE(o != nullptr);
    EXPECT_TRUE(nullptr != o);
}

// A move-only value: a const rvalue gives a reference, not a copy.
struct MoveOnly
{
    MoveOnly() = default;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    int v = 5;
};

TYPED_TEST(OptionalLike, ConstRvalueGivesAReference)
{
    using M = std::conditional_t<std::is_same_v<TypeParam, std::optional<Box>>, std::optional<MoveOnly>,
        fake::optional<MoveOnly>>;
    const M o = M(MoveOnly{});
    static_assert(std::is_same_v<decltype(hot::deref(std::move(o))), const MoveOnly&>);
    static_assert(std::is_same_v<decltype(hot::nonnull(std::move(o))), const MoveOnly&>);
    static_assert(std::is_same_v<decltype(hot::nonnull(std::move(o), "m")), const MoveOnly&>);
    EXPECT_EQ(hot::deref(std::move(o)).v, 5);
}

} // namespace
