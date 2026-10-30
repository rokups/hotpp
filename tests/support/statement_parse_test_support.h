// Helpers for the tests of the statement parser. The tests parse a block and print it.
#pragma once

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace stmt_test
{

using namespace hotc;

// A parse of the text as the body of a block. The text is the part between the braces.
struct Parsed
{
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::BlockStmt block;

    explicit Parsed(std::string t)
        : text(std::move(t))
        , map(text)
    {
        block = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
    }

    // The printed text of the statements, separated by one space.
    std::string printed() const
    {
        std::string r;
        for (const ast::StmtP& s : block.stmts)
            r += (r.empty() ? "" : " ") + ast::print(*s);
        return r;
    }
};

inline std::string round_trip(const std::string& text)
{
    Parsed p(text);
    EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
    return p.printed();
}

// The errors as "line:col: text", one in each item.
inline std::vector<std::string> errors_of(const Parsed& p)
{
    std::vector<std::string> r;
    for (const Diag& d : p.diag.items)
        r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
    return r;
}

} // namespace stmt_test
