// Tests for the parser of the statements (src/translator/parser/statement.h).
// Most tests are round trips: parse the block, print each statement and compare the text.
// The printer writes a fixed layout, so the expected text can differ in spacing.
#include <gtest/gtest.h>
#include <pthread.h>

#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"

namespace {

using namespace spc;

// A parse of the text as the body of a block. The text is the part between the braces.
struct Parsed {
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::BlockStmt block;

    explicit Parsed(std::string t) : text(std::move(t)), map(text)
    {
        block = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
    }

    // The printed text of the statements, separated by one space.
    std::string printed() const
    {
        std::string r;
        for (const ast::StmtP& s : block.stmts) r += (r.empty() ? "" : " ") + ast::print(*s);
        return r;
    }
};

std::string round_trip(const std::string& text)
{
    Parsed p(text);
    EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
    return p.printed();
}

// The errors as "line:col: text", one in each item.
std::vector<std::string> errors_of(const Parsed& p)
{
    std::vector<std::string> r;
    for (const Diag& d : p.diag.items)
        r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
    return r;
}

TEST(StmtParse, Simple)
{
    EXPECT_EQ(round_trip("a = 1;"), "a = 1;");
    EXPECT_EQ(round_trip("f(x, y);"), "f(x, y);");
    EXPECT_EQ(round_trip(";"), ";");
    EXPECT_EQ(round_trip("break; continue;"), "break; continue;");
    EXPECT_EQ(round_trip("return;"), "return;");
    EXPECT_EQ(round_trip("return a + 1;"), "return a + 1;");
    EXPECT_EQ(round_trip(""), "");
    EXPECT_EQ(round_trip("  // c\n /* d */ "), "");
    EXPECT_EQ(round_trip("a = \"x;}\"; b = ';';"), "a = \"x;}\"; b = ';';");
}

TEST(StmtParse, Declarations)
{
    EXPECT_EQ(round_trip("let x = 1;"), "auto x = 1;");
    EXPECT_EQ(round_trip("let x;"), "auto x;");
    EXPECT_EQ(round_trip("const x = f(1);"), "const auto x = f(1);");
    EXPECT_EQ(round_trip("let x: float = 2.0;"), "float x = 2.0;");
    EXPECT_EQ(round_trip("let v: std.vector<int> = {1, 2};"), "std::vector<int> v = {1, 2};");
    EXPECT_EQ(round_trip("const p: Actor* = null;"), "const Actor* p = nullptr;");
    EXPECT_EQ(round_trip("let m: std.map<int, std.vector<int>>;"),
              "std::map<int, std::vector<int>> m;");
    EXPECT_EQ(round_trip("let f = (a: int) => { return a; };"),
              "auto f = [=](int a) { return a; };");
}

TEST(StmtParse, Control)
{
    EXPECT_EQ(round_trip("if (a) b = 1;"), "if (a) b = 1;");
    EXPECT_EQ(round_trip("if (a) b = 1; else c = 2;"), "if (a) b = 1; else c = 2;");
    EXPECT_EQ(round_trip("if (a) { b = 1; } else if (c) { d = 2; } else { e = 3; }"),
              "if (a) { b = 1; } else if (c) { d = 2; } else { e = 3; }");
    EXPECT_EQ(round_trip("if (a) if (b) x; else y;"), "if (a) if (b) x; else y;");
    EXPECT_EQ(round_trip("while (a < 3) a++;"), "while (a < 3) a++;");
    EXPECT_EQ(round_trip("do { a++; } while (a < 3);"), "do { a++; } while (a < 3);");
    EXPECT_EQ(round_trip("do a++; while (f(a));"), "do a++; while (f(a));");
    EXPECT_EQ(round_trip("{ a; { b; } }"), "{ a; { b; } }");
    EXPECT_EQ(round_trip("{}"), "{}");
}

TEST(StmtParse, For)
{
    EXPECT_EQ(round_trip("for (let i = 0; i < n; i++) f(i);"), "for (auto i = 0; i < n; i++) f(i);");
    EXPECT_EQ(round_trip("for (let i: int = 0; i < n; i++, j--) { }"),
              "for (int i = 0; i < n; i++, j--) {}");
    EXPECT_EQ(round_trip("for (;;) break;"), "for (;;) break;");
    EXPECT_EQ(round_trip("for (i = 0, j = 1; i < n;) f();"), "for (i = 0, j = 1; i < n;) f();");
    EXPECT_EQ(round_trip("for (let i = f(1, 2); ; ) x;"), "for (auto i = f(1, 2);;) x;");
    // for-of
    EXPECT_EQ(round_trip("for (let x of xs) f(x);"), "for (auto x : xs) f(x);");
    EXPECT_EQ(round_trip("for (const x of this.items) { f(x); }"),
              "for (const auto x : this.items) { f(x); }");
    EXPECT_EQ(round_trip("for (let x: Actor* of actors) f(x);"), "for (Actor* x : actors) f(x);");
    EXPECT_EQ(round_trip("for (let x: std.vector<int> of g()) f(x);"),
              "for (std::vector<int> x : g()) f(x);");
    // A name that has "of" inside is not the keyword.
    EXPECT_EQ(round_trip("for (let x: Dof of xs) f(x);"), "for (Dof x : xs) f(x);");
    EXPECT_EQ(round_trip("for (let of of xs) f(of);"), "for (auto of : xs) f(of);");
    // The old text "of" as a name in a classic for.
    EXPECT_EQ(round_trip("for (let i = of; i < 3; i++) f();"), "for (auto i = of; i < 3; i++) f();");
}

TEST(StmtParse, Switch)
{
    EXPECT_EQ(round_trip("switch (x) { case 1: a(); break; case 2: case 3: b(); default: c(); }"),
              "switch (x) { case 1: a(); break; case 2: case 3: b(); default: c(); }");
    EXPECT_EQ(round_trip("switch (x) { case A.B: f(); break; case 'a': g(); }"),
              "switch (x) { case A::B: f(); break; case 'a': g(); }");
    EXPECT_EQ(round_trip("switch (x) { case (a ? 1 : 2): f(); }"),
              "switch (x) { case (a ? 1 : 2): f(); }");
    EXPECT_EQ(round_trip("switch (x) { case 1: default: f(); case 2: { g(); } }"),
              "switch (x) { case 1: default: f(); case 2: { g(); } }");
    EXPECT_EQ(round_trip("switch (x) { default: case 1: f(); }"),
              "switch (x) { default: case 1: f(); }");
    EXPECT_EQ(round_trip("switch (x) {}"), "switch (x) { }");
    Parsed p("switch (x) { case 1: a(); }");
    ASSERT_EQ(p.block.stmts.size(), 1u);
    const auto* sw = std::get_if<ast::SwitchStmt>(&p.block.stmts[0]->v);
    ASSERT_TRUE(sw);
    ASSERT_EQ(sw->cases.size(), 1u);
    EXPECT_EQ(sw->cases[0].pos, p.text.find("case"));
    EXPECT_EQ(sw->end_pos, p.text.rfind('}'));
    // A bare ternary in a label is not supported: the first ":" ends the label.
    Parsed t("switch (x) { case a ? 1 : 2: f(); }");
    EXPECT_FALSE(t.diag.items.empty());
}

TEST(StmtParse, Keywords)
{
    // A name that starts with a keyword is a name.
    Parsed p("ifx = 1; letter = 2; forx(3); doing = whiley; returned = 1; ofx = 1;");
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.block.stmts.size(), 6u);
    for (const auto& s : p.block.stmts) EXPECT_TRUE(std::holds_alternative<ast::ExprStmt>(s->v));
    EXPECT_EQ(p.printed(), "ifx = 1; letter = 2; forx(3); doing = whiley; returned = 1; ofx = 1;");
}

TEST(StmtParse, Exceptions)
{
    EXPECT_EQ(round_trip("throw 42;"), "throw 42;");
    EXPECT_EQ(round_trip("throw;"), "throw;");
    EXPECT_EQ(round_trip("throw std.runtime_error(\"x\");"), "throw std::runtime_error(\"x\");");
    EXPECT_EQ(round_trip("try { f(); } catch (e: int) { g(e); }"),
              "try { f(); } catch (int e) { g(e); }");
    EXPECT_EQ(round_trip("try { f(); } catch (e: std.exception&) { g(); }\n"
                         "catch (m: const std.string&) { throw; } catch (...) { }"),
              "try { f(); } catch (std::exception& e) { g(); } "
              "catch (const std::string& m) { throw; } catch (...) {}");
    EXPECT_EQ(round_trip("try { try { throw 1; } catch (...) { throw; } } catch (e: int) { }"),
              "try { try { throw 1; } catch (...) { throw; } } catch (int e) {}");
    // The words are keywords only as whole words.
    Parsed p("tryit = 1; thrown = 2; catcher(3);");
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_EQ(p.block.stmts.size(), 3u);
    for (const auto& s : p.block.stmts) EXPECT_TRUE(std::holds_alternative<ast::ExprStmt>(s->v));
    {
        Parsed q("try { f(); } catch (e: int) { }\nthrow;");
        ASSERT_EQ(q.block.stmts.size(), 2u);
        const auto* t = std::get_if<ast::TryStmt>(&q.block.stmts[0]->v);
        ASSERT_NE(t, nullptr);
        ASSERT_EQ(t->catches.size(), 1u);
        EXPECT_EQ(t->catches[0].type, "int");
        EXPECT_EQ(t->catches[0].name, "e");
        EXPECT_FALSE(t->catches[0].is_all);
        EXPECT_TRUE(std::holds_alternative<ast::ThrowStmt>(q.block.stmts[1]->v));
    }
}

TEST(StmtParse, ExceptionErrors)
{
    {
        Parsed p("try { f(); }\nb = 1;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:1: expected 'catch'"}))
            << p.diag.format("t");
    }
    {
        // A type without a name is not supported.
        Parsed p("a = 1;\ntry { f(); } catch (int) { }\nc = 2;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:21: expected a name"}))
            << p.diag.format("t");
        EXPECT_EQ(p.printed(), "a = 1; c = 2;");
    }
    {
        Parsed p("try { } catch (e: std::exception&) { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:22: use '.' instead of '::'"}))
            << p.diag.format("t");
    }
    {
        Parsed p("try { } catch (e: ) { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:19: expected a type"}))
            << p.diag.format("t");
    }
    {
        // The statements after a broken throw stay.
        Parsed p("throw 1 +;\na = 1;");
        EXPECT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.printed(), "a = 1;");
    }
    {
        Parsed p("try { f(); } catch (e: int { }\na = 1;");
        EXPECT_GE(p.diag.items.size(), 1u);
    }
}

TEST(StmtParse, Lambda)
{
    // Statement, expression, block, lambda: the three grammars together.
    const std::string src =
        "let f = (a: int) => { if (a) { g((b: int) => { return b + 1; }); } return a; };";
    EXPECT_EQ(round_trip(src),
              "auto f = [=](int a) { if (a) { g([=](int b) { return b + 1; }); } return a; };");
    EXPECT_EQ(round_trip("run(() => { x = 1; });"), "run([=]() { x = 1; });");
    // An error inside the lambda has an absolute position.
    Parsed p("a;\nb;\nlet f = (a: int) => {\n  x = ;\n};");
    EXPECT_EQ(errors_of(p), (std::vector<std::string>{"4:7: expected an expression"}));
}

TEST(StmtParse, Positions)
{
    Parsed p("a = 1;\n  if (b) { c; } else d;\nlet y = 2;\ndo e; while (f);");
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
    EXPECT_EQ(ast::pos_of(*p.block.stmts[2]), p.text.find("let"));
    const auto* d = std::get_if<ast::DoWhileStmt>(&p.block.stmts[3]->v);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->pos, p.text.find("do"));
    EXPECT_EQ(d->while_pos, p.text.find("while"));
    // The block gets the position of the "{" before the span and of the "}" after it.
    EXPECT_EQ(p.block.pos, 0u);
    EXPECT_EQ(p.block.end_pos, p.text.size());
}

TEST(StmtParse, OffsetsInBuffer)
{
    // A block in the middle of a larger buffer. Positions and errors are absolute.
    const std::string text = "class A : B {\n  function f() {\n    x = 1;\n    y = ;\n  }\n}\n";
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

TEST(StmtParse, Errors)
{
    {
        // Two broken statements give two errors; the good statements stay.
        Parsed p("a = 1;\nb = ;\nc = 2;\nlet = 3;\nd = 4;");
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

// A "." in the text of a type is a scope. The parser writes "::". A "::" is an error.
TEST(StmtParse, TypeScope)
{
    EXPECT_EQ(round_trip("let a: std.string = x; let b: std.vector<std.string>;"),
              "std::string a = x; std::vector<std::string> b;");
    EXPECT_EQ(round_trip("let c: A.B.C*; let d: std.array<int, 3>; let e: float;"),
              "A::B::C* c; std::array<int, 3> d; float e;");
    Parsed p("let a: int;\nlet b: std::string = x;");
    EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:11: use '.' instead of '::'"}));
    Parsed q("for (let s: std::string of xs) f(s);");
    EXPECT_EQ(errors_of(q), (std::vector<std::string>{"1:16: use '.' instead of '::'"}));
}

TEST(StmtParse, Truncation)
{
    // Each prefix of a long block ends: no crash and no hang.
    const std::string full =
        "let a = 1;\nconst b: std.vector<int> = {1, 2};\nif (a < b) { f(a); } else g(b);\n"
        "while (a) a--;\ndo { a++; } while (a < 3);\n"
        "for (let i = 0; i < n; i++) { f(i); }\nfor (let x: Actor* of xs) f(x);\n"
        "for (i = 0, j = 1; i < n; i++, j--) ;\nswitch (a) { case 1: case A.B: f(); break;\n"
        "default: { g(); } }\nlet h = (x: float) => { if (x) { return x; } return 0; };\n"
        "return a + \"s;}\" + 'c' /* c */;\n// end\n"
        "a = 1;\nb = 2;\nc = 3;\nd = 4;\ne = 5;\nf = 6;\ng = 7;\nh = 8;\ni = 9;\nj = 10;\n"
        "k = 11;\nl = 12;\nm = 13;\nn = 14;\no = 15;\np = 16;\nq = 17;\nr = 18;\ns = 19;\nt = 20;\n"
        "u = 21;\nv = 22;\nw = 23;\nx = 24;\ny = 25;\nz = 26;\nbreak;\ncontinue;\n"
        "if (a) {\n  b = 1;\n  while (c) {\n    d();\n  }\n}\nfor (let k of ks) {\n  f(k);\n}\n"
        "switch (a) {\n  case 1:\n    f();\n  default:\n    g();\n}\nlet z: int;\nz = 1;\nz = 2;\nz = 3;\n"
        "try {\n  f();\n} catch (e: const std.exception&) {\n  throw;\n} catch (...) {\n  throw 1;\n}\n";
    size_t lines = 0;
    for (char c : full) lines += c == '\n';
    EXPECT_GE(lines, 60u);
    {
        Parsed whole(full);
        EXPECT_EQ(whole.diag.items.size(), 0u) << whole.diag.format("t");
    }
    for (size_t n = 0; n <= full.size(); ++n) {
        Parsed p(full.substr(0, n));
        // Every cut gives a block, and a cut in a statement gives an error.
        if (p.diag.items.empty()) continue;
        EXPECT_FALSE(p.diag.items[0].msg.empty());
    }
}

// Runs the function in a thread with a small stack. The parse must not use more.
void run_small_stack(std::size_t bytes, const std::function<void()>& fn)
{
    struct Ctx {
        const std::function<void()>* fn;
    } ctx{&fn};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, bytes);
    pthread_t th;
    ASSERT_EQ(pthread_create(
                  &th, &attr,
                  [](void* p) -> void* {
                      (*static_cast<Ctx*>(p)->fn)();
                      return nullptr;
                  },
                  &ctx),
              0);
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
}

TEST(StmtParse, DeepNesting)
{
    auto fails = [](const std::string& text, const char* msg) {
        Parsed p(text);
        ASSERT_GE(p.diag.items.size(), 1u);
        bool found = false;
        for (const Diag& d : p.diag.items) found |= d.msg.rfind(msg, 0) == 0;
        EXPECT_TRUE(found) << p.diag.items[0].msg;
    };
    const std::size_t n = 100000;
    run_small_stack(512 << 10, [&] {
        fails(std::string(n, '{'), "maximum recursion depth exceeded");
        std::string t;
        for (std::size_t i = 0; i < n; ++i) t += "if (a) ";
        fails(t, "maximum recursion depth exceeded");
        t.clear();
        for (std::size_t i = 0; i < n; ++i) t += "try { ";
        fails(t, "maximum recursion depth exceeded");
        t.clear();
        for (std::size_t i = 0; i < n; ++i) t += "{ while (a) ";
        fails(t, "maximum recursion depth exceeded");
        // A lambda in a lambda crosses the expression grammar each time.
        t.clear();
        for (std::size_t i = 0; i < n; ++i) t += "f(() => { ";
        fails(t, "maximum");
        t.clear();
        for (std::size_t i = 0; i < 20; ++i) t += "f(() => { ";
        t += "x;";
        for (std::size_t i = 0; i < 20; ++i) t += " });";
        fails(t, "maximum nesting depth exceeded");
    });
    // A moderate depth works (the limit counts a block and an if as two levels).
    std::string ok, close;
    for (int i = 0; i < 20; ++i) {
        ok += "{ if (a) ";
        close += " }";
    }
    EXPECT_EQ(round_trip(ok + "x;" + close).empty(), false);
    std::string lam, lclose;
    for (int i = 0; i < 8; ++i) {
        lam += "f(() => { ";
        lclose += " });";
    }
    run_small_stack(512 << 10, [&] { EXPECT_EQ(round_trip(lam + "x;" + lclose).empty(), false); });
}

// ---------------------------------------------------------------- the example scripts
std::string read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST(StmtParse, ExampleScripts)
{
    const char* names[] = {"player_v1", "player_v2", "timer_v1", "timer_v2", "native_std_v1",
                           "native_std_v2"};
    size_t bodies = 0;
    for (const char* name : names) {
        const std::string text =
            read_file(std::string(SPP_SOURCE_DIR) + "/example/scripts/" + name + ".script");
        ASSERT_FALSE(text.empty()) << name;
        const LineMap map(text);
        Diagnostics sd;
        const ast::Script script = parse_script(text, map, sd);
        ASSERT_EQ(sd.items.size(), 0u) << name << "\n" << sd.format(name);
        for (const ast::ClassDef& c : script.classes) {
            for (const ast::Method& m : c.methods) {
                Diagnostics diag;
                const ast::BlockStmt b = parse_block({text, &map}, m.body_span, diag);
                EXPECT_EQ(diag.items.size(), 0u)
                    << name << " " << c.name << "::" << m.name << "\n" << diag.format(name);
                EXPECT_FALSE(b.stmts.empty()) << name << " " << m.name;
                ++bodies;
            }
        }
    }
    EXPECT_GE(bodies, 10u);
}

}  // namespace
