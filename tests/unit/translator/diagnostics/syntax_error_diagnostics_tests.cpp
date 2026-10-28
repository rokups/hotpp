// Tests for the error text of the removed and the wrong syntax in a class body.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

TEST(SyntaxErrorDiagnostics, OverrideBeforeFunctionGivesOneError)
{
    EXPECT_EQ(diag_of("class A : B {\n    override function f() { }\n    void g() { }\n}\n"),
              "t.spp:2:14: error: 'function' is removed; write 'Ret name(...)'\n");
}

TEST(SyntaxErrorDiagnostics, JunkInsteadOfAMemberIsAnError)
{
    EXPECT_NE(diag_of("class A : B {\n    123 f() { }\n}\n")
                  .find("t.spp:2:5: error: expected a member: 'Type name;' or 'Ret name(...)', "
                        "found '123'"),
              std::string::npos);
}

TEST(SyntaxErrorDiagnostics, FunctionKeywordIsRemoved)
{
    EXPECT_EQ(diag_of("class A : B {\n    function f() { }\n}\n"),
              "t.spp:2:5: error: 'function' is removed; write 'Ret name(...)'\n");
}

TEST(SyntaxErrorDiagnostics, OldFieldFormIsAnError)
{
    EXPECT_EQ(diag_of("class A : B {\n    n: int;\n    int m;\n}\n"),
              "t.spp:2:5: error: field syntax changed; write 'Type name;', found 'n'\n");
}

TEST(SyntaxErrorDiagnostics, ArrowReturnTypeIsRemoved)
{
    EXPECT_EQ(diag_of("class A : B {\n    int f() -> int { return 1; }\n}\n"),
              "t.spp:2:13: error: '->' return type is removed; write the type before the "
              "name\n");
}

TEST(SyntaxErrorDiagnostics, MethodNeedsAReturnType)
{
    EXPECT_NE(diag_of("class A : B {\n    f() { }\n}\n")
                  .find("t.spp:2:5: error: a method needs a return type; write 'Ret name(...)'"),
              std::string::npos);
}

TEST(SyntaxErrorDiagnostics, UnknownAttribute)
{
    EXPECT_EQ(diag_of("class A : B {\n    [foo(\"x\")] int n;\n}\n"),
              "t.spp:2:6: error: unknown attribute 'foo'\n");
}

TEST(SyntaxErrorDiagnostics, AliasNeedsAString)
{
    const std::string d = diag_of("class A : B {\n    [alias(x)] int n;\n}\n");
    EXPECT_EQ(d.rfind("t.spp:2:12: error: expected a string in [alias(...)]", 0), 0u) << d;
}

TEST(SyntaxErrorDiagnostics, AliasOnAMethod)
{
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"x\")] void f() { }\n}\n"),
              "t.spp:2:5: error: [alias] applies to fields only\n");
}

TEST(SyntaxErrorDiagnostics, OldWasSyntaxHasAHint)
{
    EXPECT_EQ(diag_of("class A : B {\n    @was(\"x\") int n;\n}\n"),
              "t.spp:2:5: error: use [alias(...)] instead of @was(...)\n");
}

TEST(SyntaxErrorDiagnostics, TwoAttributesInAList)
{
    // The second attribute is checked like the first one.
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), foo(\"b\")] int n;\n}\n"),
              "t.spp:2:18: error: unknown attribute 'foo'\n");
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), alias(\"b\")] int n;\n}\n"),
              "t.spp:2:18: error: duplicate attribute 'alias'\n");
}
