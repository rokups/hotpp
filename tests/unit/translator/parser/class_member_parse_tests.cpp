// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the fields and the methods of a class, and of the [alias] attribute.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace script_test;

TEST(ClassMemberParse, TypeFirstMembers)
{
    Diagnostics sd;
    EXPECT_EQ(members_of("class A : B {\n"
                         "    std.string s;\n"
                         "    float hp = 10.0;\n"
                         "    [alias(\"old\")] int n = 0;\n"
                         "    std.vector<std.string> v;\n"
                         "    void tick(float dt) { }\n"
                         "    int* pick(int* p) { return p; }\n"
                         "    std.string name() { return this.s; }\n"
                         "}\n",
                  sd),
        (std::vector<std::string>{"field \x01std::string|s|-|", "field float|hp|init|", "field int32_t|n|init|old",
            "field \x01std::vector<\x01std::string>|v|-|", "method |tick", "method int32_t*|pick", "method \x01std::string|name"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(ClassMemberParse, FieldAlias)
{
    {
        StubParsed p("class A : B { [alias(\"x\")] int y; }\n");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.script.classes.size(), 1u);
        ASSERT_EQ(p.script.classes[0].fields.size(), 1u);
        EXPECT_EQ(p.script.classes[0].fields[0].alias, "x");
    }
    {
        StubParsed p("class A : B { [alias] int y; }\n");
        EXPECT_GE(p.diag.items.size(), 1u);
    }
    {
        // Another name on a field is a script attribute tag. The translation checks its declaration.
        StubParsed p("class A : B { [native] int y; }\n");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.script.classes[0].fields[0].attributes.size(), 1u);
        EXPECT_EQ(p.script.classes[0].fields[0].attributes[0].name, "native");
    }
}

// The base of a class can be a template instance. The dots of the arguments become "::".
TEST(ClassMemberParse, TemplateInstanceBase)
{
    StubParsed p("class A : Tmpl<int>, ns.Other<std.string, Map<int, float>> { }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.classes[0].base, "Tmpl<int32_t>");
    ASSERT_EQ(p.script.classes[0].interfaces.size(), 1u);
    EXPECT_EQ(p.script.classes[0].interfaces[0], "ns::Other<\x01std::string, Map<int32_t, float>>");
}

TEST(ClassMemberParse, TemplateInstanceBaseNeedsClosingBracket)
{
    StubParsed p("class A : Tmpl<int { }\n");
    EXPECT_GT(p.diag.items.size(), 0u);
}
