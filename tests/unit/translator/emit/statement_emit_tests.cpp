// Tests for the printer of the statements: declarations, if, loops, switch, try and finally.
#include "emit.h"
#include "support/emit_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc::ast;
using namespace emit_test;

TEST(StatementEmit, VarDecl)
{
    VarDeclStmt d;
    d.name = "x";
    d.init = num("1");
    EXPECT_EQ(print(*make_stmt(std::move(d))), "auto x = 1;");
    VarDeclStmt c;
    c.is_const = true;
    c.name = "n";
    c.type = "float";
    c.init = num("2.0f");
    EXPECT_EQ(print(*make_stmt(std::move(c))), "const float n = 2.0f;");
}

TEST(StatementEmit, IfElse)
{
    IfStmt i;
    i.cond = bin(">", id("a"), num("0"));
    i.then = blk(slist(es(bin("=", id("x"), num("1")))));
    i.otherwise = blk(slist(es(bin("=", id("x"), num("2")))));
    EXPECT_EQ(print(*make_stmt(std::move(i))), "if (a > 0) { x = 1; } else { x = 2; }");
}

TEST(StatementEmit, TryCatchThrow)
{
    TryStmt t;
    t.block = BlockStmt{slist(make_stmt(ThrowStmt{num("1"), 0})), 0, 0};
    CatchClause c1;
    c1.type = "int";
    c1.name = "e";
    c1.block = BlockStmt{slist(es(id("f"))), 0, 0};
    CatchClause c2;
    c2.is_all = true;
    c2.block = BlockStmt{slist(make_stmt(ThrowStmt{})), 0, 0};
    t.catches.push_back(std::move(c1));
    t.catches.push_back(std::move(c2));
    EXPECT_EQ(print(*make_stmt(std::move(t))), "try { throw 1; } catch (int e) { f; } catch (...) { throw; }");
}

TEST(StatementEmit, TryFinally)
{
    // With a catch clause: the guard is before the try, in a new block.
    TryStmt t;
    t.block = BlockStmt{slist(es(id("f"))), 0, 0};
    CatchClause c;
    c.is_all = true;
    c.block = BlockStmt{slist(make_stmt(ThrowStmt{})), 0, 0};
    t.catches.push_back(std::move(c));
    t.finally_block = BlockStmt{slist(es(id("g"))), 0, 0};
    EXPECT_EQ(
        print(*make_stmt(std::move(t))), "{ hot::Finally __fin_0([&] { g; }); try { f; } catch (...) { throw; } }");

    // Without a catch clause: the body is a plain block. A nested try gets the next name.
    TryStmt inner;
    inner.block = BlockStmt{slist(es(id("a"))), 0, 0};
    inner.finally_block = BlockStmt{slist(es(id("b"))), 0, 0};
    TryStmt outer;
    outer.block = BlockStmt{slist(make_stmt(std::move(inner))), 0, 0};
    outer.finally_block = BlockStmt{slist(es(id("c"))), 0, 0};
    EXPECT_EQ(print(*make_stmt(std::move(outer))),
        "{ hot::Finally __fin_0([&] { c; }); "
        "{ { hot::Finally __fin_1([&] { b; }); { a; } } } }");
}

TEST(StatementEmit, ElseIfAndWhile)
{
    IfStmt inner;
    inner.cond = id("b");
    inner.then = make_stmt(ReturnStmt{});
    IfStmt i;
    i.cond = id("a");
    i.then = make_stmt(BreakStmt{});
    i.otherwise = make_stmt(std::move(inner));
    EXPECT_EQ(print(*make_stmt(std::move(i))), "if (a) break; else if (b) return;");
    WhileStmt w;
    w.cond = id("c");
    w.body = blk({});
    EXPECT_EQ(print(*make_stmt(std::move(w))), "while (c) {}");
    DoWhileStmt d;
    d.body = blk(slist(make_stmt(ContinueStmt{})));
    d.cond = id("c");
    EXPECT_EQ(print(*make_stmt(std::move(d))), "do { continue; } while (c);");
}

TEST(StatementEmit, ForClassic)
{
    ForStmt f;
    VarDeclStmt d;
    d.name = "i";
    d.type = "int";
    d.init = num("0");
    f.init = slist(make_stmt(std::move(d)));
    f.cond = bin("<", id("i"), id("n"));
    f.step = list(un("++", id("i"), true));
    f.body = blk(slist(es(call(id("f"), list(id("i"))))));
    EXPECT_EQ(print(*make_stmt(std::move(f))), "for (int i = 0; i < n; i++) { f(i); }");
    ForStmt e;
    e.body = make_stmt(BreakStmt{});
    EXPECT_EQ(print(*make_stmt(std::move(e))), "for (;;) break;");
}

TEST(StatementEmit, ForOf)
{
    ForOfStmt f;
    f.is_const = true;
    f.name = "a";
    f.range = id("actors");
    f.body = blk(slist(es(call(arrow(id("a"), "Tick"), list(id("dt"))))));
    EXPECT_EQ(print(*make_stmt(std::move(f))), "for (const auto a : actors) { a->Tick(dt); }");
    ForOfStmt g;
    g.name = "p";
    g.type = "Actor*";
    g.range = id("v");
    g.body = make_stmt(EmptyStmt{});
    EXPECT_EQ(print(*make_stmt(std::move(g))), "for (Actor* p : v) ;");
}

TEST(StatementEmit, Switch)
{
    SwitchStmt s;
    s.subject = id("k");
    SwitchCase a;
    a.labels = list(num("1"), num("2"));
    a.stmts = slist(es(bin("=", id("x"), num("1"))), make_stmt(BreakStmt{}));
    SwitchCase d;
    d.is_default = true;
    d.stmts = slist(make_stmt(ReturnStmt{num("0")}));
    s.cases.push_back(std::move(a));
    s.cases.push_back(std::move(d));
    EXPECT_EQ(print(*make_stmt(std::move(s))), "switch (k) { case 1: case 2: x = 1; break; default: return 0; }");
}
