// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of "abstract" on a class and on a method.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace script_test;

namespace
{

// A class body with one member. The class is abstract or not.
std::string abstract_class(const std::string& member)
{
    return "abstract class Shape {\n    " + member + "\n}\n";
}

} // namespace

TEST(AbstractParse, AbstractClassWithAbstractMethod)
{
    ModParsed p("abstract class Shape {\n    abstract float Area();\n}\n");
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.classes.size(), 1u);
    const ast::ClassDef& c = p.script.classes[0];
    EXPECT_TRUE(c.is_abstract);
    EXPECT_FALSE(c.is_value);
    EXPECT_EQ(c.name, "Shape");
    ASSERT_EQ(c.methods.size(), 1u);
    EXPECT_TRUE(c.methods[0].is_abstract);
    EXPECT_FALSE(c.methods[0].is_virtual);
    EXPECT_EQ(c.methods[0].name, "Area");
    EXPECT_EQ(c.methods[0].ret, "float");
}

TEST(AbstractParse, ClassIsNotAbstractByDefault)
{
    ModParsed p("class A {\n    virtual void F() { }\n}\n");
    EXPECT_TRUE(p.errors().empty());
    EXPECT_FALSE(p.script.classes[0].is_abstract);
    EXPECT_FALSE(p.method().is_abstract);
}

TEST(AbstractParse, AbstractClassWithBaseAndOtherMembers)
{
    ModParsed p("abstract class Shape : Base {\n    int n;\n    abstract void Draw(int x, float y);\n"
                "    virtual int Id() { return 1; }\n    void Plain() { }\n}\n");
    EXPECT_TRUE(p.errors().empty());
    const ast::ClassDef& c = p.script.classes.at(0);
    EXPECT_TRUE(c.is_abstract);
    EXPECT_EQ(c.base, "Base");
    ASSERT_EQ(c.methods.size(), 3u);
    EXPECT_TRUE(c.methods[0].is_abstract);
    EXPECT_EQ(c.methods[0].params.size(), 2u);
    EXPECT_EQ(c.methods[0].ret, "");
    EXPECT_FALSE(c.methods[1].is_abstract);
    EXPECT_TRUE(c.methods[1].is_virtual);
    EXPECT_FALSE(c.methods[2].is_abstract);
}

TEST(AbstractParse, AbstractClassWithNoAbstractMethod)
{
    ModParsed p("abstract class Shape {\n    int n;\n}\n");
    EXPECT_TRUE(p.errors().empty());
    EXPECT_TRUE(p.script.classes.at(0).is_abstract);
}

TEST(AbstractParse, AccessAsyncAndConstWithAbstract)
{
    {
        ModParsed p(abstract_class("protected abstract int F();"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_abstract);
        EXPECT_EQ(p.method().access, ast::Access::Protected);
    }
    {
        ModParsed p(abstract_class("abstract public int F();"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_abstract);
        EXPECT_EQ(p.method().access, ast::Access::Public);
    }
    {
        ModParsed p(abstract_class("async abstract Task<int> F();"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_abstract);
        EXPECT_TRUE(p.method().is_async);
    }
    {
        ModParsed p(abstract_class("abstract async Task<int> F();"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_abstract);
        EXPECT_TRUE(p.method().is_async);
    }
    {
        ModParsed p(abstract_class("abstract int F() const;"));
        EXPECT_TRUE(p.errors().empty());
        EXPECT_TRUE(p.method().is_abstract);
        EXPECT_TRUE(p.method().is_const);
    }
}

TEST(AbstractParse, AttributesBeforeAbstractClass)
{
    ModParsed p("[attribute] class Tag;\n[Tag] abstract class Shape {\n    abstract void F();\n}\n");
    EXPECT_TRUE(p.errors().empty());
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_TRUE(p.script.classes[0].is_abstract);
    EXPECT_EQ(p.script.classes[0].attributes, (std::vector<std::string>{"Tag"}));
}

TEST(AbstractParse, MethodPositionsAreSet)
{
    ModParsed p(abstract_class("abstract int F();"));
    EXPECT_TRUE(p.errors().empty());
    EXPECT_EQ(p.method().line, 2);
    EXPECT_EQ(p.method().end_line, 2);
    EXPECT_GT(p.method().end_pos, p.method().pos);
}

TEST(AbstractParse, AbstractWithVirtualIsAnError)
{
    {
        ModParsed p(abstract_class("abstract virtual int F();"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{
                                  "2:14: 'abstract' and 'virtual' cannot combine; an abstract method is virtual already"}));
    }
    {
        ModParsed p(abstract_class("virtual abstract int F();"));
        EXPECT_EQ(p.errors(), (std::vector<std::string>{
                                  "2:5: 'abstract' and 'virtual' cannot combine; an abstract method is virtual already"}));
    }
}

TEST(AbstractParse, AbstractWithNewIsAnError)
{
    ModParsed p(abstract_class("abstract new int F();"));
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:14: 'abstract' and 'new' cannot combine"}));
}

TEST(AbstractParse, AbstractWithOverrideIsAnError)
{
    ModParsed p(abstract_class("abstract override int F();"));
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:14: 'abstract' and 'override' cannot combine"}));
}

TEST(AbstractParse, AbstractMethodWithBodyIsAnError)
{
    {
        ModParsed p(abstract_class("abstract int F() { return 1; }"));
        EXPECT_EQ(p.errors(),
            (std::vector<std::string>{"2:5: an abstract method cannot have a body; end the declaration with ';'"}));
    }
    {
        ModParsed p(abstract_class("abstract int F() => 1;"));
        EXPECT_EQ(p.errors().size(), 1u);
    }
}

TEST(AbstractParse, AbstractMethodOutsideAbstractClassIsAnError)
{
    {
        ModParsed p("class A {\n    abstract int F();\n}\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{
                                  "2:14: abstract method 'F' needs an abstract class: write 'abstract class A'"}));
    }
    {
        ModParsed p("struct S {\n    abstract int F();\n}\n");
        EXPECT_EQ(p.errors().size(), 1u);
    }
}

TEST(AbstractParse, AbstractStructIsAnError)
{
    ModParsed p("abstract struct S {\n    int n;\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"1:1: a struct cannot be 'abstract'; only a class can be"}));
}

TEST(AbstractParse, AbstractConstructorIsAnError)
{
    ModParsed p("abstract class A {\n    abstract A() { }\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: a constructor cannot be 'abstract'"}));
}

TEST(AbstractParse, AbstractPropertyIsAnError)
{
    ModParsed p(abstract_class("abstract int P { get; set; }"));
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: a property cannot be 'abstract'; only a method can be"}));
}

TEST(AbstractParse, AbstractFieldIsAnError)
{
    ModParsed p(abstract_class("abstract int n;"));
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: a field cannot be 'abstract'; only a method can be"}));
}

TEST(AbstractParse, AbstractHookIsAnError)
{
    ModParsed p("abstract class A {\n    abstract ~A() { }\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'abstract' is not valid on a destructor"}));
}

TEST(AbstractParse, AbstractAtFileLevelWithoutClassIsAnError)
{
    ModParsed p("abstract int F();\n");
    EXPECT_FALSE(p.errors().empty());
}

TEST(AbstractParse, MethodWithoutBodyAndWithoutAbstractIsAnError)
{
    ModParsed p("class A {\n    int F();\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{
                              "2:12: method 'F' needs a body; write '{ ... }' or mark the method 'abstract'"}));
}

TEST(AbstractParse, ConstructorWithoutBodyIsAnError)
{
    ModParsed p("class A {\n    A();\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:8: a constructor needs a block body"}));
}

TEST(AbstractParse, FileLevelConvertHookWithoutBodyIsAnError)
{
    ModParsed p("int n;\nint convert_n(int old);\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:23: method 'convert_n' needs a body; write '{ ... }'"}));
}

TEST(AbstractParse, AbstractStubIsAnError)
{
    {
        ModParsed p("abstract class X;\n");
        ASSERT_FALSE(p.errors().empty());
        EXPECT_EQ(p.errors()[0], "1:1: a class stub cannot be 'abstract'");
    }
    {
        ModParsed p("abstract class : B;\n");
        ASSERT_FALSE(p.errors().empty());
        EXPECT_EQ(p.errors()[0], "1:1: a class stub cannot be 'abstract'");
    }
}

TEST(AbstractParse, AbstractNeedsClassAtFileLevel)
{
    {
        ModParsed p("abstract Foo x;\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"1:10: expected 'class' after 'abstract'"}));
    }
    {
        ModParsed p("abstract;\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"1:9: expected 'class' after 'abstract'"}));
    }
}

TEST(AbstractParse, DuplicateAbstractIsAnError)
{
    ModParsed p(abstract_class("abstract abstract int F();"));
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:14: duplicate modifier 'abstract'"}));
}

TEST(AbstractParse, AbstractStaysAnIdentifierElsewhere)
{
    {
        ModParsed p("class A {\n    int abstract;\n}\n");
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.field().name, "abstract");
    }
    {
        ModParsed p("class A {\n    void abstract_called() { }\n    int abstractly;\n}\n");
        EXPECT_TRUE(p.errors().empty());
        EXPECT_EQ(p.method().name, "abstract_called");
        EXPECT_FALSE(p.method().is_abstract);
        EXPECT_EQ(p.field().name, "abstractly");
    }
}

TEST(AbstractParse, AttributeBeforeAbstractMethodIsAnError)
{
    ModParsed p(abstract_class("[Tag] abstract int F();"));
    EXPECT_EQ(p.errors().size(), 1u);
}

TEST(AbstractParse, AbstractMethodInStructNamesTheStruct)
{
    ModParsed p("struct S {\n    abstract int F();\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{
                              "2:14: abstract method 'F' needs an abstract struct: write 'abstract class S'"}));
}

TEST(AbstractParse, DestructorWithoutBodyIsAnError)
{
    {
        ModParsed p("abstract class A {\n    abstract ~A();\n}\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:5: 'abstract' is not valid on a destructor",
                                  "2:18: a destructor needs a block body"}));
    }
    {
        ModParsed p("class A {\n    ~A();\n}\n");
        EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:9: a destructor needs a block body"}));
    }
}

TEST(AbstractParse, ConstructorBaseCallWithoutBodyIsAnError)
{
    ModParsed p("class A : B {\n    A() : base();\n}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:17: a constructor needs a block body"}));
}
