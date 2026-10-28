// Tests for the printer of the expression and statement AST (src/translator/emit.h).
// The tests build the nodes by hand and compare the C++ text.
#include <gtest/gtest.h>

#include <string>

#include "emit.h"

namespace {

using namespace spc::ast;

ExprP name(std::initializer_list<std::string> parts, uint32_t pos = 0)
{
    NameExpr n;
    n.parts = parts;
    n.pos = pos;
    return make_expr(std::move(n));
}
ExprP id(const std::string& s, uint32_t pos = 0) { return name({s}, pos); }
ExprP num(const std::string& s, uint32_t pos = 0) { return make_expr(NumberExpr{s, pos}); }
ExprP str(const std::string& s) { return make_expr(StringExpr{s}); }
// The lowering pass writes "st->x" as raw text. This helper makes the same node.
ExprP arrow(ExprP obj, const std::string& m)
{
    const uint32_t pos = pos_of(*obj);
    return make_expr(RawExpr{print(*obj) + "->" + m, pos});
}
ExprP dot(ExprP obj, const std::string& m) { return make_expr(MemberExpr{std::move(obj), m}); }
ExprP bin(const std::string& op, ExprP l, ExprP r)
{
    return make_expr(BinaryExpr{op, std::move(l), std::move(r)});
}
ExprP un(const std::string& op, ExprP x, bool postfix = false)
{
    return make_expr(UnaryExpr{op, std::move(x), postfix});
}
ExprP call(ExprP callee, std::vector<ExprP> args)
{
    return make_expr(CallExpr{std::move(callee), std::move(args)});
}
template <class... A>
std::vector<ExprP> list(A... a)
{
    std::vector<ExprP> v;
    (v.push_back(std::move(a)), ...);
    return v;
}
template <class... A>
std::vector<StmtP> slist(A... a)
{
    std::vector<StmtP> v;
    (v.push_back(std::move(a)), ...);
    return v;
}
StmtP es(ExprP x) { return make_stmt(ExprStmt{std::move(x)}); }
StmtP blk(std::vector<StmtP> v)
{
    BlockStmt b;
    b.stmts = std::move(v);
    return make_stmt(std::move(b));
}

TEST(Emit, Assignment)
{
    ExprP e = bin("=", arrow(id("st"), "fired"), bin("+", arrow(id("st"), "fired"), num("1")));
    EXPECT_EQ(print(*es(std::move(e))), "st->fired = st->fired + 1;");
}

TEST(Emit, CallTwoArgs)
{
    EXPECT_EQ(print(*call(name({"Math", "Max"}), list(id("a"), num("0.0f")))), "Math::Max(a, 0.0f)");
}

TEST(Emit, Ternary)
{
    EXPECT_EQ(print(*make_expr(TernaryExpr{id("a"), id("b"), id("c")})), "a ? b : c");
}

TEST(Emit, NestedCalls)
{
    ExprP inner = call(arrow(id("self"), "Name"), {});
    ExprP e = call(name({"std", "printf"}), list(str("\"%s\""), call(dot(std::move(inner), "c_str"), {})));
    EXPECT_EQ(print(*e), "std::printf(\"%s\", self->Name()." "c_str())");
}

TEST(Emit, NameParts)
{
    NameExpr n;
    n.parts = {"std", "vector"};
    n.template_args = "<int>";
    EXPECT_EQ(print(*make_expr(std::move(n))), "std::vector<int>");
}

TEST(Emit, Unary)
{
    EXPECT_EQ(print(*un("!", id("x"))), "!x");
    EXPECT_EQ(print(*un("-", id("x"))), "-x");
    EXPECT_EQ(print(*un("++", id("i"), true)), "i++");
    EXPECT_EQ(print(*un("-", un("-", id("x")))), "- -x");
    EXPECT_EQ(print(*un("&", id("source"))), "&source");
}

TEST(Emit, IndexParenNull)
{
    ExprP e = make_expr(IndexExpr{id("a"), make_expr(ParenExpr{bin("+", id("i"), num("1"))})});
    EXPECT_EQ(print(*e), "a[(i + 1)]");
    EXPECT_EQ(print(*make_expr(NullExpr{})), "nullptr");
}

TEST(Emit, BracedList)
{
    BracedListExpr b;
    b.elems = list(bin("*", num("2.0f"), id("x")), num("0"), num("0"));
    EXPECT_EQ(print(*make_expr(std::move(b))), "{2.0f * x, 0, 0}");
    TypedBracedExpr t;
    t.type = "Vec3";
    t.elems = list(num("1"), num("2"));
    EXPECT_EQ(print(*make_expr(std::move(t))), "Vec3{1, 2}");
}

TEST(Emit, Lambda)
{
    LambdaExpr l;
    l.params = {{"x", "int"}, {"y", ""}};
    l.body.stmts = slist(es(bin("=", id("a"), id("x"))));
    EXPECT_EQ(print(*make_expr(std::move(l))), "[=](int x, auto y) { a = x; }");
    EXPECT_EQ(print(*make_expr(LambdaExpr{})), "[=]() {}");
}

TEST(Emit, VarDecl)
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

TEST(Emit, IfElse)
{
    IfStmt i;
    i.cond = bin(">", id("a"), num("0"));
    i.then = blk(slist(es(bin("=", id("x"), num("1")))));
    i.otherwise = blk(slist(es(bin("=", id("x"), num("2")))));
    EXPECT_EQ(print(*make_stmt(std::move(i))), "if (a > 0) { x = 1; } else { x = 2; }");
}

TEST(Emit, TryCatchThrow)
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
    EXPECT_EQ(print(*make_stmt(std::move(t))),
              "try { throw 1; } catch (int e) { f; } catch (...) { throw; }");
}

TEST(Emit, ElseIfAndWhile)
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

TEST(Emit, ForClassic)
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

TEST(Emit, ForOf)
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

TEST(Emit, Switch)
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
    EXPECT_EQ(print(*make_stmt(std::move(s))),
              "switch (k) { case 1: case 2: x = 1; break; default: return 0; }");
}

TEST(Emit, Raw)
{
    EXPECT_EQ(print(*make_stmt(RawStmt{"int q = 0;"})), "int q = 0;");
    EXPECT_EQ(print(*make_expr(RawExpr{"state_of(self)"})), "state_of(self)");
}

// The script text has four lines. The first byte of line 2 is 2, of line 3 is 4
// and of line 4 is 6. A pos of 0 means: no position.
const char* kText = "a\nb\nc\nd\n";

TEST(Emit, LineDirectiveOnLineChange)
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

TEST(Emit, OneLineDirectivePerLine)
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

TEST(Emit, BodyKeepsLineAndDepth)
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

}  // namespace
