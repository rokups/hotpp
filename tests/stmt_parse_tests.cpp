// Tests for the parser of the statements (src/translator/parser/statement.h).
// Most tests are round trips: parse the block, print each statement and compare the text.
// The printer writes a fixed layout, so the expected text can differ in spacing.
#include <gtest/gtest.h>
#include <pthread.h>

#include <fstream>
#include <functional>
#include <memory>
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

// No initializer: the emitter value-initializes. "var" and "const" need an initializer.
TEST(StmtParse, DeclarationWithoutInitializer)
{
    EXPECT_EQ(round_trip("int x;"), "int x = int();");
    EXPECT_EQ(round_trip("Vec3 v;"), "Vec3 v = Vec3();");
    EXPECT_EQ(round_trip("Actor* p;"), "Actor* p = std::type_identity_t<Actor*>();");
    EXPECT_EQ(round_trip("for (int i; i < 3; i++) f(i);"),
              "for (int i = int(); i < 3; i++) f(i);");
    EXPECT_EQ(errors_of(Parsed("var x;")),
              (std::vector<std::string>{"1:1: var needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("const int X;")),
              (std::vector<std::string>{"1:1: a const needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("const var X;")),
              (std::vector<std::string>{"1:1: var needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("for (var i; i < 3; i++) f(i);")),
              (std::vector<std::string>{"1:6: var needs an initializer"}));
}

TEST(StmtParse, RefLocalAndRefArguments)
{
    EXPECT_EQ(round_trip("var r = ref this.pos;"), "auto& r = this.pos;");
    EXPECT_EQ(round_trip("Vec3 r = ref p;"), "Vec3& r = p;");
    EXPECT_EQ(round_trip("const var r = ref p;"), "const auto& r = p;");
    EXPECT_EQ(round_trip("Fill(out v); Swap(ref a, ref b);"),
              "Fill(script::ref_arg(v)); Swap(script::ref_arg(a), script::ref_arg(b));");
    EXPECT_EQ(round_trip("this.target = ref source;"), "this.target = script::ref_arg(source);");
}

TEST(StmtParse, PrefixAmpersandIsRemoved)
{
    Parsed p("var r = &x;");
    EXPECT_EQ(errors_of(p), std::vector<std::string>{"1:9: use 'ref' instead of '&'"});
    Parsed q("f(&x);");
    EXPECT_EQ(errors_of(q), std::vector<std::string>{"1:3: use 'ref' instead of '&'"});
}

TEST(StmtParse, Declarations)
{
    EXPECT_EQ(round_trip("var x = 1;"), "auto x = 1;");
    EXPECT_EQ(round_trip("const var x = f(1);"), "const auto x = f(1);");
    EXPECT_EQ(round_trip("float x = 2.0;"), "float x = 2.0;");
    EXPECT_EQ(round_trip("std.vector<int> v = f(1, 2);"), "std::vector<int> v = f(1, 2);");
    EXPECT_EQ(round_trip("const Actor* p = null;"), "const Actor* p = nullptr;");
    EXPECT_EQ(round_trip("std.map<int, std.vector<int>> m;"),
              "std::map<int, std::vector<int>> m = std::map<int, std::vector<int>>();");
    EXPECT_EQ(round_trip("var f = (int a) => { return a; };"),
              "auto f = [=](int a) { return a; };");
}

// Type-first declarations, "var" and "const". The look-ahead rule is in statement_grammar.cpp.
TEST(StmtParse, TypeFirstDeclarations)
{
    EXPECT_EQ(round_trip("int x = 5;"), "int x = 5;");
    EXPECT_EQ(round_trip("var x = 5;"), "auto x = 5;");
    EXPECT_EQ(round_trip("const int X = 5;"), "const int X = 5;");
    EXPECT_EQ(round_trip("const var y = 1;"), "const auto y = 1;");
    EXPECT_EQ(round_trip("Actor* p = null;"), "Actor* p = nullptr;");
    EXPECT_EQ(round_trip("std.string s;"), "std::string s = std::string();");
    EXPECT_EQ(round_trip("std.string s = \"x\";"), "std::string s = \"x\";");
    EXPECT_EQ(round_trip("unsigned int n = 0;"), "unsigned int n = 0;");
    EXPECT_EQ(round_trip("const std.exception& e = f();"), "const std::exception& e = f();");
    EXPECT_EQ(round_trip("int[3] a;"), "int[3] a = std::type_identity_t<int[3]>();");
    EXPECT_EQ(round_trip("/* c */ int // d\n x = 1;"), "int x = 1;");

    // The kind of each statement: a declaration or an expression.
    const auto decl = [](const std::string& text) -> const ast::VarDeclStmt* {
        static std::vector<std::unique_ptr<Parsed>> keep;
        keep.push_back(std::make_unique<Parsed>(text));
        const Parsed& p = *keep.back();
        EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
        if (p.block.stmts.size() != 1) return nullptr;
        return std::get_if<ast::VarDeclStmt>(&p.block.stmts[0]->v);
    };
    // The C++ rule: "a * b" is a declaration of b.
    const ast::VarDeclStmt* d = decl("a * b = c;");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->name, "b");
    EXPECT_EQ(d->type, "a*");
    d = decl("a * b;");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->name, "b");
    EXPECT_EQ(d->type, "a*");
    // A "<" after a name opens a template argument list.
    d = decl("a < b > c;");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->name, "c");
    EXPECT_EQ(d->type, "a<b>");
    d = decl("std.map<int, std.vector<int>> m = f();");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->type, "std::map<int, std::vector<int>>");
    EXPECT_FALSE(decl("f(x);"));
    EXPECT_FALSE(decl("a = b;"));
    EXPECT_FALSE(decl("a.b = c;"));
    EXPECT_FALSE(decl("x++;"));
    EXPECT_FALSE(decl("*p = 1;"));
    EXPECT_FALSE(decl("a == b;"));
    EXPECT_FALSE(decl("a < b;"));
    EXPECT_FALSE(decl("a + b;"));
    EXPECT_FALSE(decl("x;"));
    EXPECT_FALSE(decl("f<int>(x);"));
    EXPECT_FALSE(decl("a[i] = c;"));
    EXPECT_FALSE(decl("this.n = 1;"));
    EXPECT_FALSE(decl("a *= b;"));

    // "let" is removed. The old "let x: T = e;" gives the same error.
    const std::string let_error = "1:1: 'let' is removed; write 'var x = ...' or 'Type x = ...'";
    EXPECT_EQ(errors_of(Parsed("let x = 1;")), (std::vector<std::string>{let_error}));
    EXPECT_EQ(errors_of(Parsed("let x: int = 1;")), (std::vector<std::string>{let_error}));
    // In a for head, the first error is the "let" error.
    for (const char* text : {"for (let i = 0; i < 3; i++) f(i);"}) {
        const std::vector<std::string> e = errors_of(Parsed(text));
        ASSERT_FALSE(e.empty()) << text;
        EXPECT_EQ(e[0], "1:6: 'let' is removed; write 'var x = ...' or 'Type x = ...'") << text;
    }
    {
        const std::vector<std::string> e = errors_of(Parsed("foreach (let x in xs) f(x);"));
        ASSERT_FALSE(e.empty());
        EXPECT_EQ(e[0], "1:10: 'let' is removed; write 'var x = ...' or 'Type x = ...'");
    }
    // A "const" needs "var" or a type.
    EXPECT_EQ(errors_of(Parsed("const x = 1;")),
              (std::vector<std::string>{"1:7: expected 'var' or a type after 'const'"}));

    // A declaration in the init of a for statement.
    EXPECT_EQ(round_trip("for (std.size_t i = 0; i < n; i++) f(i);"),
              "for (std::size_t i = 0; i < n; i++) f(i);");
    EXPECT_EQ(round_trip("for (Actor* p = first; p; p = next(p)) f(p);"),
              "for (Actor* p = first; p; p = next(p)) f(p);");
    EXPECT_EQ(round_trip("for (const int k = 3; i < k; i++) f(i);"),
              "for (const int k = 3; i < k; i++) f(i);");
    EXPECT_EQ(round_trip("for (i = 0; i < n; i++) f(i);"), "for (i = 0; i < n; i++) f(i);");
    // foreach with the new declaration forms.
    EXPECT_EQ(round_trip("foreach (const Actor* a in xs) f(a);"), "for (const Actor* a : xs) f(a);");
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
    EXPECT_EQ(round_trip("for (var i = 0; i < n; i++) f(i);"), "for (auto i = 0; i < n; i++) f(i);");
    EXPECT_EQ(round_trip("for (int i = 0; i < n; i++, j--) { }"),
              "for (int i = 0; i < n; i++, j--) {}");
    EXPECT_EQ(round_trip("for (;;) break;"), "for (;;) break;");
    EXPECT_EQ(round_trip("for (i = 0, j = 1; i < n;) f();"), "for (i = 0, j = 1; i < n;) f();");
    EXPECT_EQ(round_trip("for (var i = f(1, 2); ; ) x;"), "for (auto i = f(1, 2);;) x;");
    // foreach
    EXPECT_EQ(round_trip("foreach (var x in xs) f(x);"), "for (auto x : xs) f(x);");
    EXPECT_EQ(round_trip("foreach (const var x in this.items) { f(x); }"),
              "for (const auto x : this.items) { f(x); }");
    EXPECT_EQ(round_trip("foreach (Actor* x in actors) f(x);"), "for (Actor* x : actors) f(x);");
    EXPECT_EQ(round_trip("foreach (std.vector<int> x in g()) f(x);"),
              "for (std::vector<int> x : g()) f(x);");
    // A name that has "in" inside is not the keyword.
    EXPECT_EQ(round_trip("foreach (Din x in xs) f(x);"), "for (Din x : xs) f(x);");
    EXPECT_EQ(round_trip("foreach (const Actor* a in xs) f(a);"), "for (const Actor* a : xs) f(a);");
    EXPECT_EQ(round_trip("foreach (var of in xs) f(of);"), "for (auto of : xs) f(of);");
    EXPECT_EQ(round_trip("foreach (var x in xs) foreach (int y in x) f(y);"),
              "for (auto x : xs) for (int y : x) f(y);");
    // The old text "of" as a name in a classic for.
    EXPECT_EQ(round_trip("for (var i = of; i < 3; i++) f();"), "for (auto i = of; i < 3; i++) f();");
}

TEST(StmtParse, ForOfRemoved)
{
    const std::string msg = "'for ... of' is removed; write 'foreach (Type x in c)'";
    for (const char* text : {"for (x of c) f(x);", "for (var x of c) f(x);",
                             "for (Actor* x of c) f(x);", "for (const var x of c) f(x);"}) {
        const std::vector<std::string> e = errors_of(Parsed(text));
        ASSERT_FALSE(e.empty()) << text;
        EXPECT_NE(e[0].find(msg), std::string::npos) << text << ": " << e[0];
    }
}

TEST(StmtParse, ReservedWords)
{
    for (const char* w : {"var", "foreach", "in", "using", "override", "virtual", "new", "public",
                          "private", "protected", "get", "set", "finally", "void"}) {
        for (const std::string& text :
             {std::string("var ") + w + " = 1;", std::string("int ") + w + " = 1;",
              std::string("const int ") + w + " = 1;", std::string(w) + " = 1;",
              std::string("foreach (int ") + w + " in xs) f();"}) {
            EXPECT_FALSE(errors_of(Parsed(text)).empty()) << text;
        }
    }
    // A member after a "." can have a word that is reserved as a name.
    EXPECT_EQ(round_trip("p.get().x = q.set(1);"), "p::get().x = q::set(1);");
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
    EXPECT_EQ(round_trip("try { f(); } catch (int e) { g(e); }"),
              "try { f(); } catch (int e) { g(e); }");
    EXPECT_EQ(round_trip("try { f(); } catch (std.exception& e) { g(); }\n"
                         "catch (const std.string& m) { throw; } catch (...) { }"),
              "try { f(); } catch (std::exception& e) { g(); } "
              "catch (const std::string& m) { throw; } catch (...) {}");
    EXPECT_EQ(round_trip("try { try { throw 1; } catch (...) { throw; } } catch (int e) { }"),
              "try { try { throw 1; } catch (...) { throw; } } catch (int e) {}");
    // The words are keywords only as whole words.
    Parsed p("tryit = 1; thrown = 2; catcher(3);");
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_EQ(p.block.stmts.size(), 3u);
    for (const auto& s : p.block.stmts) EXPECT_TRUE(std::holds_alternative<ast::ExprStmt>(s->v));
    {
        Parsed q("try { f(); } catch (int e) { }\nthrow;");
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

TEST(StmtParse, Finally)
{
    // A finally block with catch clauses and without catch clauses.
    {
        Parsed p("try { f(); } catch (int e) { g(e); } finally { h(); }");
        EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.block.stmts.size(), 1u);
        const auto* t = std::get_if<ast::TryStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(t, nullptr);
        ASSERT_EQ(t->catches.size(), 1u);
        EXPECT_EQ(t->catches[0].name, "e");
        ASSERT_TRUE(t->finally_block.has_value());
        EXPECT_EQ(t->finally_block->stmts.size(), 1u);
        EXPECT_EQ(t->finally_block->pos, p.text.rfind('{'));
    }
    {
        Parsed p("try { f(); } finally { h(); i(); }\nthrow;");
        EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.block.stmts.size(), 2u);
        const auto* t = std::get_if<ast::TryStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(t, nullptr);
        EXPECT_TRUE(t->catches.empty());
        ASSERT_TRUE(t->finally_block.has_value());
        EXPECT_EQ(t->finally_block->stmts.size(), 2u);
        EXPECT_TRUE(std::holds_alternative<ast::ThrowStmt>(p.block.stmts[1]->v));
    }
    {
        // A try without a finally block has no finally_block.
        Parsed p("try { f(); } catch (...) { }");
        const auto* t = std::get_if<ast::TryStmt>(&p.block.stmts.at(0)->v);
        ASSERT_NE(t, nullptr);
        EXPECT_FALSE(t->finally_block.has_value());
    }
    EXPECT_EQ(round_trip("try { f(); } catch (int e) { } finally { g(); }"),
              "{ script::Finally __fin_0([&] { g(); }); try { f(); } catch (int e) {} }");
    EXPECT_EQ(round_trip("try { f(); } finally { }"),
              "{ script::Finally __fin_0([&] {}); { f(); } }");
    {
        // "finally" is a reserved word, so a name cannot be "finally".
        Parsed p("try { f(); } finally;");
        EXPECT_FALSE(p.diag.items.empty());
    }
}

TEST(StmtParse, ExceptionErrors)
{
    {
        Parsed p("try { f(); }\nb = 1;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:1: expected 'catch' or 'finally'"}))
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
        Parsed p("try { } catch (std::exception& e) { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:19: use '.' instead of '::'"}))
            << p.diag.format("t");
    }
    {
        Parsed p("try { } catch () { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:16: expected a type"}))
            << p.diag.format("t");
    }
    {
        // The old form "catch (name: type)" gives a clear error.
        Parsed p("try { } catch (e: int) { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{
                                    "1:16: catch syntax changed; write 'catch (Type name)'"}))
            << p.diag.format("t");
    }
    {
        // A pointer type and a qualified type.
        EXPECT_EQ(round_trip("try { f(); } catch (Actor* a) { } catch (std.runtime_error e) { }"),
                  "try { f(); } catch (Actor* a) {} catch (std::runtime_error e) {}");
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
        "var f = (int a) => { if (a) { g((int b) => { return b + 1; }); } return a; };";
    EXPECT_EQ(round_trip(src),
              "auto f = [=](int a) { if (a) { g([=](int b) { return b + 1; }); } return a; };");
    EXPECT_EQ(round_trip("run(() => { x = 1; });"), "run([=]() { x = 1; });");
    // An error inside the lambda has an absolute position.
    Parsed p("a;\nb;\nvar f = (int a) => {\n  x = ;\n};");
    EXPECT_EQ(errors_of(p), (std::vector<std::string>{"4:7: expected an expression"}));
}

TEST(StmtParse, Positions)
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

TEST(StmtParse, OffsetsInBuffer)
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

TEST(StmtParse, Errors)
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

// A "." in the text of a type is a scope. The parser writes "::". A "::" is an error.
TEST(StmtParse, TypeScope)
{
    EXPECT_EQ(round_trip("std.string a = x; std.vector<std.string> b;"),
              "std::string a = x; std::vector<std::string> b = std::vector<std::string>();");
    EXPECT_EQ(round_trip("A.B.C* c; std.array<int, 3> d; float e;"),
              "A::B::C* c = std::type_identity_t<A::B::C*>(); "
              "std::array<int, 3> d = std::array<int, 3>(); float e = float();");
    Parsed p("int a;\nstd::string b = x;");
    EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:4: use '.' instead of '::'"}));
    Parsed q("foreach (std::string s in xs) f(s);");
    EXPECT_EQ(errors_of(q), (std::vector<std::string>{"1:13: use '.' instead of '::'"}));
}

TEST(StmtParse, Truncation)
{
    // Each prefix of a long block ends: no crash and no hang.
    const std::string full =
        "var a = 1;\nconst std.vector<int> b = f(1, 2);\nif (a < b) { f(a); } else g(b);\n"
        "while (a) a--;\ndo { a++; } while (a < 3);\n"
        "for (var i = 0; i < n; i++) { f(i); }\nforeach (Actor* x in xs) f(x);\n"
        "for (i = 0, j = 1; i < n; i++, j--) ;\nswitch (a) { case 1: case A.B: f(); break;\n"
        "default: { g(); } }\nvar h = (float x) => { if (x) { return x; } return 0; };\n"
        "return a + \"s;}\" + 'c' /* c */;\n// end\n"
        "a = 1;\nb = 2;\nc = 3;\nd = 4;\ne = 5;\nf = 6;\ng = 7;\nh = 8;\ni = 9;\nj = 10;\n"
        "k = 11;\nl = 12;\nm = 13;\nn = 14;\no = 15;\np = 16;\nq = 17;\nr = 18;\ns = 19;\nt = 20;\n"
        "u = 21;\nv = 22;\nw = 23;\nx = 24;\ny = 25;\nz = 26;\nbreak;\ncontinue;\n"
        "if (a) {\n  b = 1;\n  while (c) {\n    d();\n  }\n}\nforeach (var k in ks) {\n  f(k);\n}\n"
        "switch (a) {\n  case 1:\n    f();\n  default:\n    g();\n}\nint z;\nz = 1;\nz = 2;\nz = 3;\n"
        "try {\n  f();\n} catch (const std.exception& e) {\n  throw;\n} catch (...) {\n  throw 1;\n}\n";
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

// Parses a script. The value is the parameters of the first method, as "type name" items.
std::vector<std::string> method_params(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    std::vector<std::string> out;
    if (script.classes.empty() || script.classes[0].methods.empty()) return out;
    for (const ast::Param& p : script.classes[0].methods[0].params)
        out.push_back(p.type + "|" + p.name);
    return out;
}

TEST(ScriptParse, TypeFirstParameters)
{
    Diagnostics sd;
    EXPECT_EQ(method_params("class W : B {\n"
                            "    void f(Actor* a, std.string s, const T& r,\n"
                            "               std.map<int, std.vector<float>> m) { }\n"
                            "}\n",
                            sd),
              (std::vector<std::string>{"Actor*|a", "std::string|s", "const T&|r",
                                        "std::map<int, std::vector<float>>|m"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(ScriptParse, RefAndOutParameters)
{
    const std::string text =
        "class W : B {\n"
        "    void f(ref Vec3 a, out Actor b, int c) { var r = ref a; Fill(out b); }\n"
        "}\n";
    Diagnostics sd;
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    ASSERT_EQ(script.classes.size(), 1u);
    ASSERT_EQ(script.classes[0].methods.size(), 1u);
    const auto& ps = script.classes[0].methods[0].params;
    ASSERT_EQ(ps.size(), 3u);
    EXPECT_EQ(ps[0].mode, ast::PassMode::Ref);
    EXPECT_EQ(ps[0].type, "Vec3");
    EXPECT_EQ(ps[1].mode, ast::PassMode::Out);
    EXPECT_EQ(ps[1].type, "Actor");
    EXPECT_EQ(ps[2].mode, ast::PassMode::Value);
    EXPECT_EQ(script.classes[0].methods[0].body.stmts.size(), 2u);
}

TEST(ScriptParse, RefAndOutAreNotNames)
{
    Diagnostics sd;
    method_params("class W : B {\n    void f(int ref) { }\n}\n", sd);
    EXPECT_FALSE(sd.items.empty());
    Diagnostics sd2;
    method_params("class W : B {\n    void f(int out) { }\n}\n", sd2);
    EXPECT_FALSE(sd2.items.empty());
}

// Parses a class. The value lists the members: "method ret|name" and "field type|name|init".
std::vector<std::string> members_of(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    std::vector<std::string> out;
    if (script.classes.empty()) return out;
    for (const ast::Field& f : script.classes[0].fields)
        out.push_back("field " + f.type + "|" + f.name + "|" + (f.has_init ? "init" : "-") +
                      "|" + f.alias);
    for (const ast::Method& m : script.classes[0].methods)
        out.push_back("method " + m.ret + "|" + m.name);
    return out;
}

// Parses a class. The value lists the properties: "type|name|get|set|init" with an access
// letter and "auto" or "body" for each accessor.
std::vector<std::string> props_of(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    std::vector<std::string> out;
    if (script.classes.empty()) return out;
    auto acc = [](ast::Access a) {
        return a == ast::Access::Public ? "u" : a == ast::Access::Private ? "r" : "o";
    };
    for (const ast::Property& p : script.classes[0].props) {
        std::string s = p.type + "|" + p.name + "|" + acc(p.access);
        s += p.has_get ? std::string("|get:") + acc(p.get_access) + (p.get_body ? ":body" : ":auto") : "|-";
        s += p.has_set ? std::string("|set:") + acc(p.set_access) + (p.set_body ? ":body" : ":auto") : "|-";
        s += p.has_init ? (p.init ? "|init" : "|bad-init") : "|-";
        if (p.get_body) s += "|g" + std::to_string(p.get_body->stmts.size());
        if (p.set_body) s += "|s" + std::to_string(p.set_body->stmts.size());
        out.push_back(s);
    }
    return out;
}

TEST(ScriptParse, Properties)
{
    Diagnostics sd;
    EXPECT_EQ(props_of("class A : B {\n"
                       "    int A1 { get; set; }\n"
                       "    std.string A2 { get; } = \"x\";\n"
                       "    private float A3 { get; protected set; } = 1.0;\n"
                       "    int A4 {\n"
                       "        get { return this.v; }\n"
                       "        private set { this.v = value; f(value); }\n"
                       "    }\n"
                       "    int value = 0;\n"
                       "}\n",
                       sd),
              (std::vector<std::string>{
                  "int|A1|u|get:u:auto|set:u:auto|-",
                  "std::string|A2|u|get:u:auto|-|init",
                  "float|A3|r|get:r:auto|set:o:auto|init",
                  "int|A4|u|get:u:body|set:r:body|-|g1|s2",
              }));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(ScriptParse, PropertyErrors)
{
    for (const auto& [text, msg] : std::vector<std::pair<std::string, std::string>>{
             {"int N { }", "a property needs a 'get' or a 'set' accessor"},
             {"int N { set; set; }", "duplicate 'set' accessor"},
             {"int N { get { return 1; } set; }", "a property cannot mix auto and custom accessors"},
             {"int N { get { return 1; } } = 2;", "only an auto property can have an initializer"},
             {"int N { get; value; }", "expected 'get' or 'set'"},
             {"N { get; }", "a property needs a type"},
             {"int get;", "'get' is a reserved word"},
             {"int set = 1;", "'set' is a reserved word"},
             {"void get() { }", "'get' is a reserved word"},
             {"void f(int set) { }", "'set' is a reserved word"},
         }) {
        Diagnostics sd;
        (void)props_of("class A : B {\n    " + text + "\n}\n", sd);
        ASSERT_FALSE(sd.items.empty()) << text;
        EXPECT_NE(sd.format("t").find(msg), std::string::npos) << text << ": " << sd.format("t");
    }
}

TEST(ScriptParse, TypeFirstMembers)
{
    Diagnostics sd;
    EXPECT_EQ(members_of("class A : B {\n"
                         "    std.string s;\n"
                         "    float hp = 10.0;\n"
                         "    [alias(\"old\")] int n = 0;\n"
                         "    std.vector<std.string> v;\n"
                         "    void tick(float dt) { }\n"
                         "    int* pick(int* p) { return p; }\n"
                         "    std.string name() { return this.s; }\n"
                         "}\n",
                         sd),
              (std::vector<std::string>{
                  "field std::string|s|-|", "field float|hp|init|", "field int|n|init|old",
                  "field std::vector<std::string>|v|-|", "method |tick", "method int*|pick",
                  "method std::string|name"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(ScriptParse, ParameterErrors)
{
    {
        // The old form "name: type" gives a clear error.
        Diagnostics sd;
        method_params("class W : B {\n    void f(dt: float) { }\n}\n", sd);
        ASSERT_GE(sd.items.size(), 1u);
        EXPECT_NE(sd.format("t").find("t:2:12: error: parameter syntax changed; write 'Type name'"),
                  std::string::npos)
            << sd.format("t");
    }
    {
        // A method parameter needs a type.
        Diagnostics sd;
        method_params("class W : B {\n    void f(dt) { }\n}\n", sd);
        EXPECT_NE(sd.format("t").find("a parameter needs a type; write 'Type name'"),
                  std::string::npos)
            << sd.format("t");
    }
}

}  // namespace

// ------------------------------------------------------------------ member modifiers
namespace {

// Parses a class. The value is the first method, or null when there is none.
struct ModParsed {
    std::string text;
    LineMap map;
    Diagnostics sd;
    ast::Script script;

    explicit ModParsed(std::string t) : text(std::move(t)), map(text)
    {
        script = parse_script(text, map, sd);
    }
    const ast::Method& method(size_t i = 0) const { return script.classes.at(0).methods.at(i); }
    const ast::Field& field(size_t i = 0) const { return script.classes.at(0).fields.at(i); }
    // The errors as "line:col: text", one in each item.
    std::vector<std::string> errors() const
    {
        std::vector<std::string> r;
        for (const Diag& d : sd.items)
            r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
        return r;
    }
};

std::string member_class(const std::string& member)
{
    return "class A : B {\n    " + member + "\n}\n";
}

}  // namespace

TEST(ModifierParse, EachModifierOnAMethod)
{
    {
        ModParsed p(member_class("override void Tick(float dt) { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_override);
        EXPECT_FALSE(p.method().is_virtual);
        EXPECT_FALSE(p.method().is_new);
        EXPECT_EQ(p.method().access, ast::Access::Public);
        EXPECT_EQ(p.method().name, "Tick");
        EXPECT_EQ(p.method().params.size(), 1u);
    }
    {
        ModParsed p(member_class("virtual int Hp() { return 1; }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_virtual);
        EXPECT_FALSE(p.method().is_override);
        EXPECT_EQ(p.method().ret, "int");
    }
    {
        ModParsed p(member_class("new void Tick() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_new);
    }
    {
        ModParsed p(member_class("public void A() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.method().access, ast::Access::Public);
    }
    {
        ModParsed p(member_class("private void A() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.method().access, ast::Access::Private);
    }
    {
        ModParsed p(member_class("protected void A() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.method().access, ast::Access::Protected);
    }
    {
        ModParsed p(member_class("void A() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_FALSE(p.method().is_override || p.method().is_virtual || p.method().is_new);
        EXPECT_EQ(p.method().access, ast::Access::Public);
    }
}

TEST(ModifierParse, AccessOnAField)
{
    {
        ModParsed p(member_class("private int hp = 3;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().access, ast::Access::Private);
        EXPECT_EQ(p.field().type, "int");
        EXPECT_EQ(p.field().name, "hp");
        EXPECT_TRUE(p.field().has_init);
    }
    {
        ModParsed p(member_class("protected std.string s;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().access, ast::Access::Protected);
        EXPECT_EQ(p.field().type, "std::string");
    }
    {
        ModParsed p(member_class("public float f;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().access, ast::Access::Public);
    }
    {
        ModParsed p(member_class("float f;"));
        EXPECT_EQ(p.field().access, ast::Access::Public);
    }
}

TEST(ModifierParse, AnyOrderAndAlias)
{
    {
        ModParsed p(member_class("public virtual void Tick(float dt) { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_virtual);
        EXPECT_EQ(p.method().access, ast::Access::Public);
    }
    {
        ModParsed p(member_class("virtual protected void Tick(float dt) { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_virtual);
        EXPECT_EQ(p.method().access, ast::Access::Protected);
    }
    {
        ModParsed p(member_class("private override void Tick(float dt) { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_override);
        EXPECT_EQ(p.method().access, ast::Access::Private);
    }
    {
        ModParsed p(member_class("[alias(\"old\")] private int n;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().alias, "old");
        EXPECT_EQ(p.field().access, ast::Access::Private);
    }
    {
        // A name that starts with a modifier word is a plain name.
        ModParsed p(member_class("newton publicity;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().type, "newton");
    }
}

TEST(ModifierParse, Errors)
{
    {
        ModParsed p(member_class("virtual virtual void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:13: duplicate modifier 'virtual'"}));
    }
    {
        ModParsed p(member_class("public void f() { }\n    public private void g() { }"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"3:12: 'public' and 'private' cannot combine"}));
    }
    {
        ModParsed p(member_class("protected public int x;"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:15: 'protected' and 'public' cannot combine"}));
    }
    {
        ModParsed p(member_class("override virtual void f() { }"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:14: 'override' and 'virtual' cannot combine"}));
    }
    {
        ModParsed p(member_class("virtual new void f() { }"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:13: 'virtual' and 'new' cannot combine"}));
    }
    {
        ModParsed p(member_class("new override void f() { }"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:9: 'new' and 'override' cannot combine"}));
    }
    {
        ModParsed p(member_class("override int x;"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:5: 'override' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("public virtual int x;"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:12: 'virtual' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("new int x = 1;"));
        EXPECT_EQ(p.errors(),
                  (std::vector<std::string>{"2:5: 'new' is only valid on a method"}));
    }
}

// ---------------------------------------------------------------- class stubs and attributes
namespace {

struct StubParsed {
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::Script script;

    explicit StubParsed(std::string t) : text(std::move(t)), map(text)
    {
        script = parse_script(text, map, diag);
    }
};

}  // namespace

TEST(StubParse, NativeValueString)
{
    StubParsed p("[native, value] class std.string;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_TRUE(p.script.classes.empty());
    EXPECT_EQ(p.script.stubs[0].name, "std::string");
    EXPECT_TRUE(p.script.stubs[0].is_native);
    EXPECT_TRUE(p.script.stubs[0].is_value);
}

TEST(StubParse, Handle)
{
    StubParsed p("[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_EQ(p.script.stubs[0].name, "Weapon");
    EXPECT_EQ(p.script.stubs[0].handle, "std::shared_ptr<$T>");
    EXPECT_FALSE(p.script.stubs[0].is_value);
}

TEST(StubParse, TemplatePattern)
{
    StubParsed p("[native, value] class std.vector<*>;\n[native, value] class physics.*;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 2u);
    EXPECT_EQ(p.script.stubs[0].name, "std::vector<*>");
    EXPECT_EQ(p.script.stubs[1].name, "physics::*");
}

TEST(StubParse, Intrusive)
{
    StubParsed p("[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Object;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_EQ(p.script.stubs[0].handle, "$T*");
    EXPECT_EQ(p.script.stubs[0].intrusive_add, "AddRef");
    EXPECT_EQ(p.script.stubs[0].intrusive_release, "Release");
}

TEST(StubParse, DefaultHandle)
{
    StubParsed p("[native, default_handle(\"std::shared_ptr<$T>\")] class *;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_EQ(p.script.stubs[0].name, "*");
    EXPECT_EQ(p.script.stubs[0].default_handle, "std::shared_ptr<$T>");
}

TEST(StubParse, ValueScriptClass)
{
    StubParsed p("[value] class Stats : Base { }\nclass Other : Base { }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes.size(), 2u);
    EXPECT_TRUE(p.script.classes[0].is_value);
    EXPECT_FALSE(p.script.classes[1].is_value);
    EXPECT_TRUE(p.script.stubs.empty());
}

TEST(StubParse, NativeOnClassBodyIsError)
{
    StubParsed p("[native] class Foo : Base { }\n");
    ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
    EXPECT_EQ(p.diag.items[0].msg, "attribute 'native' is not allowed on a script class");
    EXPECT_EQ(p.diag.items[0].pos, 1u);
}

TEST(StubParse, AttributeErrors)
{
    {
        StubParsed p("[native, handle] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "attribute 'handle' takes 1 arguments");
    }
    {
        StubParsed p("[native, native] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "duplicate attribute 'native'");
    }
    {
        StubParsed p("[native, bogus] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "unknown attribute 'bogus'");
    }
    {
        StubParsed p("class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "a stub needs the attribute 'native'");
    }
    {
        StubParsed p("[native, value, handle(\"x\")] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "'value' and 'handle' are exclusive");
    }
    {
        StubParsed p("[native, handle(x)] class A;\n");
        EXPECT_GE(p.diag.items.size(), 1u);
        EXPECT_EQ(p.diag.items[0].msg, "expected a string in [alias(...)]");
    }
}

TEST(StubParse, FieldAlias)
{
    {
        StubParsed p("class A : B { [alias(\"x\")] int y; }\n");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.script.classes.size(), 1u);
        ASSERT_EQ(p.script.classes[0].fields.size(), 1u);
        EXPECT_EQ(p.script.classes[0].fields[0].alias, "x");
    }
    {
        StubParsed p("class A : B { [alias] int y; }\n");
        EXPECT_GE(p.diag.items.size(), 1u);
    }
    {
        StubParsed p("class A : B { [native] int y; }\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "unknown attribute 'native'");
    }
}
