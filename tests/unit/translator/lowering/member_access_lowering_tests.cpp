// Tests for the lowering of member access: fields, "this", "base", scopes and template arguments.
// Parse the text, lower it, print it and compare the C++ text.
// The golden tests in tests/golden/ check the lowering of the example scripts.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

using namespace sppc;
using namespace lowering_test;

TEST(MemberAccessLowering, FieldAccess)
{
    Fixture f;
    f.param("x");
    EXPECT_EQ(f.body("this.fired = this.fired + 1;"), "__st->fired = __st->fired + 1;");
    EXPECT_EQ(f.body("this.n = x;"), "__st->n = x;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(MemberAccessLowering, PointerFieldUsesDeref)
{
    Fixture f;
    // The field target has the type Actor*.
    EXPECT_EQ(f.expr("this.target.Name.c_str()"),
              "spp::deref(spp::deref(__st->target).Name).c_str()");
    EXPECT_EQ(f.expr("this.target"), "__st->target");
    EXPECT_EQ(f.expr("this.target.m"), "spp::deref(__st->target).m");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(MemberAccessLowering, MemberChainOfAParameter)
{
    Fixture f;
    f.param("q");
    f.param("v");
    EXPECT_EQ(f.body("q.foo();"), "spp::deref(q).foo();");
    EXPECT_EQ(f.body("v.begin();"), "spp::deref(v).begin();");
    EXPECT_EQ(f.body("v.begin().x;"), "spp::deref(spp::deref(v).begin()).x;");
    EXPECT_EQ(f.body("q.a.b.c = 1;"),
              "spp::deref(spp::deref(spp::deref(q).a).b).c = 1;");
    EXPECT_EQ(f.body("q.m = 1;"), "spp::deref(q).m = 1;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(MemberAccessLowering, MemberOfAValue)
{
    Fixture f;
    f.param("a");
    EXPECT_EQ(f.expr("make().x"), "spp::deref(make()).x");
    EXPECT_EQ(f.expr("a[1].x"), "spp::deref(a[1]).x");
    EXPECT_EQ(f.expr("(a).x"), "spp::deref((a)).x");
    EXPECT_EQ(f.expr("Vec3.Zero().x"), "spp::deref(Vec3::Zero()).x");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(MemberAccessLowering, ScopeChain)
{
    Fixture f;
    EXPECT_EQ(f.expr("Math.Max(this.health - amount, 0.0)"), "Math::Max(__st->health - amount, 0.0f)");
    EXPECT_EQ(f.expr("std.printf(\"a\")"), "std::printf(\"a\")");
    EXPECT_EQ(f.expr("Engine.World.Spawn()"), "Engine::World::Spawn()");
    EXPECT_EQ(f.expr("Color.Red"), "Color::Red");
    EXPECT_EQ(f.expr("A.B.c"), "A::B::c");
    EXPECT_EQ(f.expr("std.vector<float>(1.0, 2.0)"), "std::vector<float>(1.0f, 2.0f)");
    EXPECT_EQ(f.expr("std.vector<std.string>()"), "std::vector<std::string>()");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(MemberAccessLowering, EngineMemberAndThis)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.Name.c_str()"), "spp::deref(__self->Name).c_str()");
    EXPECT_EQ(f.expr("this.Move(1)"), "__self->Move(1)");
    EXPECT_EQ(f.expr("this"), "__self");
    EXPECT_EQ(f.expr("f(this)"), "f(__self)");
}

TEST(MemberAccessLowering, Base)
{
    Fixture f;
    EXPECT_EQ(f.expr("base.Tick(dt)"), "__self->Actor::Tick(dt)");
}

TEST(MemberAccessLowering, LetPointerUsesDeref)
{
    Fixture f;
    // The member access has one form for a pointer and for a reference.
    EXPECT_EQ(f.body("Actor* p = null; p.x;"), "Actor* p = nullptr; spp::deref(p).x;");
}

TEST(MemberAccessLowering, TemplateArgumentsAreLowered)
{
    Fixture f;
    EXPECT_EQ(f.expr("x<this.health>(c)"), "x<__st->health>(c)");
    EXPECT_EQ(f.expr("pick<Color.Red>(t)"), "pick<Color::Red>(t)");
    EXPECT_EQ(f.expr("f<1.0>(c)"), "f<1.0f>(c)");
    EXPECT_EQ(f.expr("f<this.n, 2.5>(c)"), "f<__st->n, 2.5f>(c)");
    EXPECT_EQ(f.body("var b2 = x<this.health>(c);"), "auto b2 = x<__st->health>(c);");
}

TEST(MemberAccessLowering, TypeTemplateArgumentsKeepTheirText)
{
    Fixture f;
    EXPECT_EQ(f.expr("static_cast<int>(x)"), "static_cast<int32_t>(x)");
    EXPECT_EQ(f.expr("std.vector<std.vector<int>>()"), "std::vector<std::vector<int32_t>>()");
    EXPECT_EQ(f.expr("f<b>(c)"), "f<b>(c)");
    EXPECT_EQ(f.expr("f<int*>(c)"), "f<int32_t*>(c)");
    EXPECT_EQ(f.expr("f<const char*>(c)"), "f<const char*>(c)");
    EXPECT_EQ(f.expr("std.map<int,float>()"), "std::map<int32_t,float>()");
    EXPECT_EQ(f.expr("f<std.vector<int>>(c)"), "f<std::vector<int32_t>>(c)");
    EXPECT_EQ(f.expr("f<std.string>(c)"), "f<std::string>(c)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}
