// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the statements: simple statements, control flow, switch and lambda.
// Most tests are round trips: parse the block, print each statement and compare the text.
// The printer writes a fixed layout, so the expected text can differ in spacing.
#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace stmt_test;

TEST(StatementParse, Simple)
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

TEST(StatementParse, Control)
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

TEST(StatementParse, For)
{
    EXPECT_EQ(round_trip("for (var i = 0; i < n; i++) f(i);"), "for (auto i = 0; i < n; i++) f(i);");
    EXPECT_EQ(round_trip("for (int i = 0; i < n; i++, j--) { }"), "for (int32_t i = 0; i < n; i++, j--) {}");
    EXPECT_EQ(round_trip("for (;;) break;"), "for (;;) break;");
    EXPECT_EQ(round_trip("for (i = 0, j = 1; i < n;) f();"), "for (i = 0, j = 1; i < n;) f();");
    EXPECT_EQ(round_trip("for (var i = f(1, 2); ; ) x;"), "for (auto i = f(1, 2);;) x;");
    // foreach
    EXPECT_EQ(round_trip("foreach (var x in xs) f(x);"), "for (auto x : xs) f(x);");
    EXPECT_EQ(round_trip("foreach (const var x in this.items) { f(x); }"), "for (const auto x : this.items) { f(x); }");
    EXPECT_EQ(round_trip("foreach (Actor* x in actors) f(x);"), "for (Actor* x : actors) f(x);");
    EXPECT_EQ(round_trip("foreach (std.vector<int> x in g()) f(x);"), "for (\x01std::vector<int32_t> x : g()) f(x);");
    // A name that has "in" inside is not the keyword.
    EXPECT_EQ(round_trip("foreach (Din x in xs) f(x);"), "for (Din x : xs) f(x);");
    EXPECT_EQ(round_trip("foreach (const Actor* a in xs) f(a);"), "for (const Actor* a : xs) f(a);");
    EXPECT_EQ(round_trip("foreach (var of in xs) f(of);"), "for (auto of : xs) f(of);");
    EXPECT_EQ(
        round_trip("foreach (var x in xs) foreach (int y in x) f(y);"), "for (auto x : xs) for (int32_t y : x) f(y);");
    // The old text "of" as a name in a classic for.
    EXPECT_EQ(round_trip("for (var i = of; i < 3; i++) f();"), "for (auto i = of; i < 3; i++) f();");
}

TEST(StatementParse, ForEachRef)
{
    EXPECT_EQ(round_trip("foreach (var& x in xs) f(x);"), "for (auto& x : xs) f(x);");
    EXPECT_EQ(round_trip("foreach (const var& x in xs) f(x);"), "for (const auto& x : xs) f(x);");
}

TEST(StatementParse, ReservedWords)
{
    for (const char* w : {"var", "foreach", "in", "using", "override", "virtual", "new", "public", "private",
             "protected", "get", "set", "finally", "void"})
    {
        for (const std::string& text : {std::string("var ") + w + " = 1;", std::string("int ") + w + " = 1;",
                 std::string("const int ") + w + " = 1;", std::string(w) + " = 1;",
                 std::string("foreach (int ") + w + " in xs) f();"})
        {
            EXPECT_FALSE(errors_of(Parsed(text)).empty()) << text;
        }
    }
    // A member after a "." can have a word that is reserved as a name.
    EXPECT_EQ(round_trip("p.get().x = q.set(1);"), "p::get().x = q::set(1);");
}

TEST(StatementParse, Switch)
{
    EXPECT_EQ(round_trip("switch (x) { case 1: a(); break; case 2: case 3: b(); default: c(); }"),
        "switch (x) { case 1: a(); break; case 2: case 3: b(); default: c(); }");
    EXPECT_EQ(round_trip("switch (x) { case A.B: f(); break; case 'a': g(); }"),
        "switch (x) { case A::B: f(); break; case 'a': g(); }");
    EXPECT_EQ(round_trip("switch (x) { case (a ? 1 : 2): f(); }"), "switch (x) { case (a ? 1 : 2): f(); }");
    EXPECT_EQ(round_trip("switch (x) { case 1: default: f(); case 2: { g(); } }"),
        "switch (x) { case 1: default: f(); case 2: { g(); } }");
    EXPECT_EQ(round_trip("switch (x) { default: case 1: f(); }"), "switch (x) { default: case 1: f(); }");
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

TEST(StatementParse, Keywords)
{
    // A name that starts with a keyword is a name.
    Parsed p("ifx = 1; letter = 2; forx(3); doing = whiley; returned = 1; ofx = 1;");
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.block.stmts.size(), 6u);
    for (const auto& s : p.block.stmts)
        EXPECT_TRUE(std::holds_alternative<ast::ExprStmt>(s->v));
    EXPECT_EQ(p.printed(), "ifx = 1; letter = 2; forx(3); doing = whiley; returned = 1; ofx = 1;");
}

TEST(StatementParse, Lambda)
{
    // Statement, expression, block, lambda: the three grammars together.
    const std::string src = "var f = (int a) => { if (a) { g((int b) => { return b + 1; }); } return a; };";
    EXPECT_EQ(
        round_trip(src), "auto f = [=](int32_t a) { if (a) { g([=](int32_t b) { return b + 1; }); } return a; };");
    EXPECT_EQ(round_trip("run(() => { x = 1; });"), "run([=]() { x = 1; });");
    // An error inside the lambda has an absolute position.
    Parsed p("a;\nb;\nvar f = (int a) => {\n  x = ;\n};");
    EXPECT_EQ(errors_of(p), (std::vector<std::string>{"4:7: expected an expression"}));
}

TEST(StatementParse, SwitchWithStringLabels)
{
    EXPECT_EQ(round_trip("switch (s) { case \"a\": f(); break; case \"b\": case \"c\": g(); break; default: h(); }"),
        "switch (s) { case \"a\": f(); break; case \"b\": case \"c\": g(); break; default: h(); }");
}
