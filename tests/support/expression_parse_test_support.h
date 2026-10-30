// Helpers for the tests of the expression parser. The tests parse a text and print the tree.
#pragma once

#include "emit.h"
#include "parser/expression.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

namespace expr_test
{

using namespace sppc;

// A parse of the whole text. The map and the text live as long as the object.
struct Parsed
{
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::ExprP expr;

    explicit Parsed(std::string t)
        : text(std::move(t))
        , map(text)
    {
        expr = parse_expression({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
    }
};

inline std::string round_trip(const std::string& text)
{
    Parsed p(text);
    EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
    EXPECT_TRUE(p.expr != nullptr) << text;
    return p.expr ? ast::print(*p.expr) : "<null>";
}

// The text of the one error: "line:col: text".
inline std::string one_error(const std::string& text)
{
    Parsed p(text);
    EXPECT_TRUE(p.expr == nullptr) << text;
    EXPECT_EQ(p.diag.items.size(), 1u) << text << "\n" << p.diag.format("t");
    if (p.diag.items.empty())
        return "<none>";
    const Diag& d = p.diag.items[0];
    return std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg;
}

} // namespace expr_test
