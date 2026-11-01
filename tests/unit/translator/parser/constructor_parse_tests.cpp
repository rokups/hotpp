// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the constructor, the base call and the destructor of a script class.
#include "parser/parser.h"
#include "parser/statement.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace hotc;

namespace
{

struct Parsed
{
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::Script script;
    explicit Parsed(std::string body)
        : text("class A : B {\n" + std::move(body) + "\n}\n")
        , map(text)
    {
        script = parse_script(text, map, diag);
    }
    std::string errors() const { return diag.format("t"); }
    const ast::ClassDef& cls() const { return script.classes.at(0); }
};

} // namespace

TEST(ConstructorParse, ConstructorWithSuperCall)
{
    Parsed p("    int n = 1;\n    A(int a, std.string b) : base(a, b) { this.n = a; }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.cls().ctors.size(), 1u);
    const ast::Ctor& c = p.cls().ctors[0];
    EXPECT_EQ(c.params.size(), 2u);
    EXPECT_EQ(c.params[0].type, "int32_t");
    EXPECT_EQ(c.params[1].name, "b");
    EXPECT_TRUE(c.has_base_call);
    EXPECT_EQ(c.base_args.size(), 2u);
    EXPECT_EQ(c.body.stmts.size(), 1u);
    EXPECT_TRUE(p.cls().methods.empty());
}

TEST(ConstructorParse, ConstructorWithoutSuperCall)
{
    Parsed p("    A() { }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.cls().ctors.size(), 1u);
    EXPECT_FALSE(p.cls().ctors[0].has_base_call);
    EXPECT_TRUE(p.cls().ctors[0].params.empty());
}

TEST(ConstructorParse, SuperCallWithoutArguments)
{
    Parsed p("    A(int a) : base() { }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_TRUE(p.cls().ctors[0].has_base_call);
    EXPECT_TRUE(p.cls().ctors[0].base_args.empty());
}

TEST(ConstructorParse, Destructor)
{
    Parsed p("    ~A() { this.n = 0; }\n    int n = 1;");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.cls().dtors.size(), 1u);
    EXPECT_EQ(p.cls().dtors[0].name, "~A");
    EXPECT_EQ(p.cls().dtors[0].body.stmts.size(), 1u);
    EXPECT_EQ(p.cls().fields.size(), 1u);
}

TEST(ConstructorParse, MethodsAndConstructorTogether)
{
    Parsed p("    A(int a) : base(a) { }\n    void Tick(float dt) { }\n    ~A() { }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.cls().ctors.size(), 1u);
    EXPECT_EQ(p.cls().methods.size(), 1u);
    EXPECT_EQ(p.cls().dtors.size(), 1u);
}

TEST(ConstructorParse, Errors)
{
    for (const auto& [text, msg] : std::vector<std::pair<std::string, std::string>>{
             {"C(int a) { }", "a method needs a return type"},
             {"~C() { }", "the name of a destructor is '~A'"},
             {"~A(int a) { }", "a destructor has no parameters"},
             {"~A() : base() { }", "only a constructor can call 'base(...)'"},
             {"void f() : base() { }", "only a constructor can call 'base(...)' after the parameters"},
             {"A() : other() { }", "expected 'base(...)' after ':' of a constructor"},
             {"override A() { }", "'override' is not valid on a constructor"},
             {"virtual ~A() { }", "'virtual' is not valid on a destructor"},
         })
    {
        Parsed p("    " + text);
        ASSERT_FALSE(p.diag.items.empty()) << text;
        EXPECT_NE(p.errors().find(msg), std::string::npos) << text << ": " << p.errors();
    }
}

// ---- a class without a base and a [value] class (design 7.10.1)
TEST(ClassKindParse, ClassWithoutBase)
{
    const std::string text = "class Counter {\n    int count = 0;\n}\n";
    const LineMap map(text);
    Diagnostics diag;
    const ast::Script script = parse_script(text, map, diag);
    ASSERT_TRUE(diag.items.empty()) << diag.format("t");
    ASSERT_EQ(script.classes.size(), 1u);
    EXPECT_EQ(script.classes[0].base, "");
    EXPECT_FALSE(script.classes[0].is_value);
}

TEST(ClassKindParse, ValueClassWithoutBase)
{
    const std::string text = "struct Stats {\n    float hp = 1.0;\n}\n";
    const LineMap map(text);
    Diagnostics diag;
    const ast::Script script = parse_script(text, map, diag);
    ASSERT_TRUE(diag.items.empty()) << diag.format("t");
    ASSERT_EQ(script.classes.size(), 1u);
    EXPECT_TRUE(script.classes[0].is_value);
    EXPECT_EQ(script.classes[0].base, "");
}

TEST(ClassKindParse, ClassWithBaseStillParses)
{
    const std::string text = "class Hero : Actor {\n}\n";
    const LineMap map(text);
    Diagnostics diag;
    const ast::Script script = parse_script(text, map, diag);
    ASSERT_TRUE(diag.items.empty()) << diag.format("t");
    EXPECT_EQ(script.classes.at(0).base, "Actor");
}

// ---- native interface bases (S8b, design 4.4): "class Boss : Actor, IDamageable"
TEST(ClassKindParse, InterfaceBasesFollowThePrimaryBase)
{
    const std::string text = "class Boss : Actor, IDamageable, game.ITagged {\n    int hp = 1;\n}\n";
    const LineMap map(text);
    Diagnostics diag;
    const ast::Script script = parse_script(text, map, diag);
    ASSERT_TRUE(diag.items.empty()) << diag.format("t");
    const ast::ClassDef& c = script.classes.at(0);
    EXPECT_EQ(c.base, "Actor");
    EXPECT_EQ(c.interfaces, (std::vector<std::string>{"IDamageable", "game::ITagged"}));
    EXPECT_EQ(c.fields.size(), 1u);
}

TEST(ClassKindParse, OneBaseHasNoInterfaces)
{
    const std::string text = "class Hero : Actor { }\nclass Plain { }\n";
    const LineMap map(text);
    Diagnostics diag;
    const ast::Script script = parse_script(text, map, diag);
    ASSERT_TRUE(diag.items.empty()) << diag.format("t");
    EXPECT_TRUE(script.classes.at(0).interfaces.empty());
    EXPECT_TRUE(script.classes.at(1).interfaces.empty());
}

TEST(ClassKindParse, BadBaseListIsAnError)
{
    for (const char* text : {"class Boss : Actor, { }\n", "class Boss : Actor IDamageable { }\n",
             "class Boss : , Actor { }\n", "class Boss : Actor, game::I { }\n"})
    {
        const LineMap map(text);
        Diagnostics diag;
        (void)parse_script(text, map, diag);
        EXPECT_FALSE(diag.items.empty()) << text;
    }
}
