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

    void param(const char* name) { cx.locals.insert(name); }
};

TEST(Lower, FieldAccess)
{
    Fixture f;
    f.param("x");
    EXPECT_EQ(f.body("this.fired = this.fired + 1;"), "st->fired = st->fired + 1;");
    EXPECT_EQ(f.body("this.n = x;"), "st->n = x;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, CatchNameIsALocal)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } catch (std.exception& e) { e.what(); } catch (...) { throw; }"),
              "try { throw 1; } catch (std::exception& e) { script::deref(e).what(); } "
              "catch (...) { throw; }");
    EXPECT_EQ(f.body("throw this.health;"), "throw st->health;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, PointerFieldUsesDeref)
{
    Fixture f;
    // The field target has the type Actor*.
    EXPECT_EQ(f.expr("this.target.Name.c_str()"),
              "script::deref(script::deref(st->target).Name).c_str()");
    EXPECT_EQ(f.expr("this.target"), "st->target");
    EXPECT_EQ(f.expr("this.target.m"), "script::deref(st->target).m");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, MemberChainOfAParameter)
{
    Fixture f;
    f.param("q");
    f.param("v");
    EXPECT_EQ(f.body("q.foo();"), "script::deref(q).foo();");
    EXPECT_EQ(f.body("v.begin();"), "script::deref(v).begin();");
    EXPECT_EQ(f.body("v.begin().x;"), "script::deref(script::deref(v).begin()).x;");
    EXPECT_EQ(f.body("q.a.b.c = 1;"),
              "script::deref(script::deref(script::deref(q).a).b).c = 1;");
    EXPECT_EQ(f.body("q.m = 1;"), "script::deref(q).m = 1;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, MemberOfAValue)
{
    Fixture f;
    f.param("a");
    EXPECT_EQ(f.expr("make().x"), "script::deref(make()).x");
    EXPECT_EQ(f.expr("a[1].x"), "script::deref(a[1]).x");
    EXPECT_EQ(f.expr("(a).x"), "script::deref((a)).x");
    EXPECT_EQ(f.expr("Vec3.Zero().x"), "script::deref(Vec3::Zero()).x");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, ScopeChain)
{
    Fixture f;
    EXPECT_EQ(f.expr("Math.Max(this.health - amount, 0.0)"), "Math::Max(st->health - amount, 0.0f)");
    EXPECT_EQ(f.expr("std.printf(\"a\")"), "std::printf(\"a\")");
    EXPECT_EQ(f.expr("Engine.World.Spawn()"), "Engine::World::Spawn()");
    EXPECT_EQ(f.expr("Color.Red"), "Color::Red");
    EXPECT_EQ(f.expr("A.B.c"), "A::B::c");
    EXPECT_EQ(f.expr("std.vector<float>(1.0, 2.0)"), "std::vector<float>(1.0f, 2.0f)");
    EXPECT_EQ(f.expr("std.vector<std.string>()"), "std::vector<std::string>()");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, LocalShadowsAScope)
{
    Fixture f;
    EXPECT_EQ(f.body("var std = 1; std.x;"), "auto std = 1; script::deref(std).x;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, EngineMemberAndThis)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.Name.c_str()"), "script::deref(self->Name).c_str()");
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
    f.param("heal");
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
    EXPECT_EQ(f.body("var p = null; const Actor* q = null; int k;"),
              "auto p = nullptr; const Actor* q = nullptr; int k = int();");
}

TEST(Lower, HandleLocalUsesStorageType)
{
    std::vector<ast::Stub> stubs(1);
    stubs[0].name = "Weapon";
    stubs[0].is_native = true;
    stubs[0].handle = "std::shared_ptr<$T>";
    LineMap smap("");
    Diagnostics sdiag;
    TypeModel types;
    types.add_stubs(stubs, sdiag, smap);
    ASSERT_TRUE(sdiag.items.empty());

    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.body("Weapon x = null; var y = null; int k;"),
              "std::shared_ptr<Weapon> x = nullptr; auto y = nullptr; int k = int();");
    EXPECT_EQ(f.body("foreach (Weapon w in ws) { }"), "for (std::shared_ptr<Weapon> w : ws) {}");

    // Without a type model the output does not change.
    Fixture g;
    EXPECT_EQ(g.body("Weapon x = null;"), "Weapon x = nullptr;");
}

namespace {

// A type model with one stub. The handle is empty for a value stub.
TypeModel stub_model(const char* name, const char* handle, bool is_value = false)
{
    std::vector<ast::Stub> stubs(1);
    stubs[0].name = name;
    stubs[0].is_native = true;
    stubs[0].handle = handle;
    stubs[0].is_value = is_value;
    LineMap smap("");
    Diagnostics sdiag;
    TypeModel types;
    types.add_stubs(stubs, sdiag, smap);
    EXPECT_TRUE(sdiag.items.empty());
    return types;
}

}  // namespace

TEST(Lower, NewHandle)
{
    const TypeModel types = stub_model("Weapon", "std::shared_ptr<$T>");
    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.expr("new Weapon(1)"), "std::make_shared<Weapon>(1)");
    EXPECT_EQ(f.expr("new Weapon(this.n)"), "std::make_shared<Weapon>(st->n)");
    // Without a type model the type stays as written.
    Fixture g;
    EXPECT_EQ(g.expr("new Weapon(1)"), "Weapon(1)");
}

TEST(Lower, NewValue)
{
    const TypeModel types = stub_model("Vec3", "", true);
    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.expr("new Vec3(1, 2, 3)"), "Vec3(1, 2, 3)");
    Fixture g;
    EXPECT_EQ(g.expr("new Vec3(1, 2, 3)"), "Vec3(1, 2, 3)");
}

TEST(Lower, NewCustomHandle)
{
    const TypeModel types = stub_model("Texture", "TexPtr<$T>");
    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.expr("new Texture(\"a\")"), "TexPtr<Texture>(new Texture(\"a\"))");
}

TEST(Lower, NewScriptClassIsAnError)
{
    Fixture f;
    f.cx.script_classes.insert("Player");
    f.expr("new Player()");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_NE(f.errors()[0].find("new of a script class is not supported yet"), std::string::npos);
}

TEST(Lower, ForOf)
{
    Fixture f;
    EXPECT_EQ(f.body("foreach (var x in xs) { this.n = x; }"), "for (auto x : xs) { st->n = x; }");
    EXPECT_EQ(f.body("foreach (const var y in ys) { }"), "for (const auto y : ys) {}");
}

TEST(Lower, ForAndControl)
{
    Fixture f;
    EXPECT_EQ(f.body("for (var i = 0; i < this.n; i++) { if (i > 2.5) { break; } }"),
              "for (auto i = 0; i < st->n; i++) { if (i > 2.5f) { break; } }");
}

TEST(Lower, Lambda)
{
    Fixture f;
    EXPECT_EQ(f.body("var f = (float x, int* q) => { q.foo(); this.n = x; };"),
              "auto f = script::make_closure(GEN, \"Player::run lambda\", [=](float x, int* q) "
              "{ [[maybe_unused]] auto* st = state_of(self); script::deref(q).foo(); st->n = x; });");
    EXPECT_EQ(f.expr("(Actor* a) => { a.x = 1; }"),
              "script::make_closure(GEN, \"Player::run lambda\", [=](Actor* a) "
              "{ [[maybe_unused]] auto* st = state_of(self); script::deref(a).x = 1; })");
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
    EXPECT_EQ(f.body("Foo.bar(); var Foo = 1;"), "Foo::bar(); auto Foo = 1;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, LocalLeaksOutOfABlock)
{
    Fixture f;
    // The old rewriter has no block scope: Foo stays a local after the block.
    EXPECT_EQ(f.body("{ var Foo = 1; } Foo.bar();"), "{ auto Foo = 1; } script::deref(Foo).bar();");
}

TEST(Lower, ScriptFnHiddenAfterBlock)
{
    Fixture f;
    EXPECT_EQ(f.body("{ var heal = 1; } heal(2);"), "{ auto heal = 1; } heal(2);");
}

TEST(Lower, LambdaParameterLeaks)
{
    Fixture f;
    EXPECT_EQ(f.body("var g = (int* q) => { }; q.x;"),
              "auto g = script::make_closure(GEN, \"Player::run lambda\", [=](int* q) "
              "{ [[maybe_unused]] auto* st = state_of(self); }); script::deref(q).x;");
}

TEST(Lower, ForOfVariableIsLocal)
{
    Fixture f;
    EXPECT_EQ(f.body("foreach (var Foo in xs) { Foo.a(); }"), "for (auto Foo : xs) { script::deref(Foo).a(); }");
}

TEST(Lower, LetPointerUsesDeref)
{
    Fixture f;
    // The member access has one form for a pointer and for a reference.
    EXPECT_EQ(f.body("Actor* p = null; p.x;"), "Actor* p = nullptr; script::deref(p).x;");
}

// ---- template arguments

TEST(Lower, TemplateArgumentsAreLowered)
{
    Fixture f;
    EXPECT_EQ(f.expr("x<this.health>(c)"), "x<st->health>(c)");
    EXPECT_EQ(f.expr("pick<Color.Red>(t)"), "pick<Color::Red>(t)");
    EXPECT_EQ(f.expr("f<1.0>(c)"), "f<1.0f>(c)");
    EXPECT_EQ(f.expr("f<this.n, 2.5>(c)"), "f<st->n, 2.5f>(c)");
    EXPECT_EQ(f.body("var b2 = x<this.health>(c);"), "auto b2 = x<st->health>(c);");
}

TEST(Lower, TypeTemplateArgumentsKeepTheirText)
{
    Fixture f;
    EXPECT_EQ(f.expr("static_cast<int>(x)"), "static_cast<int>(x)");
    EXPECT_EQ(f.expr("std.vector<std.vector<int>>()"), "std::vector<std::vector<int>>()");
    EXPECT_EQ(f.expr("f<b>(c)"), "f<b>(c)");
    EXPECT_EQ(f.expr("f<int*>(c)"), "f<int*>(c)");
    EXPECT_EQ(f.expr("f<const char*>(c)"), "f<const char*>(c)");
    EXPECT_EQ(f.expr("std.map<int,float>()"), "std::map<int,float>()");
    EXPECT_EQ(f.expr("f<std.vector<int>>(c)"), "f<std::vector<int>>(c)");
    EXPECT_EQ(f.expr("f<std.string>(c)"), "f<std::string>(c)");
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
    f.expr("1 +\n  (int x) => { }");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:3: a lambda is not allowed in a field initializer");
}

TEST(Lower, FinallyBody)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } finally { this.n = 1; }"),
              "{ script::Finally __fin_0([&] { st->n = 1; }); { throw 1; } }");
    // A loop or a switch fully inside the finally block can use break and continue.
    // A lambda in the finally block can use return.
    f.body("while (true) { try { } finally {\n"
           "  while (true) { break; continue; }\n"
           "  switch (this.n) { case 1: break; }\n"
           "  var g = () => { return 1; };\n"
           "} }");
    EXPECT_EQ(f.errors(), std::vector<std::string>{});
    // A "return" in the try body or in a catch clause is correct.
    f.body("try { return; } catch (...) { return; } finally { }");
    EXPECT_EQ(f.errors(), std::vector<std::string>{});
}

TEST(Lower, ErrorJumpOutOfFinally)
{
    Fixture f;
    f.body("try { } finally {\n  return;\n}");
    f.body("while (true) { try { } catch (...) { } finally {\n  if (true) break;\n} }");
    f.body("while (true) { try { } finally {\n  switch (this.n) { case 1: continue; }\n} }");
    f.body("while (true) { try { } finally { try { } finally {\n  return 1;\n} } }");
    EXPECT_EQ(f.errors(), (std::vector<std::string>{
                              "2:3: 'return' is not allowed in a 'finally' block",
                              "2:13: 'break' is not allowed in a 'finally' block",
                              "2:29: 'continue' is not allowed in a 'finally' block",
                              "2:3: 'return' is not allowed in a 'finally' block",
                          }));
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
    EXPECT_EQ(f.body("var a = 1;\nvar b = this.heal;"), "auto a = 1; auto b = Player__heal;");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:9: script function 'heal' must be called");
}

TEST(Lower, ErrorBadLambdaParameter)
{
    // The parser always gives a name, so the test builds the node by hand.
    Fixture f;
    LineMap map("\n  (x) => { }");
    f.cx.map = &map;
    ast::LambdaExpr l;
    l.pos = 3;
    l.params.push_back({"", "int"});
    ast::ExprP e = ast::make_expr(std::move(l));
    lower_expr(e, f.cx, f.diag);
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:3: a lambda parameter needs a name");
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

// ---------------------------------------------------------------- properties
// The fixture class with the properties Hp (get and set), Max (get only) and Sink (set only).
void add_props(Fixture& f)
{
    auto prop = [&](const char* name, bool get, bool set) {
        ast::Property p;
        p.name = name;
        p.type = "float";
        p.has_get = get;
        p.has_set = set;
        f.cd.props.push_back(std::move(p));
    };
    prop("Hp", true, true);
    prop("Max", true, false);
    prop("Sink", false, true);
    f.cx = make_class_ctx(f.cd);
}

TEST(Lower, PropertyReadAndWrite)
{
    Fixture f;
    add_props(f);
    EXPECT_EQ(f.expr("this.Hp"), "Player__get_Hp(self)");
    EXPECT_EQ(f.expr("Hp + Max"), "Player__get_Hp(self) + Player__get_Max(self)");
    EXPECT_EQ(f.body("Hp = 1;"), "Player__set_Hp(self, 1);");
    EXPECT_EQ(f.body("this.Hp = this.Max;"), "Player__set_Hp(self, Player__get_Max(self));");
    EXPECT_EQ(f.body("this.Hp *= 2 + 1;"), "Player__set_Hp(self, Player__get_Hp(self) * (2 + 1));");
    EXPECT_EQ(f.body("Hp <<= 1;"), "Player__set_Hp(self, Player__get_Hp(self) << (1));");
    EXPECT_EQ(f.body("--Hp;"), "Player__set_Hp(self, Player__get_Hp(self) - 1);");
    EXPECT_EQ(f.body("this.Hp++;"), "Player__set_Hp(self, Player__get_Hp(self) + 1);");
    EXPECT_EQ(f.body("Sink = Hp == 1 ? 2 : 3;"),
              "Player__set_Sink(self, Player__get_Hp(self) == 1 ? 2 : 3);");
    // A comparison is not a write.
    EXPECT_EQ(f.expr("Hp <= 2"), "Player__get_Hp(self) <= 2");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// "obj.P" on another object of the class. The object goes through script::shell_of.
TEST(Lower, PropertyOfOtherObjectRead)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "script::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.expr("p.Hp"), "Player__get_Hp(" + sh + ")");
    EXPECT_EQ(f.expr("p.Hp + p.Max"), "Player__get_Hp(" + sh + ") + Player__get_Max(" + sh + ")");
    EXPECT_EQ(f.expr("this.target.Hp"),
              "Player__get_Hp(script::shell_of<Player_Shell>(st->target, \"Player\"))");
    EXPECT_EQ(f.expr("p.target.Hp"),
              "Player__get_Hp(script::shell_of<Player_Shell>(script::deref(p).target, \"Player\"))");
    EXPECT_EQ(f.expr("f(p).Hp"), "Player__get_Hp(script::shell_of<Player_Shell>(f(p), \"Player\"))");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(Lower, PropertyOfOtherObjectWrite)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "script::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.body("p.Hp = 1;"), "Player__set_Hp(" + sh + ", 1);");
    EXPECT_EQ(f.body("p.Sink = p.Hp;"), "Player__set_Sink(" + sh + ", Player__get_Hp(" + sh + "));");
    EXPECT_EQ(f.body("this.target.Hp = 2;"),
              "Player__set_Hp(script::shell_of<Player_Shell>(st->target, \"Player\"), 2);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// A compound write on another object evaluates the object once: script::prop_update.
TEST(Lower, PropertyOfOtherObjectCompoundWrite)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "script::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.body("p.Hp += 2;"),
              "script::prop_update<script::op_add>(Player__get_Hp, Player__set_Hp, " + sh + ", 2);");
    EXPECT_EQ(f.body("p.Hp <<= 1 + 1;"),
              "script::prop_update<script::op_shl>(Player__get_Hp, Player__set_Hp, " + sh + ", 1 + 1);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(Lower, PropertyOfOtherObjectIncrement)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "script::shell_of<Player_Shell>(p, \"Player\")";
    const std::string acc = "Player__get_Hp, Player__set_Hp, " + sh + ", 1)";
    EXPECT_EQ(f.body("p.Hp++;"), "script::prop_update<script::op_add>(" + acc + ";");
    EXPECT_EQ(f.body("--p.Hp;"), "script::prop_update<script::op_sub>(" + acc + ";");
    // The value of "P++" is the old value. The value of "++P" is the new value.
    EXPECT_EQ(f.expr("p.Hp++"), "script::prop_post<script::op_add>(" + acc);
    EXPECT_EQ(f.expr("++p.Hp"), "script::prop_update<script::op_add>(" + acc);
    EXPECT_EQ(f.expr("Hp--"), "script::prop_post<script::op_sub>(Player__get_Hp, Player__set_Hp, self, 1)");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// A write in an expression gives the assigned value. A write in a statement stays a call.
TEST(Lower, PropertyWriteAsExpression)
{
    Fixture f;
    add_props(f);
    f.param("p");
    f.param("a");
    const std::string sh = "script::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.body("a = this.Hp = 1;"), "a = script::prop_set(Player__set_Hp, self, 1);");
    EXPECT_EQ(f.body("a = (p.Hp = 2);"), "a = (script::prop_set(Player__set_Hp, " + sh + ", 2));");
    EXPECT_EQ(f.body("Hp = Sink = 3;"),
              "Player__set_Hp(self, script::prop_set(Player__set_Sink, self, 3));");
    EXPECT_EQ(f.body("a = Hp *= 2;"),
              "a = script::prop_update<script::op_mul>(Player__get_Hp, Player__set_Hp, self, 2);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// A member that is not a property keeps the member access. "super.P" is not a property.
TEST(Lower, PropertyNameOnlyRewritesProperties)
{
    Fixture f;
    add_props(f);
    f.param("p");
    EXPECT_EQ(f.expr("p.health"), "script::deref(p).health");
    EXPECT_EQ(f.body("p.health = 1;"), "script::deref(p).health = 1;");
    EXPECT_EQ(f.body("p.health += 1;"), "script::deref(p).health += 1;");
    EXPECT_EQ(f.body("p.health++;"), "script::deref(p).health++;");
    EXPECT_EQ(f.expr("super.Hp"), "self->Actor::Hp");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(Lower, PropertyOfOtherObjectMissingAccessor)
{
    Fixture f;
    add_props(f);
    f.param("p");
    f.body("p.Max = 1;");
    f.body("p.Max += 1;");
    f.body("p.Max++;");
    f.expr("p.Sink");
    EXPECT_EQ(f.errors(), (std::vector<std::string>{
                              "1:1: property 'Max' has no 'set' accessor",
                              "1:1: property 'Max' has no 'set' accessor",
                              "1:1: property 'Max' has no 'set' accessor",
                              "1:1: property 'Sink' has no 'get' accessor",
                          }));
}

TEST(Lower, PropertyLocalHidesProperty)
{
    Fixture f;
    add_props(f);
    f.param("Hp");
    EXPECT_EQ(f.body("Hp = 1;"), "Hp = 1;");
    EXPECT_EQ(f.body("this.Hp = 1;"), "Player__set_Hp(self, 1);");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(Lower, PropertyMissingAccessor)
{
    Fixture f;
    add_props(f);
    f.body("Max = 1;");
    f.body("this.Max += 1;");
    f.body("Max++;");
    f.expr("this.Sink + 1");
    EXPECT_EQ(f.errors(), (std::vector<std::string>{
                              "1:1: property 'Max' has no 'set' accessor",
                              "1:1: property 'Max' has no 'set' accessor",
                              "1:1: property 'Max' has no 'set' accessor",
                              "1:1: property 'Sink' has no 'get' accessor",
                          }));
}

TEST(Lower, PropertyInFieldInitializerIsAnError)
{
    Fixture f;
    add_props(f);
    f.cx.allow_this = false;
    f.expr("Hp + 1");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:1: property 'Hp' is not allowed in a field initializer");
}
