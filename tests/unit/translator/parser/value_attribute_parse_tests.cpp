// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the "[value]" use attribute on fields, parameters, return types and locals.
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

TEST(ValueAttributeParse, Field)
{
    script_test::StubParsed p("class A {\n    [value]\n    Pt a;\n    Pt b;\n    [value, alias(\"old\")]\n    Pt c;\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const auto& f = p.script.classes[0].fields;
    ASSERT_EQ(f.size(), 3u);
    EXPECT_TRUE(f[0].value);
    EXPECT_NE(f[0].value_pos, 0u);
    EXPECT_FALSE(f[0].weak);
    EXPECT_FALSE(f[1].value);
    EXPECT_TRUE(f[2].value);
    EXPECT_EQ(f[2].alias, "old");
}

TEST(ValueAttributeParse, Parameter)
{
    script_test::StubParsed p("class A {\n    void F([value] Pt a, Pt b) { }\n    A([value] Pt a) { }\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const ast::ClassDef& c = p.script.classes[0];
    ASSERT_EQ(c.methods.size(), 1u);
    ASSERT_EQ(c.methods[0].params.size(), 2u);
    EXPECT_TRUE(c.methods[0].params[0].value);
    EXPECT_FALSE(c.methods[0].params[0].weak);
    EXPECT_EQ(c.methods[0].params[0].type, "Pt");
    EXPECT_FALSE(c.methods[0].params[1].value);
    ASSERT_EQ(c.ctors.size(), 1u);
    ASSERT_EQ(c.ctors[0].params.size(), 1u);
    EXPECT_TRUE(c.ctors[0].params[0].value);
}

TEST(ValueAttributeParse, MethodReturnType)
{
    script_test::StubParsed p("class A {\n    [value]\n    Pt F() { }\n    Pt G() { }\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const ast::ClassDef& c = p.script.classes[0];
    ASSERT_EQ(c.methods.size(), 2u);
    EXPECT_TRUE(c.methods[0].ret_value);
    EXPECT_NE(c.methods[0].ret_value_pos, 0u);
    EXPECT_EQ(c.methods[0].ret, "Pt");
    EXPECT_FALSE(c.methods[1].ret_value);
}

TEST(ValueAttributeParse, Local)
{
    stmt_test::Parsed p("[value] Pt a = s; Pt b = a; [value] const Pt c = a;");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.block.stmts.size(), 3u);
    const auto* a = std::get_if<ast::VarDeclStmt>(&p.block.stmts[0]->v);
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->value);
    EXPECT_FALSE(a->weak);
    EXPECT_EQ(a->value_pos, 1u);
    EXPECT_EQ(a->type, "Pt");
    const auto* b = std::get_if<ast::VarDeclStmt>(&p.block.stmts[1]->v);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(b->value);
    const auto* c = std::get_if<ast::VarDeclStmt>(&p.block.stmts[2]->v);
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->value);
    EXPECT_TRUE(c->is_const);
}

TEST(ValueAttributeParse, WeakAndValueTogetherIsAnError)
{
    const std::string msg = "[weak] and [value] cannot be used together: a weak handle is not a value";
    {
        script_test::StubParsed p("class A {\n    [weak, value]\n    Pt a;\n}\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, msg);
    }
    {
        script_test::StubParsed p("class A {\n    void F([value, weak] Pt a) { }\n}\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, msg);
    }
    using stmt_test::errors_of;
    using stmt_test::Parsed;
    EXPECT_EQ(errors_of(Parsed("[weak, value] Pt a;")), (std::vector<std::string>{"1:8: " + msg}));
}

TEST(ValueAttributeParse, Errors)
{
    {
        script_test::StubParsed p("class A {\n    [value(\"x\")]\n    Pt a;\n}\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "attribute 'value' takes 0 arguments");
    }
    {
        script_test::StubParsed p("class A {\n    [value, value]\n    Pt a;\n}\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "duplicate attribute 'value'");
    }
    {
        script_test::StubParsed p("class A {\n    [value]\n    int p { get; set; }\n}\n");
        ASSERT_GE(p.diag.items.size(), 1u);
        EXPECT_EQ(p.diag.items[0].msg,
            "[value] applies to fields, parameters, local variables and the return type of a method only");
    }
    {
        script_test::StubParsed p("[value] Pt g;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "[value] is not supported yet on a module variable");
    }
    {
        // A class definition takes no [value]: it is a struct.
        script_test::StubParsed p("[value] class V { int a; }\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "'[value]' is not allowed on a script class: use 'struct'");
    }
    using stmt_test::errors_of;
    using stmt_test::Parsed;
    EXPECT_EQ(errors_of(Parsed("[value, value] Pt a;")), (std::vector<std::string>{"1:9: duplicate attribute 'value'"}));
    EXPECT_FALSE(errors_of(Parsed("for ([value] Pt a = s; ; ) { }")).empty());
    EXPECT_FALSE(errors_of(Parsed("foreach ([value] Pt a in ps) { }")).empty());
}
