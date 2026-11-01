// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the runtime checks: hot::deref, hot::star and hot::index, and the exceptions.
#include "hot/runtime.h"
#include "engine/CheckedTypes.h"

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The specializations that the translator writes for the stub attribute checked_index and for the
// custom array template.
namespace hot
{
template <class... A> struct checked_index< ::hostns2::Ring<A...>> : std::true_type
{
};
template <class T, auto N> struct checked_index< ::hostns2::Fixed<T, N>> : std::true_type
{
};
} // namespace hot

namespace
{

#if HOT_RUNTIME_CHECKS

TEST(RuntimeChecks, DerefOfNullPointerThrows)
{
    int* p = nullptr;
    EXPECT_THROW(hot::deref(p), hot::NullReference);
    EXPECT_THROW(hot::deref(p), hot::ScriptError);
    EXPECT_THROW(hot::deref(p), std::exception);
}

TEST(RuntimeChecks, DerefOfNullHandleThrows)
{
    std::shared_ptr<int> h;
    EXPECT_THROW(hot::deref(h), hot::NullReference);
}

TEST(RuntimeChecks, StarOfNullThrows)
{
    int* p = nullptr;
    std::shared_ptr<int> h;
    EXPECT_THROW(hot::star(p), hot::NullReference);
    EXPECT_THROW(hot::star(h), hot::NullReference);
}

TEST(RuntimeChecks, IndexOfArrayOutOfRangeThrows)
{
    std::array<int, 3> a{1, 2, 3};
    try
    {
        hot::index(a, 3);
        FAIL() << "no exception";
    }
    catch (const hot::IndexOutOfRange& e)
    {
        EXPECT_EQ(e.index(), 3u);
        EXPECT_EQ(e.size(), 3u);
    }
    int c[2] = {1, 2};
    EXPECT_THROW(hot::index(c, 2), hot::IndexOutOfRange);
    const std::array<int, 3>& ca = a;
    EXPECT_THROW(hot::index(ca, 5), hot::IndexOutOfRange);
    EXPECT_THROW(hot::index(std::array<int, 3>{}, 3), hot::IndexOutOfRange);
}

TEST(RuntimeChecks, VectorIndexOutOfRangeThrows)
{
    std::vector<int> v{1, 2, 3};
    try
    {
        hot::index(v, 3);
        FAIL() << "no exception";
    }
    catch (const hot::IndexOutOfRange& e)
    {
        EXPECT_EQ(e.index(), 3u);
        EXPECT_EQ(e.size(), 3u);
    }
    EXPECT_THROW(hot::index(v, -1), hot::IndexOutOfRange);
    const std::vector<int>& cv = v;
    EXPECT_THROW(hot::index(cv, 7), hot::IndexOutOfRange);
    EXPECT_THROW(hot::index(std::vector<int>{}, 0), hot::IndexOutOfRange);
    EXPECT_THROW(hot::index(std::vector<int>(), 0), hot::IndexOutOfRange);
}

TEST(RuntimeChecks, CustomContainerWithTheTraitIsChecked)
{
    hostns2::Ring<int> ring(3);
    EXPECT_THROW(hot::index(ring, 3), hot::IndexOutOfRange);
    const hostns2::Ring<int>& cr = ring;
    EXPECT_THROW(hot::index(cr, 10), hot::IndexOutOfRange);
    hostns2::Fixed<int, 2> fixed;
    EXPECT_THROW(hot::index(fixed, 2), hot::IndexOutOfRange);
    EXPECT_THROW(hot::index(fixed, -1), hot::IndexOutOfRange);
}

TEST(RuntimeChecks, ContainerWithoutTheTraitIsNotChecked)
{
    // hostns2::Plain has no specialization: hot::index calls its operator[] and adds no test.
    static_assert(!hot::checked_index<hostns2::Plain>::value);
    static_assert(hot::checked_index<std::vector<int>>::value);
    static_assert(hot::checked_index<std::array<int, 2>>::value);
    static_assert(hot::checked_index<hostns2::Ring<int>>::value);
    static_assert(!hot::checked_index<std::map<int, int>>::value);
    hostns2::Plain plain;
    EXPECT_NO_THROW(hot::index(plain, 3));
    std::map<int, int> m;
    EXPECT_NO_THROW(hot::index(m, 99));
    EXPECT_EQ(m.size(), 1u);
}

TEST(RuntimeChecks, NegativeIndexThrows)
{
    std::array<int, 3> a{1, 2, 3};
    EXPECT_THROW(hot::index(a, -1), hot::IndexOutOfRange);
}

TEST(RuntimeChecks, ReportUnhandledCallsTheHandler)
{
    std::string type, message, cls;
    hot::set_script_error_handler([&](const hot::ScriptErrorReport& r) {
        type = r.type;
        message = r.message;
        cls = r.class_name;
    });
    int* p = nullptr;
    try
    {
        hot::deref(p);
    }
    catch (...)
    {
        hot::detail::report_unhandled(nullptr, std::current_exception());
    }
    hot::set_script_error_handler({});
    EXPECT_EQ(type, "hot::NullReference");
    EXPECT_EQ(message, "null pointer dereference");
    EXPECT_EQ(cls, "?");
}

#endif

TEST(RuntimeChecks, ValidAccessWorks)
{
    int v = 5;
    int* p = &v;
    auto h = std::make_shared<int>(6);
    EXPECT_EQ(hot::deref(p), 5);
    EXPECT_EQ(hot::star(p), 5);
    EXPECT_EQ(hot::star(h), 6);
    std::array<int, 3> a{1, 2, 3};
    EXPECT_EQ(hot::index(a, 2), 3);
    hot::index(a, 0) = 9;
    EXPECT_EQ(a[0], 9);
    int c[2] = {4, 5};
    EXPECT_EQ(hot::index(c, 1), 5);
    EXPECT_EQ(hot::index(std::array<int, 2>{7, 8}, 1), 8);
}

TEST(RuntimeChecks, IndexOfVectorInRangeWorks)
{
    EXPECT_EQ(hot::index(std::vector<int>{1}, 0), 1);
    std::vector<int> v{1, 2};
    hot::index(v, 1) = 5;
    EXPECT_EQ(v[1], 5);
    hostns2::Ring<int> ring(2);
    hot::index(ring, 1) = 4;
    EXPECT_EQ(hot::index(ring, 1), 4);
    // An rvalue checked container returns the element by value (moved).
    static_assert(!std::is_reference_v<decltype(hot::index(std::vector<std::string>{"a"}, 0))>);
    EXPECT_EQ(hot::index(std::vector<std::string>{"abc"}, 0), "abc");
    // vector<bool> returns a proxy for an lvalue. An rvalue returns a bool: a proxy would dangle.
    std::vector<bool> bits{true, false};
    EXPECT_TRUE(hot::index(bits, 0));
    hot::index(bits, 1) = true;
    EXPECT_TRUE(bits[1]);
    static_assert(std::is_same_v<decltype(hot::index(std::vector<bool>{true}, 0)), bool>);
    EXPECT_TRUE(hot::index(std::vector<bool>{false, true}, 1));
    EXPECT_FALSE(hot::index(std::vector<bool>{false, true}, 0));
}

TEST(RuntimeChecks, IndexOfMapUsesItsOperator)
{
    std::map<int, int> m;
    hot::index(m, 3) = 7;
    EXPECT_EQ(m.at(3), 7);
}

} // namespace
