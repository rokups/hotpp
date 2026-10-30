// Tests for the parser of the module-level declarations: struct, enum and module variables.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace sppc;
using namespace script_test;

TEST(ModuleDeclParse, StructIsAValueClass)
{
    StubParsed p("struct Point { float x = 0.0; float y; }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.classes[0].name, "Point");
    EXPECT_TRUE(p.script.classes[0].is_value);
    EXPECT_EQ(p.script.classes[0].fields.size(), 2u);
}

TEST(ModuleDeclParse, ClassIsNotAValueClass)
{
    StubParsed p("class A { int n; }\nstruct B { int n; }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes.size(), 2u);
    EXPECT_FALSE(p.script.classes[0].is_value);
    EXPECT_TRUE(p.script.classes[1].is_value);
}

TEST(ModuleDeclParse, EnumWithValuesAndTrailingComma)
{
    StubParsed p("enum Color : byte { Red, Green = 5, Blue, }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.enums.size(), 1u);
    const ast::EnumDef& e = p.script.enums[0];
    EXPECT_EQ(e.name, "Color");
    EXPECT_FALSE(e.underlying.empty());
    ASSERT_EQ(e.items.size(), 3u);
    EXPECT_EQ(e.items[0].name, "Red");
    EXPECT_FALSE(e.items[0].has_value);
    EXPECT_TRUE(e.items[1].has_value);
    EXPECT_NE(e.items[1].value, nullptr);
    EXPECT_EQ(e.items[2].name, "Blue");
}

TEST(ModuleDeclParse, EnumWithoutUnderlyingType)
{
    StubParsed p("enum Mode { Idle, Run }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.enums.size(), 1u);
    EXPECT_TRUE(p.script.enums[0].underlying.empty());
    EXPECT_EQ(p.script.enums[0].items.size(), 2u);
}

TEST(ModuleDeclParse, ModuleVariables)
{
    StubParsed p("int counter = 0;\n[alias(\"old\")] float speed = 1.5;\nPoint last;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_TRUE(p.script.classes.empty());
    ASSERT_EQ(p.script.vars.size(), 3u);
    EXPECT_EQ(p.script.vars[0].name, "counter");
    EXPECT_NE(p.script.vars[0].init, nullptr);
    EXPECT_EQ(p.script.vars[1].name, "speed");
    EXPECT_EQ(p.script.vars[1].alias, "old");
    EXPECT_EQ(p.script.vars[2].name, "last");
    EXPECT_EQ(p.script.vars[2].init, nullptr);
}

TEST(ModuleDeclParse, DeclarationsMixWithClasses)
{
    StubParsed p("int a = 1;\nclass C { int n; }\nenum E { X }\nint b = 2;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.enums.size(), 1u);
    EXPECT_EQ(p.script.vars.size(), 2u);
}

TEST(ModuleDeclParse, ErrorEnumWithoutName)
{
    StubParsed p("enum { A }\n");
    ASSERT_GE(p.diag.items.size(), 1u);
    EXPECT_NE(p.diag.format("t").find("expected an enum name"), std::string::npos) << p.diag.format("t");
}

TEST(ModuleDeclParse, ErrorEnumeratorMissing)
{
    StubParsed p("enum E { A, 5 }\n");
    ASSERT_GE(p.diag.items.size(), 1u);
    EXPECT_NE(p.diag.format("t").find("expected an enumerator name"), std::string::npos) << p.diag.format("t");
}

TEST(ModuleDeclParse, ErrorAttributeOtherThanAliasOnModuleVariable)
{
    StubParsed p("[value] int n = 0;\n");
    ASSERT_GE(p.diag.items.size(), 1u);
    EXPECT_NE(p.diag.format("t").find("is not allowed on a module variable"), std::string::npos) << p.diag.format("t");
}

TEST(ModuleDeclParse, ErrorMethodAtFileLevel)
{
    StubParsed p("int F() { return 1; }\n");
    ASSERT_GE(p.diag.items.size(), 1u);
}
