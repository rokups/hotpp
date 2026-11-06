// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the module-level declarations: struct, enum and module variables.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace hotc;
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
    StubParsed p("[self] int n = 0;\n");
    ASSERT_GE(p.diag.items.size(), 1u);
    EXPECT_NE(p.diag.format("t").find("is not allowed on a module variable"), std::string::npos) << p.diag.format("t");
}

TEST(ModuleDeclParse, FreeFunctionAtFileLevel)
{
    StubParsed p("int F(int a) { return a; }\nvoid G() => 1;\nint x = 1;\nint convert_x(float old) { return 2; }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.fns.size(), 2u);
    EXPECT_EQ(p.script.fns[0].name, "F");
    EXPECT_EQ(p.script.fns[0].params.size(), 1u);
    EXPECT_EQ(p.script.fns[0].body.stmts.size(), 1u);
    EXPECT_FALSE(p.script.fns[0].is_static);
    EXPECT_EQ(p.script.fns[1].name, "G");
    EXPECT_TRUE(p.script.fns[1].expr_body);
    // A convert hook is not a free function.
    ASSERT_EQ(p.script.var_hooks.size(), 1u);
    EXPECT_EQ(p.script.var_hooks[0].name, "convert_x");
    EXPECT_EQ(p.script.vars.size(), 1u);
}

TEST(ModuleDeclParse, ErrorFreeFunctionWords)
{
    auto errs = [](const char* text) { return StubParsed(text).diag.format("t"); };
    EXPECT_NE(errs("public int F() { return 1; }\n").find("a free function has no access word"), std::string::npos);
    // "private" at file level hides the function from importers (modules). It is no error.
    EXPECT_EQ(errs("private int F() { return 1; }\n"), "");
    EXPECT_NE(errs("async int F() { return 1; }\n").find("a free function cannot be 'async'"), std::string::npos);
    EXPECT_NE(errs("static int F() { return 1; }\n").find("a free function cannot be 'static'"), std::string::npos);
    EXPECT_NE(errs("virtual int F() { return 1; }\n").find("a free function cannot be 'virtual'"), std::string::npos);
    EXPECT_NE(errs("int F();\n").find("needs a body"), std::string::npos);
    EXPECT_NE(errs("int operator+(int a) { return a; }\n").find("an operator needs a class or a struct"),
        std::string::npos);
}
