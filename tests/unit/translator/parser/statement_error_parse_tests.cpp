// Tests for the errors and the positions of the statement parser.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

using namespace sppc;
using namespace stmt_test;

TEST(StatementParseError, Errors)
{
    {
        // Two broken statements give two errors; the good statements stay.
        Parsed p("a = 1;\nb = ;\nc = 2;\nvar = 3;\nd = 4;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:5: expected an expression",
                                                          "4:5: expected a name"}))
            << p.diag.format("t");
        EXPECT_EQ(p.printed(), "a = 1; c = 2; d = 4;");
    }
    {
        // A missing ";" at the end is one error.
        Parsed p("a = 1;\nb = 2");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:6: expected ';'"}));
        EXPECT_EQ(p.printed(), "a = 1; b = 2;");
    }
    {
        Parsed p("if (a) { b = 1 }\nc = 2;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:16: expected ';'"}));
        EXPECT_EQ(p.printed(), "if (a) { b = 1; } c = 2;");
    }
    {
        Parsed p("if a) b;\nc;");
        ASSERT_GE(p.diag.items.size(), 1u);
        EXPECT_EQ(errors_of(p)[0], "1:4: expected '('");
        EXPECT_EQ(p.printed(), "c;");
    }
    {
        Parsed p("if (a b;\nc;");
        ASSERT_GE(p.diag.items.size(), 1u);
        EXPECT_EQ(p.printed(), "c;");
    }
    {
        Parsed p("do a; (b);\nc;");
        EXPECT_EQ(errors_of(p)[0], "1:7: expected 'while'");
        EXPECT_EQ(p.printed(), "c;");
    }
    {
        Parsed p("if (a) { b;\n");
        ASSERT_GE(p.diag.items.size(), 1u);
        EXPECT_EQ(errors_of(p).back(), "2:1: expected '}'");
    }
    {
        // A stray "}" is one error and the next statement parses.
        Parsed p("a; }\nb;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:4: expected a statement"}));
        EXPECT_EQ(p.printed(), "a; b;");
    }
    {
        Parsed p("a = (1;");
        EXPECT_EQ(errors_of(p).back().substr(errors_of(p).back().find(' ') + 1),
                  "unexpected end of file");
    }
    {
        Parsed p("switch (x) { a(); case 1: b(); }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:14: expected 'case' or 'default'"}));
    }
}

TEST(StatementParseError, Positions)
{
    Parsed p("a = 1;\n  if (b) { c; } else d;\nvar y = 2;\ndo e; while (f);");
    ASSERT_EQ(p.block.stmts.size(), 4u) << p.diag.format("t");
    EXPECT_EQ(ast::pos_of(*p.block.stmts[0]), 0u);
    const auto* i = std::get_if<ast::IfStmt>(&p.block.stmts[1]->v);
    ASSERT_TRUE(i);
    EXPECT_EQ(i->pos, p.text.find("if"));
    EXPECT_EQ(i->else_pos, p.text.find("else"));
    const auto* b = std::get_if<ast::BlockStmt>(&i->then->v);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->pos, p.text.find("{ c"));
    EXPECT_EQ(b->end_pos, p.text.find("} else"));
    EXPECT_EQ(ast::pos_of(*b->stmts[0]), p.text.find("c;"));
    EXPECT_EQ(ast::pos_of(*p.block.stmts[2]), p.text.find("var y"));
    const auto* d = std::get_if<ast::DoWhileStmt>(&p.block.stmts[3]->v);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->pos, p.text.find("do"));
    EXPECT_EQ(d->while_pos, p.text.find("while"));
    // The block gets the position of the "{" before the span and of the "}" after it.
    EXPECT_EQ(p.block.pos, 0u);
    EXPECT_EQ(p.block.end_pos, p.text.size());
}

TEST(StatementParseError, OffsetsInBuffer)
{
    // A block in the middle of a larger buffer. Positions and errors are absolute.
    const std::string text = "class A : B {\n  void f() {\n    x = 1;\n    y = ;\n  }\n}\n";
    const LineMap map(text);
    const uint32_t b = static_cast<uint32_t>(text.find("{\n    x")) + 1;
    const uint32_t e = static_cast<uint32_t>(text.find("  }\n}"));
    Diagnostics diag;
    ast::BlockStmt blk = parse_block({text, &map}, {b, e}, diag);
    EXPECT_EQ(blk.pos, b - 1);
    EXPECT_EQ(blk.end_pos, e);
    ASSERT_EQ(blk.stmts.size(), 1u);
    EXPECT_EQ(ast::pos_of(*blk.stmts[0]), text.find("x = 1"));
    ASSERT_EQ(diag.items.size(), 1u);
    EXPECT_EQ(diag.items[0].line, 4);
    EXPECT_EQ(diag.items[0].col, 9);
    EXPECT_EQ(diag.items[0].msg, "expected an expression");
    // The parser reads only the span.
    Diagnostics d2;
    ast::BlockStmt part = parse_block({text, &map}, {b, b + 15}, d2);  // up to "x = 1;"
    EXPECT_EQ(d2.items.size(), 0u) << d2.format("t");
    EXPECT_EQ(part.stmts.size(), 1u);
}
