// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the error text of the removed and the wrong syntax in a class body.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

TEST(SyntaxErrorDiagnostics, JunkInsteadOfAMemberIsAnError)
{
    EXPECT_NE(diag_of("class A : B {\n    123 f() { }\n}\n")
                  .find("t.hot:2:5: error: expected a member: 'Type name;' or 'Ret name(...)', "
                        "found '123'"),
        std::string::npos);
}

TEST(SyntaxErrorDiagnostics, MethodNeedsAReturnType)
{
    EXPECT_NE(diag_of("class A : B {\n    f() { }\n}\n")
                  .find("t.hot:2:5: error: a method needs a return type; write 'Ret name(...)'"),
        std::string::npos);
}

TEST(SyntaxErrorDiagnostics, UnknownAttribute)
{
    EXPECT_EQ(diag_of("class A : B {\n    [foo(\"x\")] int n;\n}\n"), "t.hot:2:6: error: unknown attribute 'foo'\n");
}

TEST(SyntaxErrorDiagnostics, AliasNeedsAString)
{
    const std::string d = diag_of("class A : B {\n    [alias(x)] int n;\n}\n");
    EXPECT_EQ(d.rfind("t.hot:2:12: error: expected a string in [alias(...)]", 0), 0u) << d;
}

TEST(SyntaxErrorDiagnostics, AliasOnAMethod)
{
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"x\")] void f() { }\n}\n"),
        "t.hot:2:5: error: [alias] applies to fields only\n");
}

TEST(SyntaxErrorDiagnostics, TwoAttributesInAList)
{
    // The second attribute is checked like the first one.
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), foo(\"b\")] int n;\n}\n"),
        "t.hot:2:18: error: unknown attribute 'foo'\n");
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), alias(\"b\")] int n;\n}\n"),
        "t.hot:2:18: error: duplicate attribute 'alias'\n");
}
