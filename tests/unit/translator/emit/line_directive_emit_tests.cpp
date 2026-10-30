// Tests for the #line directives that the printer writes.
#include "emit.h"
#include "support/emit_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc::ast;
using namespace emit_test;

namespace
{

// The script text has four lines. The first byte of line 2 is 2, of line 3 is 4
// and of line 4 is 6. A pos of 0 means: no position.
const char* kText = "a\nb\nc\nd\n";

} // namespace

TEST(LineDirectiveEmit, LineDirectiveOnLineChange)
{
    hotc::LineMap map(kText);
    // line 2: std::printf("x", dt,
    // line 3:     self->p, hp);
    ExprP e = call(name({"std", "printf"}, 2), list(str("\"x\""), id("dt"), arrow(id("self", 4), "p"), id("hp")));
    StmtP s = es(std::move(e));
    EXPECT_EQ(print_lines(*s, map, "t.hot", 1),
        "#line 2 \"t.hot\"\n"
        "    std::printf(\"x\", dt,\n"
        "#line 3 \"t.hot\"\n"
        "    self->p, hp);\n");
}

TEST(LineDirectiveEmit, FinallyLineDirectives)
{
    hotc::LineMap map(kText);
    // line 2: try { f;
    // line 3: } finally {
    // line 4: g; }
    // The finally body prints first. The try body gets its "#line" again.
    TryStmt t;
    t.pos = 2;
    t.block = BlockStmt{slist(es(id("f", 2))), 2, 4};
    t.finally_block = BlockStmt{slist(es(id("g", 6))), 4, 6};
    StmtP s = make_stmt(std::move(t));
    EXPECT_EQ(print_lines(*s, map, "t.hot", 1),
        "#line 2 \"t.hot\"\n"
        "    { hot::Finally __fin_0([&]\n"
        "#line 3 \"t.hot\"\n"
        "        {\n"
        "#line 4 \"t.hot\"\n"
        "            g; });\n"
        "#line 2 \"t.hot\"\n"
        "        { f;\n"
        "#line 3 \"t.hot\"\n"
        "        } }\n");
}

TEST(LineDirectiveEmit, OneLineDirectivePerLine)
{
    hotc::LineMap map(kText);
    // line 2: if (c)
    // line 3: { x = 1;
    // line 4: }
    IfStmt i;
    i.pos = 2;
    i.cond = id("c", 2);
    BlockStmt b;
    b.pos = 4;
    b.end_pos = 6;
    b.stmts = slist(es(bin("=", id("x", 4), num("1", 4))));
    i.then = make_stmt(std::move(b));
    StmtP s = make_stmt(std::move(i));
    EXPECT_EQ(print_lines(*s, map, "t.hot", 1),
        "#line 2 \"t.hot\"\n"
        "    if (c)\n"
        "#line 3 \"t.hot\"\n"
        "    { x = 1;\n"
        "#line 4 \"t.hot\"\n"
        "    }\n");
}

TEST(LineDirectiveEmit, BodyKeepsLineAndDepth)
{
    hotc::LineMap map(kText);
    // The old emitter case: statements of a function body at depth 1 and a lambda block.
    LambdaExpr l;
    l.pos = 2;
    l.body.pos = 2;
    l.body.end_pos = 6;
    l.body.stmts = slist(es(bin("=", id("a", 4), num("1", 4))));
    hotc::ast::Emitter e(map, "t.hot");
    emit_stmts(slist(es(call(id("f", 2), list(make_expr(std::move(l)))))), e, 1);
    e.finish();
    EXPECT_EQ(e.text(),
        "#line 2 \"t.hot\"\n"
        "    f([=]() {\n"
        "#line 3 \"t.hot\"\n"
        "        a = 1;\n"
        "#line 4 \"t.hot\"\n"
        "    });\n");
}
