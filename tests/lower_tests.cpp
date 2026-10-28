// Tests for the lowering pass (src/translator/lower.h).
// Parse the text, lower it, print it and compare the C++ text.
// The Golden tests in translator_tests.cpp check the lowering of the example scripts.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"

namespace {

using namespace spc;

// The class "Player : Actor" with these members.
//   fields: health float, fired int, n int, target Actor*
//   fns:    heal
struct Fixture {
    ast::ClassDef cd;
    RCtx cx;
    Diagnostics diag;

    Fixture()
    {
        cd.name = "Player";
        cd.base = "Actor";
        auto field = [&](const char* name, const char* type) {
            ast::Field f;
            f.name = name;
            f.type = type;
            cd.fields.push_back(std::move(f));
        };
        field("health", "float");
        field("fired", "int");
        field("n", "int");
        field("target", "Actor*");
        ast::Method heal;
        heal.name = "heal";
        cd.methods.push_back(std::move(heal));
        cx = make_class_ctx(cd);
        cx.where = "Player::run";
    }

    // The errors as "line:col: text".
    std::vector<std::string> errors() const
    {
        std::vector<std::string> r;
        for (const Diag& d : diag.items)
            r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
        return r;
    }

    // Lowers the text as the statements of a body and prints them, separated by one space.
    std::string body(const std::string& text)
    {
        LineMap map(text);
        cx.map = &map;
        ast::BlockStmt b = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
        lower_block(b, cx, diag);
        cx.map = nullptr;
        std::string r;
        for (const ast::StmtP& s : b.stmts) r += (r.empty() ? "" : " ") + ast::print(*s);
        return r;
    }

    std::string expr(const std::string& text)
    {
        LineMap map(text);
        cx.map = &map;
        ast::ExprP e = parse_expression({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
        EXPECT_TRUE(e) << text;
        if (!e) return "";
        lower_expr(e, cx, diag);
        cx.map = nullptr;
        return ast::print(*e);
    }

    void param(const char* name, bool ptr)
    {
        cx.locals.insert(name);
        if (ptr) cx.ptr_names.insert(name);
    }
};

TEST(Lower, FieldAccess)
{
    Fixture f;
    f.param("x", false);
    EXPECT_EQ(f.body("this.fired = this.fired + 1;"), "st->fired = st->fired + 1;");
    EXPECT_EQ(f.body("this.n = x;"), "st->n = x;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, PointerFieldGetsArrow)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.target.Name.c_str()"), "st->target->Name.c_str()");
    EXPECT_EQ(f.expr("this.target"), "st->target");
}

TEST(Lower, PointerParameterGetsArrow)
{
    Fixture f;
    f.param("q", true);
    f.param("v", false);
    EXPECT_EQ(f.body("q.foo();"), "q->foo();");
    EXPECT_EQ(f.body("v.foo();"), "v.foo();");
}

TEST(Lower, StaticMember)
{
    Fixture f;
    EXPECT_EQ(f.expr("Math.Max(this.health - amount, 0.0)"), "Math::Max(st->health - amount, 0.0f)");
    EXPECT_EQ(f.expr("std::printf(\"a\")"), "std::printf(\"a\")");
    EXPECT_EQ(f.expr("A::B.c"), "A::B.c");
}

TEST(Lower, EngineMemberAndThis)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.Name.c_str()"), "self->Name.c_str()");
    EXPECT_EQ(f.expr("this.Move(1)"), "self->Move(1)");
    EXPECT_EQ(f.expr("this"), "self");
    EXPECT_EQ(f.expr("f(this)"), "f(self)");
}

TEST(Lower, Super)
{
    Fixture f;
    EXPECT_EQ(f.expr("super.Tick(dt)"), "self->Actor::Tick(dt)");
}

TEST(Lower, ScriptFnCall)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.heal(1.0)"), "Player__heal(self, 1.0f)");
    EXPECT_EQ(f.expr("this.heal()"), "Player__heal(self)");
    EXPECT_EQ(f.expr("heal(1.0)"), "Player__heal(self, 1.0f)");
    EXPECT_EQ(f.expr("heal()"), "Player__heal(self)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, LocalHidesScriptFn)
{
    Fixture f;
    f.param("heal", false);
    EXPECT_EQ(f.expr("heal(1)"), "heal(1)");
}

TEST(Lower, Numbers)
{
    Fixture f;
    EXPECT_EQ(f.expr("1.5"), "1.5f");
    EXPECT_EQ(f.expr("1.5d"), "1.5");
    EXPECT_EQ(f.expr("1.5f"), "1.5f");
    EXPECT_EQ(f.expr("2e3"), "2e3f");
    EXPECT_EQ(f.expr("3L"), "int64_t{3}");
    EXPECT_EQ(f.expr("0x10"), "0x10");
    EXPECT_EQ(f.expr("7"), "7");
    EXPECT_EQ(rewrite_number("0.5"), "0.5f");
}

TEST(Lower, NullAndDeclarations)
{
    Fixture f;
    EXPECT_EQ(f.body("let p = null; const q: Actor* = null; let k: int;"),
              "auto p = nullptr; const Actor* q = nullptr; int k;");
}

TEST(Lower, ForOf)
{
    Fixture f;
    EXPECT_EQ(f.body("for (let x of xs) { this.n = x; }"), "for (auto x : xs) { st->n = x; }");
    EXPECT_EQ(f.body("for (const y of ys) { }"), "for (const auto y : ys) {}");
}

TEST(Lower, ForAndControl)
{
    Fixture f;
    EXPECT_EQ(f.body("for (let i = 0; i < this.n; i++) { if (i > 2.5) { break; } }"),
              "for (auto i = 0; i < st->n; i++) { if (i > 2.5f) { break; } }");
}

TEST(Lower, Lambda)
{
    Fixture f;
    EXPECT_EQ(f.body("let f = (x: float, q: int*) => { q.foo(); this.n = x; };"),
              "auto f = script::make_closure(GEN, \"Player::run lambda\", [=](float x, int* q) "
              "{ [[maybe_unused]] auto* st = state_of(self); q->foo(); st->n = x; });");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, LambdaNoParameters)
{
    Fixture f;
    EXPECT_EQ(f.expr("() => { this.fired = this.fired + 1; }"),
              "script::make_closure(GEN, \"Player::run lambda\", [=]() "
              "{ [[maybe_unused]] auto* st = state_of(self); st->fired = st->fired + 1; })");
}

// ---- scope. The old rewriter has one flat set for the whole body.

TEST(Lower, LocalDoesNotLeakIntoEarlierText)
{
    Fixture f;
    // The use is before the declaration, so the name is not yet a local.
    EXPECT_EQ(f.body("Foo.bar(); let Foo = 1;"), "Foo::bar(); auto Foo = 1;");
}

TEST(Lower, LocalLeaksOutOfABlock)
{
    Fixture f;
    // The old rewriter has no block scope: Foo stays a local after the block.
    EXPECT_EQ(f.body("{ let Foo = 1; } Foo.bar();"), "{ auto Foo = 1; } Foo.bar();");
}

TEST(Lower, ScriptFnHiddenAfterBlock)
{
    Fixture f;
    EXPECT_EQ(f.body("{ let heal = 1; } heal(2);"), "{ auto heal = 1; } heal(2);");
}

TEST(Lower, LambdaParameterLeaks)
{
    Fixture f;
    EXPECT_EQ(f.body("let g = (q: int*) => { }; q.x;"),
              "auto g = script::make_closure(GEN, \"Player::run lambda\", [=](int* q) "
              "{ [[maybe_unused]] auto* st = state_of(self); }); q->x;");
}

TEST(Lower, ForOfVariableIsLocal)
{
    Fixture f;
    EXPECT_EQ(f.body("for (let Foo of xs) { Foo.a(); }"), "for (auto Foo : xs) { Foo.a(); }");
}

TEST(Lower, LetDoesNotMakeAPointerName)
{
    Fixture f;
    // The old rewriter adds a let to the locals only. So "p.x" keeps the dot.
    EXPECT_EQ(f.body("let p: Actor* = null; p.x;"), "Actor* p = nullptr; p.x;");
}

// ---- template arguments

TEST(Lower, TemplateArgumentsAreLowered)
{
    Fixture f;
    EXPECT_EQ(f.expr("x<this.health>(c)"), "x<st->health>(c)");
    EXPECT_EQ(f.expr("get<Color.Red>(t)"), "get<Color::Red>(t)");
    EXPECT_EQ(f.expr("f<1.0>(c)"), "f<1.0f>(c)");
    EXPECT_EQ(f.expr("f<this.n, 2.5>(c)"), "f<st->n, 2.5f>(c)");
    EXPECT_EQ(f.body("let b2 = x<this.health>(c);"), "auto b2 = x<st->health>(c);");
}

TEST(Lower, TypeTemplateArgumentsKeepTheirText)
{
    Fixture f;
    EXPECT_EQ(f.expr("static_cast<int>(x)"), "static_cast<int>(x)");
    EXPECT_EQ(f.expr("std::vector<std::vector<int>>{}"), "std::vector<std::vector<int>>{}");
    EXPECT_EQ(f.expr("f<b>(c)"), "f<b>(c)");
    EXPECT_EQ(f.expr("f<int*>(c)"), "f<int*>(c)");
    EXPECT_EQ(f.expr("f<const char*>(c)"), "f<const char*>(c)");
    EXPECT_EQ(f.expr("std::map<int,float>{}"), "std::map<int,float>{}");
    EXPECT_EQ(f.expr("f<std::vector<int>>(c)"), "f<std::vector<int>>(c)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// ---- positions

TEST(Lower, CaseLabelsOnSeparateLinesKeepTheirCase)
{
    Fixture f;
    const std::string text = " switch (k) {\ncase 1:\ncase 2: default:\nx = 1;\n}";
    LineMap map(text);
    f.cx.map = &map;
    ast::BlockStmt b = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, f.diag);
    lower_block(b, f.cx, f.diag);
    ast::Emitter e(map, "f.script");
    ast::emit_stmts(b.stmts, e, 1);
    e.finish();
    EXPECT_EQ(f.diag.items.size(), 0u);
    EXPECT_EQ(e.text(),
              "#line 1 \"f.script\"\n    switch (k) {\n"
              "#line 2 \"f.script\"\n        case 1:\n"
              "#line 3 \"f.script\"\n        case 2: default:\n"
              "#line 4 \"f.script\"\n        x = 1;\n"
              "#line 5 \"f.script\"\n    }\n");
}

// ---- errors

TEST(Lower, ErrorLambdaInFieldInitializer)
{
    Fixture f;
    f.cx.allow_this = false;
    f.expr("1 +\n  (x: int) => { }");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:3: a lambda is not allowed in a field initializer");
}

TEST(Lower, ErrorThisInFieldInitializer)
{
    Fixture f;
    f.cx.allow_this = false;
    f.expr("1 + this.n");
    f.expr("this");
    ASSERT_EQ(f.errors().size(), 2u);
    EXPECT_EQ(f.errors()[0], "1:5: 'this' is not allowed in a field initializer");
    EXPECT_EQ(f.errors()[1], "1:1: 'this' is not allowed in a field initializer");
}

TEST(Lower, ErrorScriptFnMustBeCalled)
{
    Fixture f;
    EXPECT_EQ(f.body("let a = 1;\nlet b = this.heal;"), "auto a = 1; auto b = Player__heal;");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:9: script fn 'heal' must be called");
}

TEST(Lower, ErrorBadLambdaParameter)
{
    // The parser rejects "(x) =>", so the test builds the node by hand.
    Fixture f;
    LineMap map("\n  (x) => { }");
    f.cx.map = &map;
    ast::LambdaExpr l;
    l.pos = 3;
    l.params.push_back({"x", ""});
    ast::ExprP e = ast::make_expr(std::move(l));
    lower_expr(e, f.cx, f.diag);
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:3: a lambda parameter needs the form 'name: type'");
}

TEST(Lower, ErrorWithoutMapHasNoLine)
{
    Fixture f;
    f.cx.allow_this = false;
    ast::ExprP e = ast::make_expr(ast::ThisExpr{5});
    lower_expr(e, f.cx, f.diag);
    ASSERT_EQ(f.diag.items.size(), 1u);
    EXPECT_EQ(f.diag.items[0].line, 0);
}

TEST(Lower, ErrorsDoNotStopThePass)
{
    Fixture f;
    f.cx.allow_this = false;
    EXPECT_EQ(f.expr("f(this, 1.5)"), "f(this, 1.5f)");
    EXPECT_EQ(f.errors().size(), 1u);
}

}  // namespace
