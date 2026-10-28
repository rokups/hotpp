// Tests for the #line directives that the printer writes.
#include <gtest/gtest.h>

#include <string>

#include "emit.h"
#include "support/emit_test_support.h"

using namespace spc::ast;
using namespace emit_test;

namespace {

// The script text has four lines. The first byte of line 2 is 2, of line 3 is 4
// and of line 4 is 6. A pos of 0 means: no position.
const char* kText = "a\nb\nc\nd\n";

}  // namespace

TEST(LineDirectiveEmit, LineDirectiveOnLineChange)
{
    spc::LineMap map(kText);
    // line 2: std::printf("x", dt,
    // line 3:     self->p, hp);
    ExprP e = call(name({"std", "printf"}, 2),
                   list(str("\"x\""), id("dt"), arrow(id("self", 4), "p"), id("hp")));
    StmtP s = es(std::move(e));
    EXPECT_EQ(print_lines(*s, map, "t.script", 1),
              "#line 2 \"t.script\"\n"
              "    std::printf(\"x\", dt,\n"
              "#line 3 \"t.script\"\n"
              "    self->p, hp);\n");
}

TEST(LineDirectiveEmit, FinallyLineDirectives)
{
    spc::LineMap map(kText);
    // line 2: try { f;
    // line 3: } finally {
    // line 4: g; }
    // The finally body prints first. The try body gets its "#line" again.
    TryStmt t;
    t.pos = 2;
    t.block = BlockStmt{slist(es(id("f", 2))), 2, 4};
    t.finally_block = BlockStmt{slist(es(id("g", 6))), 4, 6};
    StmtP s = make_stmt(std::move(t));
    EXPECT_EQ(print_lines(*s, map, "t.script", 1),
              "#line 2 \"t.script\"\n"
              "    { script::Finally __fin_0([&]\n"
              "#line 3 \"t.script\"\n"
              "        {\n"
              "#line 4 \"t.script\"\n"
              "            g; });\n"
              "#line 2 \"t.script\"\n"
              "        { f;\n"
              "#line 3 \"t.script\"\n"
              "        } }\n");
}

TEST(LineDirectiveEmit, OneLineDirectivePerLine)
{
    spc::LineMap map(kText);
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
    EXPECT_EQ(print_lines(*s, map, "t.script", 1),
              "#line 2 \"t.script\"\n"
              "    if (c)\n"
              "#line 3 \"t.script\"\n"
              "    { x = 1;\n"
              "#line 4 \"t.script\"\n"
              "    }\n");
}

TEST(LineDirectiveEmit, BodyKeepsLineAndDepth)
{
    spc::LineMap map(kText);
    // The old emitter case: statements of a function body at depth 1 and a lambda block.
    LambdaExpr l;
    l.pos = 2;
    l.body.pos = 2;
    l.body.end_pos = 6;
    l.body.stmts = slist(es(bin("=", id("a", 4), num("1", 4))));
    spc::ast::Emitter e(map, "t.script");
    emit_stmts(slist(es(call(id("f", 2), list(make_expr(std::move(l)))))), e, 1);
    e.finish();
    EXPECT_EQ(e.text(),
              "#line 2 \"t.script\"\n"
              "    f([=]() {\n"
              "#line 3 \"t.script\"\n"
              "        a = 1;\n"
              "#line 4 \"t.script\"\n"
              "    });\n");
}
