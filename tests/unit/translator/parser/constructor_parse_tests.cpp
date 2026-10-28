// Tests for the parser of the constructor, the super call and the destructor of a script class.
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "parser/parser.h"
#include "parser/statement.h"

using namespace sppc;

namespace {

struct Parsed {
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::Script script;
    explicit Parsed(std::string body)
        : text("class A : B {\n" + std::move(body) + "\n}\n"), map(text)
    {
        script = parse_script(text, map, diag);
    }
    std::string errors() const { return diag.format("t"); }
    const ast::ClassDef& cls() const { return script.classes.at(0); }
};

}  // namespace

TEST(ConstructorParse, ConstructorWithSuperCall)
{
    Parsed p("    int n = 1;\n    A(int a, std.string b) : super(a, b) { this.n = a; }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.cls().ctors.size(), 1u);
    const ast::Ctor& c = p.cls().ctors[0];
    EXPECT_EQ(c.params.size(), 2u);
    EXPECT_EQ(c.params[0].type, "int32_t");
    EXPECT_EQ(c.params[1].name, "b");
    EXPECT_TRUE(c.has_super);
    EXPECT_EQ(c.super_args.size(), 2u);
    EXPECT_EQ(c.body.stmts.size(), 1u);
    EXPECT_TRUE(p.cls().methods.empty());
}

TEST(ConstructorParse, ConstructorWithoutSuperCall)
{
    Parsed p("    A() { }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.cls().ctors.size(), 1u);
    EXPECT_FALSE(p.cls().ctors[0].has_super);
    EXPECT_TRUE(p.cls().ctors[0].params.empty());
}

TEST(ConstructorParse, SuperCallWithoutArguments)
{
    Parsed p("    A(int a) : super() { }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_TRUE(p.cls().ctors[0].has_super);
    EXPECT_TRUE(p.cls().ctors[0].super_args.empty());
}

TEST(ConstructorParse, Destructor)
{
    Parsed p("    ~A() { this.n = 0; }\n    int n = 1;");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    ASSERT_EQ(p.cls().dtors.size(), 1u);
    EXPECT_EQ(p.cls().dtors[0].name, "~A");
    EXPECT_EQ(p.cls().dtors[0].body.stmts.size(), 1u);
    EXPECT_EQ(p.cls().fields.size(), 1u);
}

TEST(ConstructorParse, MethodsAndConstructorTogether)
{
    Parsed p("    A(int a) : super(a) { }\n    void Tick(float dt) { }\n    ~A() { }");
    ASSERT_TRUE(p.diag.items.empty()) << p.errors();
    EXPECT_EQ(p.cls().ctors.size(), 1u);
    EXPECT_EQ(p.cls().methods.size(), 1u);
    EXPECT_EQ(p.cls().dtors.size(), 1u);
}

TEST(ConstructorParse, Errors)
{
    for (const auto& [text, msg] : std::vector<std::pair<std::string, std::string>>{
             {"C(int a) { }", "a method needs a return type"},
             {"~C() { }", "the name of a destructor is '~A'"},
             {"~A(int a) { }", "a destructor has no parameters"},
             {"~A() : super() { }", "only a constructor can call 'super(...)'"},
             {"void f() : super() { }", "only a constructor can call 'super(...)' after the parameters"},
             {"A() : base() { }", "expected 'super(...)' after ':' of a constructor"},
             {"override A() { }", "'override' is not valid on a constructor"},
             {"virtual ~A() { }", "'virtual' is not valid on a destructor"},
         }) {
        Parsed p("    " + text);
        ASSERT_FALSE(p.diag.items.empty()) << text;
        EXPECT_NE(p.errors().find(msg), std::string::npos) << text << ": " << p.errors();
    }
}
