// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of "async" (a method modifier) and "await" (a prefix operator).
#include "parser/parser.h"
#include "support/expression_parse_test_support.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace hotc;
using namespace script_test;

TEST(AsyncParse, AsyncMethod)
{
    ModParsed p(member_class("async int f() { return 1; }"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_TRUE(p.method().is_async);
    EXPECT_FALSE(p.method().is_virtual);
    EXPECT_FALSE(p.method().is_override);
    EXPECT_EQ(p.method().name, "f");
    EXPECT_EQ(p.method().ret, "int32_t");
}

TEST(AsyncParse, MethodWithoutAsync)
{
    ModParsed p(member_class("int f() { return 1; }"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_FALSE(p.method().is_async);
}

TEST(AsyncParse, WithOtherModifiers)
{
    {
        ModParsed p(member_class("virtual async void tick() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_async);
        EXPECT_TRUE(p.method().is_virtual);
    }
    {
        ModParsed p(member_class("override async void tick() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_async);
        EXPECT_TRUE(p.method().is_override);
    }
    {
        ModParsed p(member_class("async override void tick() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_async);
        EXPECT_TRUE(p.method().is_override);
    }
    {
        ModParsed p(member_class("public async int load(int id) { return id; }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_async);
        EXPECT_EQ(p.method().access, ast::Access::Public);
        EXPECT_EQ(p.method().params.size(), 1u);
    }
    {
        ModParsed p(member_class("private async void run() { }"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_async);
        EXPECT_EQ(p.method().access, ast::Access::Private);
    }
}

TEST(AsyncParse, Errors)
{
    {
        ModParsed p(member_class("async async void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:11: duplicate modifier 'async'"}));
    }
    {
        ModParsed p(member_class("async int x;"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'async' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("public async int x = 1;"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: 'async' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("async int P { get; set; }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'async' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("async A() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'async' is not valid on a constructor"}));
    }
    {
        ModParsed p(member_class("async ~A() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'async' is not valid on a destructor"}));
    }
}

TEST(AsyncParse, WordsAreNotNames)
{
    EXPECT_FALSE(ModParsed(member_class("int await;")).errors().empty());
    EXPECT_FALSE(ModParsed(member_class("int async;")).errors().empty());
    EXPECT_FALSE(ModParsed(member_class("void f() { int await; }")).errors().empty());
    EXPECT_FALSE(ModParsed(member_class("void f() { var async = 1; }")).errors().empty());
    EXPECT_FALSE(ModParsed(member_class("void f(int await) { }")).errors().empty());
    EXPECT_FALSE(ModParsed(member_class("void f() { g(async); }")).errors().empty());
    // A member name after a "." can be any word: a native function can have the name.
    EXPECT_TRUE(ModParsed(member_class("void f() { std.async(1); }")).errors().empty());
}

TEST(AwaitParse, UnaryNode)
{
    using namespace expr_test;
    Parsed p("await a.b(1)");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(p.diag.items.size(), 0u);
    const auto* u = std::get_if<ast::UnaryExpr>(&p.expr->v);
    ASSERT_TRUE(u);
    EXPECT_EQ(u->op, "await");
    EXPECT_FALSE(u->postfix);
    ASSERT_TRUE(u->operand);
    EXPECT_TRUE(std::get_if<ast::CallExpr>(&u->operand->v));
    EXPECT_EQ(ast::pos_of(*p.expr), 0u);
}

TEST(AwaitParse, Precedence)
{
    using namespace expr_test;
    {
        Parsed p("await x + 1");
        ASSERT_TRUE(p.expr);
        EXPECT_EQ(p.diag.items.size(), 0u);
        const auto* b = std::get_if<ast::BinaryExpr>(&p.expr->v);
        ASSERT_TRUE(b);
        EXPECT_EQ(b->op, "+");
        const auto* u = std::get_if<ast::UnaryExpr>(&b->lhs->v);
        ASSERT_TRUE(u);
        EXPECT_EQ(u->op, "await");
    }
    {
        Parsed p("-await x");
        ASSERT_TRUE(p.expr);
        const auto* u = std::get_if<ast::UnaryExpr>(&p.expr->v);
        ASSERT_TRUE(u);
        EXPECT_EQ(u->op, "-");
        const auto* in = std::get_if<ast::UnaryExpr>(&u->operand->v);
        ASSERT_TRUE(in);
        EXPECT_EQ(in->op, "await");
    }
    {
        Parsed p("await await f()");
        ASSERT_TRUE(p.expr);
        EXPECT_EQ(p.diag.items.size(), 0u);
    }
    EXPECT_EQ(round_trip("await f(1)"), "await f(1)");
    EXPECT_EQ(round_trip("(await f()).x"), "(await f()).x");
    EXPECT_EQ(round_trip("a = await f()"), "a = await f()");
}

TEST(AwaitParse, InAStatement)
{
    ModParsed p(member_class("async void f() { await g(); var x = await h(1); return; }"));
    EXPECT_TRUE(p.errors().empty()) << (p.errors().empty() ? "" : p.errors()[0]);
}
