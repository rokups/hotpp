// Tests for the runtime checks: hot::deref, hot::star and hot::index, and the exceptions.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

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

TEST(RuntimeChecks, IndexOfOtherContainerUsesItsOperator)
{
    EXPECT_EQ(hot::index(std::vector<int>{1}, 0), 1);
    std::vector<int> v{1, 2};
    hot::index(v, 1) = 5;
    EXPECT_EQ(v[1], 5);
}

} // namespace
