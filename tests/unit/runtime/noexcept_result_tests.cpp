// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::noexcept_result: the result of a noexcept shell override after an unhandled script error.
#include "hot/runtime/shell.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace hot
{
// The host gives a safe result for a result type with no default value.
template <> struct noexcept_result<const std::string&>
{
    static const std::string& get()
    {
        static const std::string empty;
        return empty;
    }
};
} // namespace hot

namespace
{

static_assert(!hot::detail::has_noexcept_result<int&>::value, "no specialization for int&");
static_assert(hot::detail::has_noexcept_result<const std::string&>::value, "specialization is found");

struct NoexceptResultTest : testing::Test
{
    hot::ShellCore shell;
    std::vector<std::string> reports;
    void SetUp() override
    {
        hot::set_script_error_handler([this](const hot::ScriptErrorReport& r) { reports.push_back(r.message); });
    }
    void TearDown() override { hot::set_script_error_handler(nullptr); }
};

// A reference result returns the value of the specialization, and the handler sees one report.
TEST_F(NoexceptResultTest, ReferenceResultUsesSpecialization)
{
    const std::string& r = hot::call_direct<const std::string&, true>(
        &shell, [&]() -> const std::string& { throw hot::ScriptError("script error"); });
    EXPECT_EQ(&r, &hot::noexcept_result<const std::string&>::get());
    EXPECT_TRUE(r.empty());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0], "script error");
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// A result with a default value does not use the specialization.
TEST_F(NoexceptResultTest, DefaultValueResultStaysDefault)
{
    EXPECT_EQ((hot::call_direct<int, true>(&shell, []() -> int { throw hot::ScriptError("script error"); })), 0);
    EXPECT_EQ(reports.size(), 1u);
}

} // namespace
