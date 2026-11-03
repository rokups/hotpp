// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the removed "[value]" attribute. The parser reports each use as an error.
#include "emit.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/script_parse_test_support.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;

namespace
{
const std::string kMsg = "[value] is removed; write the type without [value]";

void expect_removed(const std::string& src)
{
    script_test::StubParsed p(src);
    ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
    EXPECT_EQ(p.diag.items[0].msg, kMsg);
}
} // namespace

TEST(ValueAttributeParse, FieldIsAnError)
{
    expect_removed("class A {\n    [value]\n    Pt a;\n}\n");
    expect_removed("class A {\n    [value, alias(\"old\")]\n    Pt c;\n}\n");
}

TEST(ValueAttributeParse, ParameterIsAnError)
{
    expect_removed("class A {\n    void F([value] Pt a) { }\n}\n");
    expect_removed("class A {\n    A([value] Pt a) { }\n}\n");
}

TEST(ValueAttributeParse, MethodReturnTypeIsAnError)
{
    expect_removed("class A {\n    [value]\n    Pt F() { }\n}\n");
}

TEST(ValueAttributeParse, ModuleVariableIsAnError)
{
    expect_removed("[value] Pt g;\n");
}

TEST(ValueAttributeParse, ClassDefinitionIsAnError)
{
    expect_removed("[value] class V { int a; }\n");
}

TEST(ValueAttributeParse, LocalIsAnError)
{
    using stmt_test::errors_of;
    using stmt_test::Parsed;
    EXPECT_EQ(errors_of(Parsed("[value] Pt a = s;")), (std::vector<std::string>{"1:2: " + kMsg}));
    EXPECT_EQ(errors_of(Parsed("[weak, value] Pt a;")), (std::vector<std::string>{"1:8: " + kMsg}));
}

TEST(ValueAttributeParse, NoFlagIsSetByTheParser)
{
    script_test::StubParsed p("class A {\n    Pt a;\n    Pt F(Pt b) { Pt c = b; return c; }\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    const ast::ClassDef& c = p.script.classes[0];
    EXPECT_FALSE(c.fields[0].value);
    EXPECT_FALSE(c.methods[0].ret_value);
    EXPECT_FALSE(c.methods[0].params[0].value);
}
