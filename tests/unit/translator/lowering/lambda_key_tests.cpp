// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the identity key of a lambda (lambda_key.h).
#include "lambda_key.h"
#include "parser/parser.h"
#include "parser/statement.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;

namespace
{

struct KeyRun
{
    Diagnostics diag;
    std::vector<std::string> all; // the key of each lambda, then its alias keys, in source order

    explicit KeyRun(const std::string& text, const std::string& base = "u:Foo::run")
    {
        LineMap map(text);
        ast::BlockStmt b = parse_block({text, 0, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
        EXPECT_TRUE(diag.items.empty()) << text;
        assign_lambda_keys(b, base, &map, diag, &all);
    }

    std::vector<std::string> errors() const
    {
        std::vector<std::string> r;
        for (const Diag& d : diag.items)
            r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
        return r;
    }
};

using V = std::vector<std::string>;

} // namespace

TEST(LambdaKey, ExplicitNameGivesNamePart)
{
    KeyRun r("add([name(\"tick\")] () => { });");
    EXPECT_EQ(r.all, (V{"u:Foo::run/n:tick"}));
    EXPECT_TRUE(r.diag.items.empty());
}

TEST(LambdaKey, LocalDeclarationGivesVariablePart)
{
    KeyRun r("var inc = () => { };");
    EXPECT_EQ(r.all, (V{"u:Foo::run/v:inc"}));
}

TEST(LambdaKey, AssignmentToLocalOrFieldGivesVariablePart)
{
    KeyRun r("var f = 0; f = () => { }; this.cb = () => { }; cb2 = () => { };");
    EXPECT_EQ(r.all, (V{"u:Foo::run/v:f", "u:Foo::run/v:cb", "u:Foo::run/v:cb2"}));
}

TEST(LambdaKey, NameWinsOverVariable)
{
    KeyRun r("var inc = [name(\"x\")] () => { };");
    EXPECT_EQ(r.all, (V{"u:Foo::run/n:x"}));
}

TEST(LambdaKey, OrdinalCountsOnlyUnnamedLambdas)
{
    KeyRun r("a((x) => { }); var v = () => { }; b(() => { }); c([name(\"n\")] () => { }); d(() => { });");
    EXPECT_EQ(r.all, (V{"u:Foo::run/#0", "u:Foo::run/v:v", "u:Foo::run/#1", "u:Foo::run/n:n", "u:Foo::run/#2"}));
}

TEST(LambdaKey, LambdaInsideAnArgumentIsNotAVariable)
{
    KeyRun r("var f = wrap(() => { });");
    EXPECT_EQ(r.all, (V{"u:Foo::run/#0"}));
}

TEST(LambdaKey, NestedLambdaAppendsItsPart)
{
    KeyRun r("var outer = () => { var inner = () => { }; go(() => { }); };");
    EXPECT_EQ(r.all, (V{"u:Foo::run/v:outer", "u:Foo::run/v:outer/v:inner", "u:Foo::run/v:outer/#0"}));
}

TEST(LambdaKey, OrdinalIsCountedForTheWholeFunction)
{
    KeyRun r("a(() => { b(() => { }); }); c(() => { });");
    EXPECT_EQ(r.all, (V{"u:Foo::run/#0", "u:Foo::run/#0/#1", "u:Foo::run/#2"}));
}

TEST(LambdaKey, AliasGivesExtraKey)
{
    KeyRun r("add([name(\"new\"), alias(\"old\"), alias(\"older\")] () => { });");
    EXPECT_EQ(r.all, (V{"u:Foo::run/n:new", "u:Foo::run/n:old", "u:Foo::run/n:older"}));
}

TEST(LambdaKey, AliasOfNestedLambdaUsesTheParentPart)
{
    KeyRun r("var o = () => { add([name(\"b\"), alias(\"a\")] () => { }); };");
    EXPECT_EQ(r.all, (V{"u:Foo::run/v:o", "u:Foo::run/v:o/n:b", "u:Foo::run/v:o/n:a"}));
}

TEST(LambdaKey, SameNameInDifferentParentsIsAllowed)
{
    KeyRun r("var p = () => { add([name(\"x\")] () => { }); }; var q = () => { add([name(\"x\")] () => { }); };");
    EXPECT_TRUE(r.diag.items.empty());
}

TEST(LambdaKey, ErrorEqualNames)
{
    KeyRun r("add([name(\"x\")] () => { });\nadd([name(\"x\")] () => { });");
    ASSERT_EQ(r.errors().size(), 1u);
    EXPECT_EQ(r.errors()[0], "2:6: two lambdas of this function have the name 'x'");
}

TEST(LambdaKey, ErrorEqualVariables)
{
    KeyRun r("var f = () => { };\nvar g = 0;\ng = () => { };\nf = () => { };");
    ASSERT_EQ(r.errors().size(), 1u);
    EXPECT_EQ(r.errors()[0],
        "4:5: two lambdas of this function are assigned to 'f'. Add [name(\"...\")] to each lambda");
}

TEST(LambdaKey, ErrorAliasWithoutName)
{
    KeyRun r("add([alias(\"old\")] () => { });");
    ASSERT_EQ(r.errors().size(), 1u);
    EXPECT_NE(r.errors()[0].find("[alias(\"old\")] needs [name(\"...\")]"), std::string::npos) << r.errors()[0];
}

TEST(LambdaKey, ErrorAliasEqualToAnotherKey)
{
    KeyRun r("add([name(\"a\")] () => { });\nadd([name(\"b\"), alias(\"a\")] () => { });");
    ASSERT_EQ(r.errors().size(), 1u);
    EXPECT_EQ(r.errors()[0], "2:6: the alias 'a' is already the key of another lambda of this function");
}
