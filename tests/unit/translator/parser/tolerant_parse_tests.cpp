// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the tolerant parse: a syntax error does not hide the later errors, and the tree
// of the valid parts stays (design 10.1, M10).
#include "emit.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;

namespace
{

struct Parsed
{
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::Script script;
    explicit Parsed(std::string t)
        : text(std::move(t))
        , map(text)
        , script(parse_script(text, map, diag))
    {
    }

    // "line:col: message" for each error, in the order of the result.
    std::vector<std::string> errors() const
    {
        std::vector<std::string> v;
        for (const Diag& d : diag.items)
            v.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
        return v;
    }
    // The printed statements of a method.
    static std::string body(const ast::Method& m)
    {
        std::string s;
        for (const ast::StmtP& st : m.body.stmts)
            s += (s.empty() ? "" : " ") + ast::print(*st);
        return s;
    }
};

} // namespace

TEST(TolerantParse, TwoErrorsInTwoMethodsAreBothReported)
{
    Parsed p(
        "class A : B {\n"
        "    void f() { x = 1; y = ; }\n"
        "    void g() { z = 2 +; w = 3; }\n"
        "    void h() { v = 4; }\n"
        "}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:27: expected an expression", "3:23: expected an expression"}));
    ASSERT_EQ(p.script.classes.size(), 1u);
    const ast::ClassDef& c = p.script.classes[0];
    ASSERT_EQ(c.methods.size(), 3u);
    // The valid statements stay. The broken statement is not in the tree.
    EXPECT_EQ(Parsed::body(c.methods[0]), "x = 1;");
    EXPECT_EQ(Parsed::body(c.methods[1]), "w = 3;");
    EXPECT_EQ(Parsed::body(c.methods[2]), "v = 4;");
}

TEST(TolerantParse, TwoBrokenMembersAreBothReported)
{
    Parsed p(
        "class A : B {\n"
        "    int a;\n"
        "    123 junk;\n"
        "    int b;\n"
        "    456 more;\n"
        "    void f() { }\n"
        "}\n");
    ASSERT_EQ(p.diag.items.size(), 2u);
    EXPECT_EQ(p.diag.items[0].line, 3);
    EXPECT_EQ(p.diag.items[1].line, 5);
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.classes[0].fields.size(), 2u);
    EXPECT_EQ(p.script.classes[0].methods.size(), 1u);
}

TEST(TolerantParse, TrailingDotGivesAnEmptyMemberNode)
{
    Parsed p(
        "class A : B {\n"
        "    void f() {\n"
        "        player.\n"
        "    }\n"
        "}\n");
    // One error at the end of the dot. The missing ";" is a second error at the "}".
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"3:16: expected a member name after '.'", "4:5: expected ';'"}));
    ASSERT_EQ(p.diag.member_holes.size(), 1u);
    // The hole is right after the ".": line 3, col 16 is the end of "        player.".
    EXPECT_EQ(p.map.line(p.diag.member_holes[0]), 3);
    EXPECT_EQ(p.map.col(p.diag.member_holes[0]), 16);
    ASSERT_EQ(p.script.classes.size(), 1u);
    const ast::Method& m = p.script.classes[0].methods.at(0);
    ASSERT_EQ(m.body.stmts.size(), 1u);
    const auto* es = std::get_if<ast::ExprStmt>(&m.body.stmts[0]->v);
    ASSERT_NE(es, nullptr);
    const auto* me = std::get_if<ast::MemberExpr>(&es->expr->v);
    ASSERT_NE(me, nullptr);
    EXPECT_EQ(me->name, "");
    EXPECT_EQ(p.map.col(me->name_pos), 5); // the next token, "}", on line 4
    EXPECT_EQ(ast::print(*me->obj), "player");
}

TEST(TolerantParse, TrailingDotBeforeSemicolonKeepsTheStatements)
{
    Parsed p(
        "class A : B {\n"
        "    void f() { a = 1; x = player.; b = 2; }\n"
        "}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"2:34: expected a member name after '.'"}));
    ASSERT_EQ(p.diag.member_holes.size(), 1u);
    EXPECT_EQ(Parsed::body(p.script.classes.at(0).methods.at(0)), "a = 1; x = player.; b = 2;");
}

TEST(TolerantParse, MissingClassBraceAtTheEndKeepsTheMembers)
{
    Parsed p(
        "class A : B {\n"
        "    int n;\n"
        "    void f() { x = 1; }\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"4:1: expected '}'"}));
    ASSERT_EQ(p.script.classes.size(), 1u);
    const ast::ClassDef& c = p.script.classes[0];
    EXPECT_EQ(c.name, "A");
    ASSERT_EQ(c.fields.size(), 1u);
    EXPECT_EQ(c.fields[0].name, "n");
    ASSERT_EQ(c.methods.size(), 1u);
    EXPECT_EQ(Parsed::body(c.methods[0]), "x = 1;");
}

TEST(TolerantParse, MissingMethodBraceAtTheEndGivesOneError)
{
    Parsed p(
        "class A : B {\n"
        "    int n;\n"
        "    void f() { x = 1;\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"4:1: unexpected end of file"}));
    ASSERT_EQ(p.script.classes.size(), 1u);
    EXPECT_EQ(p.script.classes[0].fields.size(), 1u);
}

TEST(TolerantParse, MissingSemicolonKeepsTheOtherStatements)
{
    Parsed p(
        "class A : B {\n"
        "    void f() {\n"
        "        x = 1;\n"
        "        y = 2\n"
        "    }\n"
        "    void g() { z = 3; }\n"
        "}\n");
    EXPECT_EQ(p.errors(), (std::vector<std::string>{"5:5: expected ';'"}));
    ASSERT_EQ(p.script.classes.at(0).methods.size(), 2u);
    EXPECT_EQ(Parsed::body(p.script.classes[0].methods[0]), "x = 1; y = 2;");
    EXPECT_EQ(Parsed::body(p.script.classes[0].methods[1]), "z = 3;");
}

TEST(TolerantParse, ErrorsComeInSourceOrder)
{
    Parsed p(
        "class A : B {\n"
        "    void f() { y = ; }\n"
        "    123 junk;\n"
        "    void g() { z = ; }\n"
        "}\n");
    ASSERT_GE(p.diag.items.size(), 3u);
    for (size_t i = 1; i < p.diag.items.size(); ++i)
        EXPECT_LE(p.diag.items[i - 1].pos, p.diag.items[i].pos);
}
