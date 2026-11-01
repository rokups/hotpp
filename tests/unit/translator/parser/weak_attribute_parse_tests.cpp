// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the "[weak]" attribute (task 38) on fields, parameters and locals.
#include "emit.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/script_parse_test_support.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace hotc;

TEST(WeakAttributeParse, Field)
{
    // The field style: the attribute is on its own line.
    script_test::StubParsed p("class A {\n    [weak]\n    Weapon w;\n    Weapon s;\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes.size(), 1u);
    ASSERT_EQ(p.script.classes[0].fields.size(), 2u);
    EXPECT_TRUE(p.script.classes[0].fields[0].weak);
    EXPECT_FALSE(p.script.classes[0].fields[1].weak);
}

TEST(WeakAttributeParse, FieldWithAlias)
{
    script_test::StubParsed p("class A {\n    [weak, alias(\"old\")]\n    Weapon w;\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes[0].fields.size(), 1u);
    EXPECT_TRUE(p.script.classes[0].fields[0].weak);
    EXPECT_EQ(p.script.classes[0].fields[0].alias, "old");
}

TEST(WeakAttributeParse, Parameter)
{
    script_test::StubParsed p("class A {\n    void F([weak] Weapon w, Weapon s) { }\n    A([weak] Weapon w) { }\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const ast::ClassDef& c = p.script.classes[0];
    ASSERT_EQ(c.methods.size(), 1u);
    ASSERT_EQ(c.methods[0].params.size(), 2u);
    EXPECT_TRUE(c.methods[0].params[0].weak);
    EXPECT_EQ(c.methods[0].params[0].type, "Weapon");
    EXPECT_EQ(c.methods[0].params[0].name, "w");
    EXPECT_FALSE(c.methods[0].params[1].weak);
    ASSERT_EQ(c.ctors.size(), 1u);
    ASSERT_EQ(c.ctors[0].params.size(), 1u);
    EXPECT_TRUE(c.ctors[0].params[0].weak);
}

TEST(WeakAttributeParse, NotOnOtherMembers)
{
    {
        script_test::StubParsed p("class A {\n    [weak]\n    void F() { }\n}\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "[weak] applies to fields, parameters and local variables only");
    }
    {
        script_test::StubParsed p("class A {\n    [weak(\"x\")]\n    Weapon w;\n}\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "attribute 'weak' takes 0 arguments");
    }
    {
        script_test::StubParsed p("void F([alias(\"x\")] int a) { }\n");
        EXPECT_GE(p.diag.items.size(), 1u);
    }
    {
        script_test::StubParsed p("[weak, native] class W;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "attribute 'weak' is not allowed on a stub: put it on a field, a parameter or a local");
    }
    {
        script_test::StubParsed p("[weak] Weapon g;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "attribute 'weak' is not allowed on a module variable");
    }
}

TEST(WeakAttributeParse, Local)
{
    stmt_test::Parsed p("[weak] Weapon w = s; Weapon t = w; [weak] const Weapon c = s;");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.block.stmts.size(), 3u);
    const auto* w = std::get_if<ast::VarDeclStmt>(&p.block.stmts[0]->v);
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(w->weak);
    EXPECT_EQ(w->weak_pos, 1u);
    EXPECT_EQ(w->name, "w");
    EXPECT_EQ(w->type, "Weapon");
    const auto* t = std::get_if<ast::VarDeclStmt>(&p.block.stmts[1]->v);
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->weak);
    const auto* c = std::get_if<ast::VarDeclStmt>(&p.block.stmts[2]->v);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->weak);
    EXPECT_TRUE(c->is_const);
}

TEST(WeakAttributeParse, LocalErrors)
{
    using stmt_test::errors_of;
    using stmt_test::Parsed;
    const std::string untyped =
        "[weak] needs the handle type of the local: write '[weak] T name', not 'var' or a structured binding";
    EXPECT_EQ(errors_of(Parsed("[weak] var w = s;")), (std::vector<std::string>{"1:2: " + untyped}));
    EXPECT_EQ(errors_of(Parsed("[weak] var& w = s;")), (std::vector<std::string>{"1:2: " + untyped}));
    EXPECT_EQ(errors_of(Parsed("[weak] var (a, b) = s;")), (std::vector<std::string>{"1:2: " + untyped}));
    EXPECT_EQ(errors_of(Parsed("[weak, weak] Weapon w;")),
        (std::vector<std::string>{"1:8: duplicate attribute 'weak'"}));
    EXPECT_EQ(errors_of(Parsed("[strong] Weapon w;")),
        (std::vector<std::string>{"1:2: unknown attribute 'strong' on a local variable"}));
    // A for init and a foreach take no attribute.
    EXPECT_FALSE(errors_of(Parsed("for ([weak] Weapon w = s; ; ) { }")).empty());
    EXPECT_FALSE(errors_of(Parsed("foreach ([weak] Weapon w in ws) { }")).empty());
}
