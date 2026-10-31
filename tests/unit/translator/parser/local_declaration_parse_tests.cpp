// Tests for the parser of the local declarations: type first, "var", "const" and scoped types.
#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace stmt_test;

// No initializer: the emitter value-initializes. "var" and "const" need an initializer.
TEST(LocalDeclarationParse, DeclarationWithoutInitializer)
{
    EXPECT_EQ(round_trip("int x;"), "int32_t x = int32_t();");
    EXPECT_EQ(round_trip("Vec3 v;"), "Vec3 v = Vec3();");
    EXPECT_EQ(round_trip("Actor* p;"), "Actor* p = std::type_identity_t<Actor*>();");
    EXPECT_EQ(round_trip("for (int i; i < 3; i++) f(i);"), "for (int32_t i = int32_t(); i < 3; i++) f(i);");
    EXPECT_EQ(errors_of(Parsed("var x;")), (std::vector<std::string>{"1:1: var needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("const int X;")), (std::vector<std::string>{"1:1: a const needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("const var X;")), (std::vector<std::string>{"1:1: var needs an initializer"}));
    EXPECT_EQ(errors_of(Parsed("for (var i; i < 3; i++) f(i);")),
        (std::vector<std::string>{"1:6: var needs an initializer"}));
}

TEST(LocalDeclarationParse, Declarations)
{
    EXPECT_EQ(round_trip("var x = 1;"), "auto x = 1;");
    EXPECT_EQ(round_trip("const var x = f(1);"), "const auto x = f(1);");
    EXPECT_EQ(round_trip("float x = 2.0;"), "float x = 2.0;");
    EXPECT_EQ(round_trip("std.vector<int> v = f(1, 2);"), "\x01std::vector<int32_t> v = f(1, 2);");
    EXPECT_EQ(round_trip("const Actor* p = null;"), "const Actor* p = nullptr;");
    EXPECT_EQ(round_trip("std.map<int, std.vector<int>> m;"),
        "\x01std::map<int32_t, \x01std::vector<int32_t>> m = \x01std::map<int32_t, \x01std::vector<int32_t>>();");
    EXPECT_EQ(round_trip("var f = (int a) => { return a; };"), "auto f = [=](int32_t a) { return a; };");
}

// Type-first declarations, "var" and "const". The look-ahead rule is in statement_grammar.cpp.
TEST(LocalDeclarationParse, TypeFirstDeclarations)
{
    EXPECT_EQ(round_trip("int x = 5;"), "int32_t x = 5;");
    EXPECT_EQ(round_trip("var x = 5;"), "auto x = 5;");
    EXPECT_EQ(round_trip("const int X = 5;"), "const int32_t X = 5;");
    EXPECT_EQ(round_trip("const var y = 1;"), "const auto y = 1;");
    EXPECT_EQ(round_trip("Actor* p = null;"), "Actor* p = nullptr;");
    EXPECT_EQ(round_trip("std.string s;"), "\x01std::string s = \x01std::string();");
    EXPECT_EQ(round_trip("std.string s = \"x\";"), "\x01std::string s = \"x\";");
    EXPECT_EQ(round_trip("uint n = 0;"), "uint32_t n = 0;");
    EXPECT_EQ(round_trip("const std.exception& e = f();"), "const \x01std::exception& e = f();");
    EXPECT_EQ(round_trip("int[3] a;"), "\x01std::array<int32_t, 3> a = \x01std::array<int32_t, 3>();");
    EXPECT_EQ(round_trip("/* c */ int // d\n x = 1;"), "int32_t x = 1;");

    // The kind of each statement: a declaration or an expression.
    const auto decl = [](const std::string& text) -> const ast::VarDeclStmt*
    {
        static std::vector<std::unique_ptr<Parsed>> keep;
        keep.push_back(std::make_unique<Parsed>(text));
        const Parsed& p = *keep.back();
        EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
        if (p.block.stmts.size() != 1)
            return nullptr;
        return std::get_if<ast::VarDeclStmt>(&p.block.stmts[0]->v);
    };
    // The C++ rule: "a * b" is a declaration of b.
    const ast::VarDeclStmt* d = decl("a * b = c;");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->name, "b");
    EXPECT_EQ(d->type, "a*");
    d = decl("a * b;");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->name, "b");
    EXPECT_EQ(d->type, "a*");
    // A "<" after a name opens a template argument list.
    d = decl("a < b > c;");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->name, "c");
    EXPECT_EQ(d->type, "a<b>");
    d = decl("std.map<int, std.vector<int>> m = f();");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->type, "\x01std::map<int32_t, \x01std::vector<int32_t>>");
    EXPECT_FALSE(decl("f(x);"));
    EXPECT_FALSE(decl("a = b;"));
    EXPECT_FALSE(decl("a.b = c;"));
    EXPECT_FALSE(decl("x++;"));
    EXPECT_FALSE(decl("*p = 1;"));
    EXPECT_FALSE(decl("a == b;"));
    EXPECT_FALSE(decl("a < b;"));
    EXPECT_FALSE(decl("a + b;"));
    EXPECT_FALSE(decl("x;"));
    EXPECT_FALSE(decl("f<int>(x);"));
    EXPECT_FALSE(decl("a[i] = c;"));
    EXPECT_FALSE(decl("this.n = 1;"));
    EXPECT_FALSE(decl("a *= b;"));

    // A "const" needs "var" or a type.
    EXPECT_EQ(
        errors_of(Parsed("const x = 1;")), (std::vector<std::string>{"1:7: expected 'var' or a type after 'const'"}));

    // A declaration in the init of a for statement.
    EXPECT_EQ(round_trip("for (std.size_t i = 0; i < n; i++) f(i);"), "for (\x01std::size_t i = 0; i < n; i++) f(i);");
    EXPECT_EQ(
        round_trip("for (Actor* p = first; p; p = next(p)) f(p);"), "for (Actor* p = first; p; p = next(p)) f(p);");
    EXPECT_EQ(round_trip("for (const int k = 3; i < k; i++) f(i);"), "for (const int32_t k = 3; i < k; i++) f(i);");
    EXPECT_EQ(round_trip("for (i = 0; i < n; i++) f(i);"), "for (i = 0; i < n; i++) f(i);");
    // foreach with the new declaration forms.
    EXPECT_EQ(round_trip("foreach (const Actor* a in xs) f(a);"), "for (const Actor* a : xs) f(a);");
}

// A "." in the text of a type is a scope. The parser writes "::". A "::" is an error.
TEST(LocalDeclarationParse, TypeScope)
{
    EXPECT_EQ(round_trip("std.string a = x; std.vector<std.string> b;"),
        "\x01std::string a = x; \x01std::vector<\x01std::string> b = \x01std::vector<\x01std::string>();");
    EXPECT_EQ(round_trip("A.B.C* c; std.array<int, 3> d; float e;"),
        "A::B::C* c = std::type_identity_t<A::B::C*>(); "
        "\x01std::array<int32_t, 3> d = \x01std::array<int32_t, 3>(); float e = float();");
    Parsed p("int a;\nstd::string b = x;");
    EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:4: use '.' instead of '::'"}));
    Parsed q("foreach (std::string s in xs) f(s);");
    EXPECT_EQ(errors_of(q), (std::vector<std::string>{"1:13: use '.' instead of '::'"}));
}

TEST(LocalDeclarationParse, CollectionLiteralInitializer)
{
    EXPECT_EQ(round_trip("std.vector<int> xs = [1, 2, 3];"), "\x01std::vector<int32_t> xs = {1, 2, 3};");
    EXPECT_EQ(round_trip("std.map<std.string, int> m = [\"a\": 1, \"b\": 2];"),
        "\x01std::map<\x01std::string, int32_t> m = {{\"a\", 1}, {\"b\", 2}};");
    EXPECT_EQ(round_trip("const std.vector<int> e = [];"), "const \x01std::vector<int32_t> e = {};");
}
