// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the member modifiers: override, virtual, new and access.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace script_test;

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
        EXPECT_EQ(p.method().ret, "int32_t");
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
        EXPECT_EQ(p.field().type, "int32_t");
        EXPECT_EQ(p.field().name, "hp");
        EXPECT_TRUE(p.field().has_init);
    }
    {
        ModParsed p(member_class("protected std.string s;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().access, ast::Access::Protected);
        EXPECT_EQ(p.field().type, "\x01std::string");
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
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"3:12: 'public' and 'private' cannot combine"}));
    }
    {
        ModParsed p(member_class("protected public int x;"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:15: 'protected' and 'public' cannot combine"}));
    }
    {
        ModParsed p(member_class("override virtual void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:14: 'override' and 'virtual' cannot combine"}));
    }
    {
        ModParsed p(member_class("virtual new void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:13: 'virtual' and 'new' cannot combine"}));
    }
    {
        ModParsed p(member_class("new override void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:9: 'new' and 'override' cannot combine"}));
    }
    {
        ModParsed p(member_class("override int x;"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'override' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("public virtual int x;"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: 'virtual' is only valid on a method"}));
    }
    {
        ModParsed p(member_class("new int x = 1;"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'new' is only valid on a method"}));
    }
}

TEST(ModifierParse, StaticMethodGoesToStatics)
{
    for (const char* text : {"static int Twice(int x) { return 2 * x; }", "public static int Twice(int x) { return 2 * x; }",
             "static public int Twice(int x) { return 2 * x; }", "static int Twice(int x) => 2 * x;"})
    {
        ModParsed p(member_class(text));
        EXPECT_TRUE(p.errors().empty()) << text;
        const ast::ClassDef& c = p.script.classes.at(0);
        EXPECT_TRUE(c.methods.empty());
        ASSERT_EQ(c.statics.size(), 1u);
        EXPECT_TRUE(c.statics[0].is_static);
        EXPECT_EQ(c.statics[0].name, "Twice");
        EXPECT_EQ(c.statics[0].params.size(), 1u);
        EXPECT_EQ(c.statics[0].body.stmts.size(), 1u);
    }
    {
        ModParsed p(member_class("private static void A() { }\n    void B() { }"));
        EXPECT_TRUE(p.errors().empty());
        const ast::ClassDef& c = p.script.classes.at(0);
        ASSERT_EQ(c.statics.size(), 1u);
        EXPECT_EQ(c.statics[0].access, ast::Access::Private);
        ASSERT_EQ(c.methods.size(), 1u);
        EXPECT_FALSE(c.methods[0].is_static);
    }
    {
        ModParsed p("struct S {\n    static int Make() { return 1; }\n}\n");
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.script.classes.at(0).statics.size(), 1u);
    }
}

TEST(ModifierParse, StaticErrors)
{
    {
        // A static field and a static property are valid (gap G23).
        ModParsed p(member_class("static int count = 0;"));
        EXPECT_TRUE(p.errors().empty());
    }
    {
        ModParsed p(member_class("static int N { get; set; }"));
        EXPECT_TRUE(p.errors().empty());
    }
    {
        ModParsed p(member_class("static A() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: a constructor cannot be 'static'"}));
    }
    {
        ModParsed p(member_class("static virtual void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: 'static' and 'virtual' cannot combine"}));
    }
    {
        ModParsed p(member_class("override static void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'static' and 'override' cannot combine"}));
    }
    {
        ModParsed p(member_class("static new void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: 'static' and 'new' cannot combine"}));
    }
    {
        ModParsed p("abstract class A : B {\n    static abstract void f();\n}\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: 'static' and 'abstract' cannot combine"}));
    }
    {
        ModParsed p(member_class("static async void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: an async function needs an object; remove 'static'"}));
    }
    {
        ModParsed p(member_class("static static void f() { }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:12: duplicate modifier 'static'"}));
    }
    {
        ModParsed p(member_class("static int operator+(int a) { return a; }"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: an operator cannot be 'static'"}));
    }
    {
        ModParsed p("interface I {\n    static int f();\n}\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: an interface member cannot have the modifier 'static'"}));
    }
}

// The script never writes "const" after the parameters: the const inference does not change a method.
TEST(ModifierParse, ConstMarkAfterTheParametersIsAnError)
{
    for (const char* text : {"override int Size() const { return 1; }", "int Size() const => 1;"})
    {
        ModParsed p(member_class(text));
        EXPECT_EQ(p.errors().size(), 1u);
        ASSERT_FALSE(p.errors().empty());
        EXPECT_NE(p.errors()[0].find("const is inferred; remove it"), std::string::npos) << p.errors()[0];
    }
    {
        ModParsed p(member_class("override int Size() { return 1; }"));
        EXPECT_TRUE(p.errors().empty());
    }
}
