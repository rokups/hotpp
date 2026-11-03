// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the type syntax: the prefix "$" and the suffixes "*", "?", "[]" and "[N]".
// The tree keeps the type as text. The tests check the text and the ambiguity rules.
#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/expression_parse_test_support.h"
#include "support/script_parse_test_support.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;

namespace
{

// The first statement of the text as a local declaration. Null when it is another statement.
const ast::VarDeclStmt* decl_of(const stmt_test::Parsed& p)
{
    if (p.block.stmts.empty())
        return nullptr;
    return std::get_if<ast::VarDeclStmt>(&p.block.stmts[0]->v);
}

// The type text of a local declaration. The text has no errors.
std::string local_type(const std::string& text)
{
    stmt_test::Parsed p(text);
    EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
    const ast::VarDeclStmt* d = decl_of(p);
    EXPECT_NE(d, nullptr) << text;
    return d ? d->type : std::string("<not a declaration>");
}

struct Row
{
    const char* script; // the type as the script writes it
    const char* text; // the type text of the tree
};

const Row kTypes[] = {
    {"$Foo", "$Foo"},
    {"$ Foo", "$Foo"},
    {"$std.string", "$\x01std::string"},
    {"Foo*", "Foo*"},
    {"Foo**", "Foo**"},
    {"int*", "int32_t*"},
    {"int?", "int32_t?"},
    {"Foo?", "Foo?"},
    {"int[]", "int32_t[]"},
    {"Foo[]", "Foo[]"},
    {"int*[]", "int32_t*[]"},
    {"int?[]", "int32_t?[]"},
    {"int[]?", "int32_t[]?"},
    {"$Foo[]", "$Foo[]"},
    {"$Foo*", "$Foo*"},
    {"std.vector<int?>", "\x01std::vector<int32_t?>"},
};

std::string errors_of_script(const std::string& text)
{
    const LineMap map(text);
    Diagnostics diag;
    parse_script(text, map, diag);
    return diag.format("t");
}

} // namespace

TEST(TypeSyntaxParse, LocalTypeText)
{
    for (const Row& r : kTypes)
        EXPECT_EQ(local_type(std::string(r.script) + " x;"), r.text) << r.script;
    EXPECT_EQ(local_type("int[3] x;"), "\x01std::array<int32_t, 3>");
    EXPECT_EQ(local_type("Foo x = null;"), "Foo");
    EXPECT_EQ(local_type("$Foo x = null;"), "$Foo");
    EXPECT_EQ(local_type("int? x = null;"), "int32_t?");
}

TEST(TypeSyntaxParse, FieldParameterAndReturnTypes)
{
    for (const Row& r : kTypes)
    {
        const std::string t = r.script;
        const bool view = t.find("[]") != std::string::npos;
        // A "T[]" field is an error. The golden test "30_array_without_size" keeps that rule.
        if (!view)
        {
            script_test::StubParsed f("class A : B { " + t + " v; }\n");
            ASSERT_EQ(f.diag.items.size(), 0u) << t << "\n" << f.diag.format("t");
            ASSERT_EQ(f.script.classes[0].fields.size(), 1u) << t;
            EXPECT_EQ(f.script.classes[0].fields[0].type, r.text) << t;
        }
        script_test::StubParsed m("class A : B { " + t + " F(" + t + " p, int n) { return p; } }\n");
        ASSERT_EQ(m.diag.items.size(), 0u) << t << "\n" << m.diag.format("t");
        ASSERT_EQ(m.script.classes[0].methods.size(), 1u) << t;
        const ast::Method& me = m.script.classes[0].methods[0];
        EXPECT_EQ(me.ret, r.text) << t;
        ASSERT_EQ(me.params.size(), 2u) << t;
        EXPECT_EQ(me.params[0].type, r.text) << t;
        EXPECT_EQ(me.params[0].name, "p") << t;
    }
}

TEST(TypeSyntaxParse, HandleFieldAndModuleMember)
{
    script_test::StubParsed p("class A : B {\n    $Foo next;\n    $Foo Get() { return this.next; }\n}\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_EQ(p.script.classes[0].fields[0].type, "$Foo");
    EXPECT_EQ(p.script.classes[0].methods[0].ret, "$Foo");
}

TEST(TypeSyntaxParse, ArrayViewOnFieldStaysAnError)
{
    EXPECT_NE(errors_of_script("class A : B { int[] v; }\n").find("T[] is a view; a field needs std.vector<T> or T[N]"), std::string::npos);
}

TEST(TypeSyntaxParse, CastAndNew)
{
    EXPECT_EQ(expr_test::round_trip("($Foo)h"), "($Foo)h");
    EXPECT_EQ(expr_test::round_trip("(Foo*)p"), "(Foo*)p");
    EXPECT_EQ(expr_test::round_trip("(int?)x"), "(int32_t?)x");
    EXPECT_EQ(expr_test::round_trip("(Foo?)x"), "(Foo?)x");
    EXPECT_EQ(expr_test::round_trip("(int)x"), "(int32_t)x");
    EXPECT_EQ(expr_test::round_trip("new $Foo(1, 2)"), "new $Foo(1, 2)");
    EXPECT_EQ(expr_test::round_trip("new Foo(1)"), "new Foo(1)");
    EXPECT_EQ(expr_test::round_trip("new $std.vector<int>()"), "new $\x01std::vector<int32_t>()");
}

TEST(TypeSyntaxParse, DollarNeedsAClassType)
{
    for (const char* t : {"$int", "$float", "$bool", "$ulong", "$int*"})
    {
        stmt_test::Parsed p(std::string(t) + " x;");
        ASSERT_EQ(p.diag.items.size(), 1u) << t << "\n" << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "\"$\" needs a class type") << t;
    }
    EXPECT_EQ(stmt_test::errors_of(stmt_test::Parsed("$int x;")),
        (std::vector<std::string>{"1:1: \"$\" needs a class type"}));
    script_test::StubParsed f("class A : B { $int v; }\n");
    ASSERT_EQ(f.diag.items.size(), 1u) << f.diag.format("t");
    EXPECT_EQ(f.diag.items[0].msg, "\"$\" needs a class type");
    script_test::StubParsed m("class A : B { void F($float p) { } }\n");
    ASSERT_EQ(m.diag.items.size(), 1u) << m.diag.format("t");
    EXPECT_EQ(m.diag.items[0].msg, "\"$\" needs a class type");
    for (const char* text : {"new $int()", "($int)x", "($float*)x"})
    {
        expr_test::Parsed e(text);
        ASSERT_EQ(e.diag.items.size(), 1u) << text << "\n" << e.diag.format("t");
        EXPECT_EQ(e.diag.items[0].msg, "\"$\" needs a class type") << text;
    }
}

TEST(TypeSyntaxParse, QuestionCannotFollowPointerOrHandle)
{
    const std::string msg = "\"?\" cannot follow a pointer or handle; T* and $T are already nullable";
    for (const char* t : {"$Foo?", "Foo*?", "Foo**?", "$Foo*?"})
    {
        stmt_test::Parsed p(std::string(t) + " x;");
        ASSERT_EQ(p.diag.items.size(), 1u) << t << "\n" << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, msg) << t;
    }
    script_test::StubParsed f("class A : B { $Foo? v; }\n");
    ASSERT_EQ(f.diag.items.size(), 1u) << f.diag.format("t");
    EXPECT_EQ(f.diag.items[0].msg, msg);
    script_test::StubParsed m("class A : B { Foo*? F() { } }\n");
    ASSERT_EQ(m.diag.items.size(), 1u) << m.diag.format("t");
    EXPECT_EQ(m.diag.items[0].msg, msg);
    // The check is for the top level of the type only.
    EXPECT_EQ(local_type("std.vector<Foo*> x;"), "\x01std::vector<Foo*>");
    EXPECT_EQ(local_type("Foo*[] x;"), "Foo*[]");
}

TEST(TypeSyntaxParse, RefLocal)
{
    {
        stmt_test::Parsed p("ref r = x;");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        const ast::VarDeclStmt* d = decl_of(p);
        ASSERT_NE(d, nullptr);
        EXPECT_TRUE(d->is_ref);
        EXPECT_FALSE(d->is_const);
        EXPECT_EQ(d->name, "r");
        EXPECT_EQ(d->type, "");
        EXPECT_NE(d->init, nullptr);
    }
    {
        stmt_test::Parsed p("ref Vec3 r = x;");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        const ast::VarDeclStmt* d = decl_of(p);
        ASSERT_NE(d, nullptr);
        EXPECT_TRUE(d->is_ref);
        EXPECT_EQ(d->name, "r");
        EXPECT_EQ(d->type, "Vec3");
    }
    EXPECT_EQ(stmt_test::round_trip("ref r = this.pos;"), "auto& r = this.pos;");
    EXPECT_EQ(stmt_test::round_trip("ref (a, b) = x;"), "auto& [a, b] = x;");
    EXPECT_EQ(stmt_test::round_trip("var v = 1; ref r = v;"), "auto v = 1; auto& r = v;");
    EXPECT_FALSE(stmt_test::Parsed("ref r;").diag.items.empty());
    EXPECT_FALSE(stmt_test::Parsed("ref Vec3 r;").diag.items.empty());
}

TEST(TypeSyntaxParse, VarAmpersandIsRemoved)
{
    for (const char* text : {"var& r = x;", "var &r = x;", "const var& r = x;", "foreach (var& x in c) f(x);",
             "var& (a, b) = x;"})
    {
        stmt_test::Parsed p(text);
        ASSERT_FALSE(p.diag.items.empty()) << text;
        EXPECT_EQ(p.diag.items[0].msg, "\"var&\" is not valid; write ref name = expr") << text;
    }
}

TEST(TypeSyntaxParse, ForeachRef)
{
    {
        stmt_test::Parsed p("foreach (ref x in c) f(x);");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.block.stmts.size(), 1u);
        const auto* f = std::get_if<ast::ForOfStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(f->is_ref);
        EXPECT_EQ(f->name, "x");
        EXPECT_EQ(f->type, "");
    }
    {
        stmt_test::Parsed p("foreach (ref Vec3 x in c) f(x);");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        const auto* f = std::get_if<ast::ForOfStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(f->is_ref);
        EXPECT_EQ(f->type, "Vec3");
    }
    EXPECT_EQ(stmt_test::round_trip("foreach (ref x in xs) f(x);"), "for (auto& x : xs) f(x);");
    EXPECT_EQ(stmt_test::round_trip("foreach (ref (k, v) in m) f(k, v);"), "for (auto& [k, v] : m) f(k, v);");
    {
        stmt_test::Parsed p("foreach (var x in c) f(x);");
        const auto* f = std::get_if<ast::ForOfStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(f->is_ref);
    }
    {
        stmt_test::Parsed p("foreach ($Foo x in c) f(x);");
        ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        const auto* f = std::get_if<ast::ForOfStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->type, "$Foo");
    }
}

// A declaration and an expression start alike. The look-ahead decides.
TEST(TypeSyntaxParse, StatementStartIsADeclaration)
{
    for (const char* text : {"Foo? x;", "int? x;", "$Foo x;", "int[] x;", "Foo*[] x;", "Foo? x = null;",
             "$Foo x = null;", "int[] x = f();", "std.vector<int>? x;"})
    {
        stmt_test::Parsed p(text);
        EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
        EXPECT_NE(decl_of(p), nullptr) << text;
    }
}

TEST(TypeSyntaxParse, StatementStartIsAnExpression)
{
    for (const char* text : {"a ? b : c;", "a ? f() : g();", "a ? b = 1 : c;", "a?b:c;", "a?.b();", "a[i] = 1;",
             "x = a ? b : c;", "f(a ? b : c);"})
    {
        stmt_test::Parsed p(text);
        EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
        if (!p.block.stmts.empty())
            EXPECT_EQ(std::get_if<ast::VarDeclStmt>(&p.block.stmts[0]->v), nullptr) << text;
    }
}

TEST(TypeSyntaxParse, ExpressionsKeepTheirMeaning)
{
    EXPECT_EQ(expr_test::round_trip("a ? b : c"), "a ? b : c");
    EXPECT_EQ(expr_test::round_trip("x * y"), "x * y");
    EXPECT_EQ(expr_test::round_trip("a[i]"), "hot::index(a, i)");
    EXPECT_EQ(expr_test::round_trip("a[i] * b[j]"), "hot::index(a, i) * hot::index(b, j)");
    EXPECT_EQ(expr_test::round_trip("a is T"), "a is T");
    EXPECT_EQ(expr_test::round_trip("a is T t"), "a is T t");
    EXPECT_EQ(expr_test::round_trip("(T*)x"), "(T*)x");
    EXPECT_EQ(expr_test::round_trip("(a ? b : c)"), "(a ? b : c)");
    EXPECT_EQ(expr_test::round_trip("(a) ? b : c"), "(a) ? b : c");
    EXPECT_EQ(expr_test::round_trip("(a * b)"), "(a * b)");
    EXPECT_EQ(expr_test::round_trip("f<int>(x)"), "f<int32_t>(x)");
    EXPECT_EQ(expr_test::round_trip("std.vector<std.vector<int>>()"),
        "\x01std::vector<\x01std::vector<int32_t>>()");
    EXPECT_EQ(expr_test::round_trip("a ? (b) : (c)"), "a ? (b) : (c)");
}

// An attribute string with "$T" is a string, not a type.
TEST(TypeSyntaxParse, DollarInAttributeStringIsText)
{
    script_test::StubParsed p("class A : B { [alias(\"$T\")] int y; }\n");
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
}
