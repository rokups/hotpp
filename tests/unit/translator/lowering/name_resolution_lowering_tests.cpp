// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of names: locals, parameters and script functions, and their scope.
#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace lowering_test;

TEST(NameResolutionLowering, CatchOfTheRuntimeTaskErrors)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } catch (TaskCancelled e) { e.what(); } catch (hot.TaskReloadCancelled r) { r.what(); }"),
        "try { throw 1; } catch (const hot::TaskCancelled& e) { hot::deref(e).what(); } "
        "catch (const hot::TaskReloadCancelled& r) { hot::deref(r).what(); }");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, CatchNameIsALocal)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } catch (std.exception e) { e.what(); } catch (...) { throw; }"),
        "try { throw 1; } catch (const \x01std::exception& e) { hot::deref(e).what(); } "
        "catch (...) { throw; }");
    EXPECT_EQ(f.body("throw this.health;"), "throw __st->health;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalShadowsAScope)
{
    Fixture f;
    EXPECT_EQ(f.body("var std = 1; std.x;"), "auto std = 1; hot::deref(std).x;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, ScriptFnCall)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.heal(1.0)"), "Player__heal(__self, 1.0f)");
    EXPECT_EQ(f.expr("this.heal()"), "Player__heal(__self)");
    EXPECT_EQ(f.expr("heal(1.0)"), "Player__heal(__self, 1.0f)");
    EXPECT_EQ(f.expr("heal()"), "Player__heal(__self)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalHidesScriptFn)
{
    Fixture f;
    f.param("heal");
    EXPECT_EQ(f.expr("heal(1)"), "heal(1)");
}

TEST(NameResolutionLowering, LocalDoesNotLeakIntoEarlierText)
{
    Fixture f;
    // The use is before the declaration, so the name is not yet a local.
    EXPECT_EQ(f.body("Foo.bar(); var Foo = 1;"), "Foo::bar(); auto Foo = 1;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalEndsWithItsBlock)
{
    Fixture f;
    // A local has block scope: after the block, Foo is a scope again.
    EXPECT_EQ(f.body("{ var Foo = 1; } Foo.bar();"), "{ auto Foo = 1; } Foo::bar();");
}

TEST(NameResolutionLowering, ScriptFnBackAfterBlock)
{
    Fixture f;
    // The local ends with the block, so the call is the script function again.
    EXPECT_EQ(f.body("{ var heal = 1; } heal(2);"), "{ auto heal = 1; } Player__heal(__self, 2);");
}

TEST(NameResolutionLowering, LambdaParameterEndsWithTheLambda)
{
    Fixture f;
    EXPECT_EQ(f.body("var g = (int* q) => { }; q.x;"),
        "auto g = hot::make_closure(GEN, &hot_live, \"Player::run lambda\", [=](int32_t* q) "
        "{ HOT_NODEBUG_LOCAL auto* __st = state_of(__self); }); q::x;");
}

TEST(NameResolutionLowering, ForOfVariableIsLocal)
{
    Fixture f;
    EXPECT_EQ(f.body("foreach (var Foo in xs) { Foo.a(); }"), "for (auto __it0 = hot::iter(xs); __it0.next();) { auto Foo = __it0.current(); { hot::deref(Foo).a(); } }");
}

TEST(NameResolutionLowering, ErrorScriptFnMustBeCalled)
{
    Fixture f;
    EXPECT_EQ(f.body("var a = 1;\nvar b = this.heal;"), "auto a = 1; auto b = Player__heal;");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:9: script function 'heal' must be called");
}

// A bare field before "." is the field (as "this.f"), not a scope.
TEST(NameResolutionLowering, BareFieldBeforeADotIsAMember)
{
    Fixture f;
    EXPECT_EQ(f.expr("target.hp"), "hot::deref(__st->target).hp");
    EXPECT_EQ(f.expr("target.hp"), f.expr("this.target.hp"));
    EXPECT_EQ(f.body("target.push_back(1);"), "hot::deref(__st->target).push_back(1);");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalHidesABareFieldBeforeADot)
{
    Fixture f;
    f.param("target");
    EXPECT_EQ(f.expr("target.hp"), "hot::deref(target).hp");
    // A name that is not a member stays a scope.
    EXPECT_EQ(f.expr("Foo.bar"), "Foo::bar");
}

// A bare member of the native base is "this.m" when nothing of the class has the name.
TEST(NameResolutionLowering, BareNativeBaseMember)
{
    Fixture f;
    f.cx.native_members = {"Move", "Position", "Name", "health"};
    EXPECT_EQ(f.expr("Move(1)"), "__self->Move(1)");
    EXPECT_EQ(f.expr("Position"), "__self->Position");
    EXPECT_EQ(f.expr("Position.x"), "hot::deref(__self->Position).x");
    EXPECT_EQ(f.body("Name = 1;"), "__self->Name = 1;");
    // A field of the script class wins over the native member.
    EXPECT_EQ(f.expr("health"), "health");
    // A local wins over the native member.
    f.param("Name");
    EXPECT_EQ(f.expr("Name"), "Name");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// In a class the functions are free functions, so a bare field is "__st->f".
TEST(NameResolutionLowering, BareFieldOfAValueClass)
{
    Fixture f;
    f.cd.is_value = true;
    EXPECT_EQ(f.expr("health * 2"), "__st->health * 2");
    EXPECT_EQ(f.body("n += 1;"), "__st->n += 1;");
    EXPECT_EQ(f.expr("target.x"), "hot::deref(__st->target).x");
    f.param("n");
    EXPECT_EQ(f.expr("n"), "n");
}

// A script member (a field, a property) wins over a module variable of the same name. A
// module variable wins over a native base member.
TEST(NameResolutionLowering, ScriptMemberWinsOverAModuleVariable)
{
    Fixture f;
    f.cx.module_vars = {{"health", "float"}, {"level", "int"}, {"Name", "int"}};
    f.cx.native_members = {"level", "Name"};
    EXPECT_EQ(f.expr("health"), "health");
    EXPECT_EQ(f.expr("target.hp"), "hot::deref(__st->target).hp");
    EXPECT_EQ(f.expr("target.hp"), f.expr("this.target.hp"));
    EXPECT_EQ(f.expr("level"), "hot_mod()->level");
    EXPECT_EQ(f.expr("Name"), "hot_mod()->Name");
    // A local still hides both.
    f.param("health");
    EXPECT_EQ(f.expr("health"), "health");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, FieldWinsOverAModuleVariableInAValueClass)
{
    Fixture f;
    f.cd.is_value = true;
    f.cx.module_vars = {{"n", "int"}};
    EXPECT_EQ(f.expr("n + 1"), "__st->n + 1");
}

// A bare member in a field initializer is an error, as "this.m" is.
TEST(NameResolutionLowering, ErrorBareMemberInAFieldInitializer)
{
    Fixture f;
    f.cx.allow_this = false;
    f.cx.native_members = {"Name"};
    f.expr("n + 1");
    f.expr("target.hp");
    f.expr("Name");
    f.expr("Foo.bar");
    const std::string msg = "'this' is not allowed in a field initializer";
    ASSERT_EQ(f.errors().size(), 3u);
    EXPECT_EQ(f.errors()[0], "1:1: " + msg);
    EXPECT_EQ(f.errors()[1], "1:1: " + msg);
    EXPECT_EQ(f.errors()[2], "1:1: " + msg);
}

// A module variable and a name that is not a member stay valid in a field initializer.
TEST(NameResolutionLowering, FieldInitializerKeepsModuleVariableAndOtherNames)
{
    Fixture f;
    f.cx.allow_this = false;
    f.cx.module_vars = {{"limit", "int"}};
    EXPECT_EQ(f.expr("limit + 1"), "hot_mod()->limit + 1");
    EXPECT_EQ(f.expr("Foo.bar"), "Foo::bar");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// "C.F(1)" in a method of another class has the return type of the static method F.
TEST(NameResolutionLowering, StaticCallHasTheReturnTypeOfTheFunction)
{
    Fixture f;
    ast::ClassDef other;
    other.name = "Other";
    ast::Field v;
    v.name = "v";
    v.type = "int";
    v.script_type = "int";
    other.fields.push_back(std::move(v));
    ast::ClassDef util;
    util.name = "Util";
    ast::Method make;
    make.name = "Make";
    make.is_static = true;
    make.script_ret = "Other";
    ast::Param p;
    p.name = "x";
    p.type = "int";
    p.script_type = "int";
    make.params.push_back(std::move(p));
    util.statics.push_back(std::move(make));
    f.cx.classes["Other"] = &other;
    f.cx.classes["Util"] = &util;
    f.cx.script_classes.insert("Other");
    f.cx.script_classes.insert("Util");
    const std::string out = f.expr("Util.Make(1).v");
    EXPECT_NE(out.find("->v"), std::string::npos) << out;
    EXPECT_EQ(f.diag.items.size(), 0u);
}

namespace
{
ast::Method make_fn(const char* name, bool is_static = false)
{
    ast::Method m;
    m.name = name;
    m.is_static = is_static;
    ast::Param p;
    p.name = "x";
    p.type = "int";
    p.script_type = "int";
    m.params.push_back(std::move(p));
    return m;
}
} // namespace

// A call of a free function gets the name "hot_fn_F". The call has no "__self".
TEST(NameResolutionLowering, FreeFunctionCall)
{
    Fixture f;
    std::vector<ast::Method> fns;
    fns.push_back(make_fn("Add"));
    f.cx.free_fns = &fns;
    EXPECT_EQ(f.expr("Add(1, 2)"), "hot_fn_Add(1, 2)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// A bare call of a static method of the class gets the name "C__s_F".
TEST(NameResolutionLowering, BareStaticCall)
{
    Fixture f;
    f.cd.statics.push_back(make_fn("Make", true));
    EXPECT_EQ(f.expr("Make(1)"), "Player__s_Make(1)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// "C.F(args)" is a call of a static method. The owner is the class that declares it.
TEST(NameResolutionLowering, ScopedStaticCallUsesTheOwner)
{
    Fixture f;
    ast::ClassDef actor;
    actor.name = "Actor";
    actor.statics.push_back(make_fn("Spawn", true));
    ast::ClassDef child;
    child.name = "Child";
    child.base = "Actor";
    f.cx.classes["Actor"] = &actor;
    f.cx.classes["Child"] = &child;
    f.cx.script_classes.insert("Actor");
    f.cx.script_classes.insert("Child");
    EXPECT_EQ(f.expr("Actor.Spawn(1)"), "Actor__s_Spawn(1)");
    EXPECT_EQ(f.expr("Child.Spawn(2)"), "Actor__s_Spawn(2)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// A bare call in a class with a script parent finds a static method of the parent.
TEST(NameResolutionLowering, BareStaticCallFindsTheParent)
{
    Fixture f;
    ast::ClassDef actor;
    actor.name = "Actor";
    actor.statics.push_back(make_fn("Spawn", true));
    f.cx.classes["Actor"] = &actor;
    f.cx.script_classes.insert("Actor");
    EXPECT_EQ(f.expr("Spawn(3)"), "Actor__s_Spawn(3)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// A local, a field and an instance method win over a free function. A static method wins too.
TEST(NameResolutionLowering, FreeFunctionPriority)
{
    Fixture f;
    std::vector<ast::Method> fns;
    fns.push_back(make_fn("n"));
    fns.push_back(make_fn("heal"));
    fns.push_back(make_fn("loc"));
    fns.push_back(make_fn("Make"));
    f.cx.free_fns = &fns;
    f.cd.statics.push_back(make_fn("Make", true));
    f.param("loc");
    EXPECT_EQ(f.expr("heal(1)"), "Player__heal(__self, 1)");
    EXPECT_EQ(f.expr("Make(1)"), "Player__s_Make(1)");
    EXPECT_EQ(f.expr("loc(1)"), "loc(1)");
    EXPECT_EQ(f.expr("n(1)").find("hot_fn_"), std::string::npos);
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// A free function wins over a native global.
TEST(NameResolutionLowering, FreeFunctionWinsOverANativeGlobal)
{
    Fixture f;
    std::vector<ast::Method> fns;
    fns.push_back(make_fn("min"));
    f.cx.free_fns = &fns;
    EXPECT_EQ(f.expr("min(1, 2)"), "hot_fn_min(1, 2)");
    EXPECT_EQ(f.expr("max(1, 2)"), "max(1, 2)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

// The symbol function gives the names that the lowering uses.
TEST(NameResolutionLowering, StaticFunctionSymbol)
{
    EXPECT_EQ(static_fn_symbol("", "Add"), "hot_fn_Add");
    EXPECT_EQ(static_fn_symbol("Util", "Make"), "Util__s_Make");
}
