// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of a top-level "interface" declaration.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace script_test;

namespace
{

// True when one error has the text.
bool has_error(const ModParsed& p, const std::string& text)
{
    for (const std::string& e : p.errors())
        if (e.find(text) != std::string::npos)
            return true;
    return false;
}

// An interface "I" with one member.
std::string iface(const std::string& member)
{
    return "interface I {\n    " + member + "\n}\n";
}

} // namespace

TEST(InterfaceParse, ValidInterface)
{
    ModParsed p("interface IShape { float Area(); void Scale(float f); }\n");
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.interfaces.size(), 1u);
    const ast::InterfaceDef& d = p.script.interfaces[0];
    EXPECT_EQ(d.name, "IShape");
    EXPECT_EQ(d.line, 1);
    ASSERT_EQ(d.methods.size(), 2u);
    EXPECT_EQ(d.methods[0].name, "Area");
    EXPECT_EQ(d.methods[0].ret, "float");
    EXPECT_TRUE(d.methods[0].params.empty());
    EXPECT_EQ(d.methods[1].name, "Scale");
    EXPECT_EQ(d.methods[1].ret, ""); // a void method has no written type
    ASSERT_EQ(d.methods[1].params.size(), 1u);
    EXPECT_EQ(d.methods[1].params[0].name, "f");
    EXPECT_FALSE(d.methods[1].is_abstract);
    EXPECT_FALSE(d.methods[1].is_virtual);
}

TEST(InterfaceParse, EmptyInterface)
{
    ModParsed p("interface IEmpty { }\n");
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.interfaces.size(), 1u);
    EXPECT_TRUE(p.script.interfaces[0].methods.empty());
}

TEST(InterfaceParse, InterfaceNextToClasses)
{
    ModParsed p("class A {\n    int n;\n}\ninterface I {\n    void F();\n}\nclass B : A {\n    void G() { }\n}\n"
                "interface J { int H(int a, int b); }\n");
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.classes.size(), 2u);
    ASSERT_EQ(p.script.interfaces.size(), 2u);
    EXPECT_EQ(p.script.interfaces[0].name, "I");
    EXPECT_EQ(p.script.interfaces[0].line, 4);
    EXPECT_EQ(p.script.interfaces[1].name, "J");
    EXPECT_EQ(p.script.interfaces[1].methods[0].params.size(), 2u);
    EXPECT_TRUE(p.script.classes[0].script_ifaces.empty());
    EXPECT_TRUE(p.script.classes[1].script_ifaces.empty());
}

TEST(InterfaceParse, MethodWithBodyIsAnError)
{
    ModParsed p(iface("void F() { }"));
    EXPECT_TRUE(has_error(p, "an interface method cannot have a body"));
    ModParsed q(iface("int F() => 1;"));
    EXPECT_TRUE(has_error(q, "an interface method cannot have a body"));
}

TEST(InterfaceParse, FieldIsAnError)
{
    ModParsed p(iface("int n;"));
    EXPECT_TRUE(has_error(p, "an interface cannot have a field"));
}

TEST(InterfaceParse, ConstructorIsAnError)
{
    ModParsed p(iface("I();"));
    EXPECT_TRUE(has_error(p, "an interface cannot have a constructor"));
}

TEST(InterfaceParse, MemberModifierIsAnError)
{
    for (const char* word : {"static", "async", "virtual", "abstract", "override", "public", "private", "protected"})
    {
        ModParsed p(iface(std::string(word) + " void F();"));
        EXPECT_TRUE(has_error(p, std::string("an interface member cannot have the modifier '") + word + "'")) << word;
    }
}

TEST(InterfaceParse, MemberAttributeIsAnError)
{
    ModParsed p(iface("[value] Foo F();"));
    EXPECT_TRUE(has_error(p, "an interface member cannot have an attribute"));
}

TEST(InterfaceParse, InterfaceModifierIsAnError)
{
    ModParsed p("abstract interface I { }\n");
    EXPECT_TRUE(has_error(p, "an interface cannot have the modifier 'abstract'"));
    ModParsed q("static interface I { }\n");
    EXPECT_TRUE(has_error(q, "an interface cannot have the modifier 'static'"));
    ModParsed r("public interface I { }\n");
    EXPECT_TRUE(has_error(r, "an interface cannot have the modifier 'public'"));
}

TEST(InterfaceParse, InterfaceAttributeIsAnError)
{
    ModParsed p("[value]\ninterface I { }\n");
    EXPECT_TRUE(has_error(p, "an interface cannot have an attribute"));
}

TEST(InterfaceParse, BaseListIsAnError)
{
    ModParsed p("interface A : B { void F(); }\n");
    EXPECT_TRUE(has_error(p, "an interface cannot have a base list in this version"));
    ASSERT_EQ(p.script.interfaces.size(), 1u);
    EXPECT_EQ(p.script.interfaces[0].methods.size(), 1u);
}

TEST(InterfaceParse, DuplicateMethodIsAnError)
{
    ModParsed p(iface("void M();\n    int M(int a);"));
    EXPECT_TRUE(has_error(p, "duplicate method 'M' in interface 'I'"));
}

TEST(InterfaceParse, DuplicateInterfaceIsAnError)
{
    ModParsed p("interface I { }\ninterface I { void F(); }\n");
    EXPECT_TRUE(has_error(p, "duplicate interface 'I'"));
    EXPECT_EQ(p.script.interfaces.size(), 1u);
}

TEST(InterfaceParse, SameNameAsClassIsNotAParseError)
{
    ModParsed p("class I { }\ninterface I { }\n");
    EXPECT_TRUE(p.errors().empty());
    EXPECT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.interfaces.size(), 1u);
}

TEST(InterfaceParse, InterfaceIsAReservedWord)
{
    ModParsed p("class A {\n    int interface;\n}\n");
    EXPECT_TRUE(has_error(p, "'interface' is a reserved word"));
}

TEST(InterfaceParse, ModifierBeforeNameThatContainsInterface)
{
    for (const char* text : {"static int my_interface;\nclass B { int y; }\n", "static int interface_x;\nclass B { int y; }\n",
             "static void my_interface() {}\nclass B { int y; }\n",
             "static int x; // interface\nclass B { int y; }\n",
             "static int x /* interface */;\nclass B { int y; }\n"})
    {
        ModParsed p(text);
        EXPECT_FALSE(has_error(p, "expected 'interface'")) << text;
        EXPECT_TRUE(p.script.interfaces.empty()) << text;
    }
}

TEST(InterfaceParse, ModifierErrorThenNextDeclarationParses)
{
    ModParsed p("static interface I {}\nclass B { int y; }\ninterface J { void F(); }\n");
    EXPECT_TRUE(has_error(p, "an interface cannot have the modifier 'static'"));
    EXPECT_EQ(p.errors().size(), 1u);
    ASSERT_EQ(p.script.interfaces.size(), 2u);
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.classes[0].name, "B");
}

TEST(InterfaceParse, ModifierThenCommentThenInterface)
{
    ModParsed p("static /* c */ interface I {}\n");
    EXPECT_TRUE(has_error(p, "an interface cannot have the modifier 'static'"));
    EXPECT_EQ(p.script.interfaces.size(), 1u);
}

TEST(InterfaceParse, ErrorInsideInterfaceThenClassParses)
{
    ModParsed p("interface I {\n    int n;\n    void F() { }\n}\nclass B { int y; }\n");
    EXPECT_TRUE(has_error(p, "an interface cannot have a field"));
    EXPECT_TRUE(has_error(p, "an interface method cannot have a body"));
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.classes[0].name, "B");
    ModParsed q("interface I {\n    @@ junk;\n    void F();\n}\nclass B { int y; }\n");
    EXPECT_FALSE(q.errors().empty());
    ASSERT_EQ(q.script.classes.size(), 1u);
}
