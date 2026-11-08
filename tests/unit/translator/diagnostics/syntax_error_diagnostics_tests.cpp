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
    EXPECT_EQ(diag_of("class A : B {\n    [foo(\"x\")] int n;\n}\n"), "t.hot:2:6: error: unknown attribute 'foo' on field 'A.n': declare it as '[attribute] class foo;'\n");
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
        "t.hot:2:18: error: unknown attribute 'foo' on field 'A.n': declare it as '[attribute] class foo;'\n");
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), alias(\"b\")] int n;\n}\n"),
        "t.hot:2:18: error: duplicate attribute 'alias'\n");
}

// Hot language features (TODO item 8). A form that the script does not have gives one clear error.
namespace
{
std::string body_diag(const std::string& body)
{
    return diag_of("class A {\n    int n;\n    int F(int v) {\n        " + body + "\n        return 0;\n    }\n}\n");
}
} // namespace

TEST(SyntaxErrorDiagnostics, ConstructorInitializerList)
{
    const std::string d = diag_of("class A {\n    int x;\n    A(int v) : x(v) { }\n}\n");
    EXPECT_NE(d.find("t.hot:3:16: error: expected 'base(...)' after ':' of a constructor; a field initializer list is "
                     "not supported: set the field in the body (this.x = v;)"),
        std::string::npos)
        << d;
}

TEST(SyntaxErrorDiagnostics, LocalFunction)
{
    const char* msg = "error: a local function is not supported; use a lambda (var f = (int x) => x * 2;) or a method";
    EXPECT_NE(body_diag("int Sq(int k) { return k * k; }").find(std::string("t.hot:4:9: ") + msg), std::string::npos);
    EXPECT_NE(body_diag("void G() { }").find(std::string("t.hot:4:9: ") + msg), std::string::npos);
    EXPECT_NE(body_diag("std.vector<int> G(int k) { return {}; }").find(std::string("t.hot:4:9: ") + msg),
        std::string::npos);
}

TEST(SyntaxErrorDiagnostics, SwitchExpression)
{
    const std::string d = body_diag("var r = v switch { 1 => 10, _ => 0 };");
    EXPECT_NE(d.find("t.hot:4:19: error: a switch expression is not supported; use a switch statement or '?:'"),
        std::string::npos)
        << d;
}

TEST(SyntaxErrorDiagnostics, TupleTypeAndTupleExpression)
{
    const std::string t = diag_of("class A {\n    (int, int) F() { return std.make_tuple(1, 2); }\n}\n");
    EXPECT_NE(t.find("t.hot:2:5: error: a tuple type '(A, B)' is not supported; use std.tuple<A, B>"), std::string::npos)
        << t;
    const std::string e = body_diag("var t = (1, 2);");
    EXPECT_NE(e.find("t.hot:4:19: error: a tuple expression '(a, b)' is not supported; use std.make_tuple(a, b)"),
        std::string::npos)
        << e;
    EXPECT_EQ(count_errors(e), 1) << e;
}

TEST(SyntaxErrorDiagnostics, CStyleCastToANonBuiltinType)
{
    const std::string d = body_diag("var s = (std.string)v;");
    EXPECT_NE(d.find("t.hot:4:29: error: a C style cast is only for a builtin type or 'T*'; use static_cast<T>(e)"),
        std::string::npos)
        << d;
    // A call before the parentheses keeps the plain message.
    EXPECT_NE(body_diag("var s = F(v) v;").find("expected end of expression"), std::string::npos);
}

TEST(SyntaxErrorDiagnostics, RawStringLiteral)
{
    const std::string d = body_diag("var s = R\"(a\\b)\";");
    EXPECT_NE(d.find("t.hot:4:18: error: a raw string literal is not supported; use a string with escapes"),
        std::string::npos)
        << d;
}

TEST(SyntaxErrorDiagnostics, SpaceshipOperator)
{
    const std::string d = body_diag("bool b = (v <=> 1) < 0;");
    EXPECT_NE(d.find("t.hot:4:21: error: the operator '<=>' is not supported; use '<', '==' and '>'"), std::string::npos)
        << d;
    EXPECT_EQ(count_errors(d), 1) << d;
}

TEST(SyntaxErrorDiagnostics, Goto)
{
    const std::string d = body_diag("goto end;");
    EXPECT_NE(d.find("t.hot:4:9: error: 'goto' is not supported; use a loop with 'break' or 'continue', or a method "
                     "with 'return'"),
        std::string::npos)
        << d;
}

TEST(SyntaxErrorDiagnostics, Nameof)
{
    const std::string d = body_diag("var s = nameof(v);");
    EXPECT_NE(d.find("t.hot:4:17: error: 'nameof' is not supported; write the name as a string literal"),
        std::string::npos)
        << d;
}

// "get => e;" and "set => e;" are the expression form of an accessor body.
TEST(SyntaxErrorDiagnostics, AccessorExpressionBody)
{
    const hotc::TranslateResult r = translate_text("class A {\n    int x;\n    int Twice { get => this.x * 2; }\n"
                                                   "    int X { get => this.x; set => this.x = value; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("int32_t get_Twice()"), std::string::npos);
    EXPECT_NE(r.cpp.find("void set_X(int32_t value)"), std::string::npos);
    EXPECT_NE(r.cpp.find("* 2;"), std::string::npos);
    const std::string d = diag_of("class A {\n    int x;\n    int X { get => ; }\n}\n");
    EXPECT_NE(d.find("error: expected an expression"), std::string::npos) << d;
}
