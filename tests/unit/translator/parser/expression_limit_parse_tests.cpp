// Tests for the limits of the expression parser: truncated input, deep nesting and long input.
#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

using namespace sppc;
using namespace expr_test;

TEST(ExpressionParseLimits, Truncation)
{
    // Each prefix of a long expression ends: no crash and no hang.
    const std::string full =
        "x = this.Move(Vec3(2.0 * this.speed * dt, 0, 0)) + Math.Max(a ? b : c, x[i + 1].y->z) "
        "<< 2 > Vec3(1, 2, 3) && static_cast<int>(y) < n || (float x, int* q) => { f(); } "
        "|| std::vector<int>::size_type(s) + 'c' + \"str\" + 0x1e+5 + .5e-3f ;";
    size_t ok = 0;
    for (size_t n = 0; n <= full.size(); ++n) {
        const std::string text = full.substr(0, n);
        const LineMap map(text);
        Diagnostics diag;
        ast::ExprP x = parse_expression({text, &map}, {0, static_cast<uint32_t>(n)}, diag);
        if (x) ++ok;
        if (!x) EXPECT_GE(diag.items.size(), 1u) << text;
    }
    EXPECT_GT(ok, 0u);
}

TEST(ExpressionParseLimits, DeepNesting)
{
    auto fails_with = [](const std::string& text, const std::string& msg) {
        Parsed p(text);
        EXPECT_TRUE(p.expr == nullptr);
        ASSERT_GE(p.diag.items.size(), 1u);
        // A generic error can have the text ", found ..." after the message.
        EXPECT_EQ(p.diag.items[0].msg.rfind(msg, 0), 0u) << p.diag.items[0].msg;
    };
    const size_t n = 100000;
    fails_with(std::string(n, '('), "maximum recursion depth exceeded");
    fails_with(std::string(n, '(') + "a" + std::string(n, ')'),
               "maximum recursion depth exceeded");
    fails_with(std::string(n, '['), "expected an expression");
    {
        std::string t;
        for (size_t i = 0; i < n; ++i) t += "a[";
        fails_with(t, "maximum recursion depth exceeded");
    }
    {
        std::string t = "f(";
        for (size_t i = 0; i < n; ++i) t += "g(";
        fails_with(t, "maximum recursion depth exceeded");
    }
    fails_with(std::string(n, '-') + "a", "maximum operator nesting level exceeded");
    fails_with(std::string(n, '!') + "a", "maximum operator nesting level exceeded");
    {
        std::string t;
        for (size_t i = 0; i < n; ++i) t += "a = ";
        fails_with(t + "a", "maximum operator nesting level exceeded");
    }
    {
        std::string t;
        for (size_t i = 0; i < n; ++i) t += "a ? ";
        fails_with(t, "maximum recursion depth exceeded");
    }
    // A moderate depth works.
    std::string ok = std::string(100, '(') + "a" + std::string(100, ')');
    EXPECT_EQ(round_trip(ok), ok);
}

TEST(ExpressionParseLimits, LongInput)
{
    // A long chain of a left associative operator, within the limit of the operators.
    std::string t = "a";
    for (int i = 0; i < 200; ++i) t += " + b";
    Parsed p(t);
    EXPECT_TRUE(p.expr != nullptr);
    // A long argument list is not a nesting.
    std::string args = "f(";
    for (int i = 0; i < 5000; ++i) args += (i ? ", x" : "x");
    args += ")";
    Parsed q(args);
    EXPECT_TRUE(q.expr != nullptr) << q.diag.format("t");
    EXPECT_EQ(q.diag.items.size(), 0u);
}
