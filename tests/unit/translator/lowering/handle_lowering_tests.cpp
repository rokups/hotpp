// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the lowering of the handle types: storage types and "new".
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

TEST(HandleLowering, HandleLocalUsesStorageType)
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
    EXPECT_EQ(f.body("Weapon x = null; var y = 1; int k;"),
        "std::shared_ptr<Weapon> x = nullptr; auto y = 1; int32_t k = int32_t();");
    EXPECT_EQ(f.body("foreach (Weapon w in ws) { }"), "for (auto __it0 = hot::iter(ws); __it0.next();) { std::shared_ptr<Weapon> w = __it0.ref(); {} }");

    // Without a type model the output does not change.
    Fixture g;
    EXPECT_EQ(g.body("Weapon x = null;"), "Weapon x = nullptr;");
}

TEST(HandleLowering, NewHandle)
{
    const TypeModel types = stub_model("Weapon", "std::shared_ptr<$T>");
    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.expr("new Weapon(1)"), "std::make_shared<Weapon>(1)");
    EXPECT_EQ(f.expr("new Weapon(this.n)"), "std::make_shared<Weapon>(__st->n)");
    // Without a type model the type stays as written.
    Fixture g;
    EXPECT_EQ(g.expr("new Weapon(1)"), "Weapon(1)");
}

TEST(HandleLowering, NewValue)
{
    const TypeModel types = stub_model("Vec3", "", true);
    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.expr("new Vec3(1, 2, 3)"), "Vec3(1, 2, 3)");
    Fixture g;
    EXPECT_EQ(g.expr("new Vec3(1, 2, 3)"), "Vec3(1, 2, 3)");
}

TEST(HandleLowering, NewCustomHandle)
{
    const TypeModel types = stub_model("Texture", "TexPtr<$T>");
    Fixture f;
    f.cx.types = &types;
    EXPECT_EQ(f.expr("new Texture(\"a\")"), "TexPtr<Texture>(new Texture(\"a\"))");
}

TEST(HandleLowering, NewScriptClassCallsTheNewFunction)
{
    Fixture f;
    f.cx.script_classes.insert("Player");
    f.cx.new_classes.insert("Player");
    EXPECT_EQ(f.expr("new Player(1, f)"), "Player__new(1, f)");
    EXPECT_TRUE(f.errors().empty());
}

TEST(HandleLowering, NewScriptClassBeforeItsDefinitionIsAnError)
{
    Fixture f;
    f.cx.script_classes.insert("Player");
    f.expr("new Player()");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_NE(f.errors()[0].find("must come before the class that builds it with 'new'"), std::string::npos);
}

// The call-site conversion of a handle to a native parameter (task 33, phase 3).
TEST(HandleLowering, HandleToNativePointerAndReference)
{
    const TypeModel types = stub_model("Weapon", "std::shared_ptr<$T>");
    Fixture f;
    f.cx.types = &types;
    std::string carrier;
    f.cx.native_key = [](const ast::CallExpr&) { return std::string("Use"); };
    f.cx.native_carrier = [&carrier](const std::string&, size_t arg, size_t) { return arg == 0 ? carrier : std::string(); };

    carrier = "p:Weapon";
    EXPECT_EQ(f.body("Weapon w = null; Use(w);"), "std::shared_ptr<Weapon> w = nullptr; Use(hot::as_ptr(w));");
    carrier = "r:Weapon";
    EXPECT_EQ(f.body("Weapon w = null; Use(w, 1);"), "std::shared_ptr<Weapon> w = nullptr; Use(hot::as_ref(w), 1);");
    // The handle form, a disagreement and another handle type keep the argument.
    carrier = "h:Weapon";
    EXPECT_EQ(f.body("Weapon w = null; Use(w);"), "std::shared_ptr<Weapon> w = nullptr; Use(w);");
    carrier = "";
    EXPECT_EQ(f.body("Weapon w = null; Use(w);"), "std::shared_ptr<Weapon> w = nullptr; Use(w);");
    carrier = "p:Shield";
    EXPECT_EQ(f.body("Weapon w = null; Use(w);"), "std::shared_ptr<Weapon> w = nullptr; Use(w);");
    // An explicit form and a non-handle argument are not converted.
    carrier = "p:Weapon";
    EXPECT_EQ(f.body("Weapon w = null; Use((Weapon*)w); Use(3);"),
        "std::shared_ptr<Weapon> w = nullptr; Use(hot::cast_ptr<Weapon*>(w)); Use(3);");
}

// A value lvalue of type T goes to a native "T*" or "const T*" parameter: "hot::as_ptr(x)".
TEST(HandleLowering, ValueLvalueToNativeValuePointer)
{
    const TypeModel types = stub_model("Weapon", "std::shared_ptr<$T>");
    Fixture f;
    f.cx.types = &types;
    std::string carrier;
    f.cx.native_key = [](const ast::CallExpr&) { return std::string("Nudge"); };
    f.cx.native_carrier = [&carrier](const std::string&, size_t arg, size_t) { return arg == 0 ? carrier : std::string(); };

    carrier = "v:Vec3";
    EXPECT_EQ(f.body("Vec3 v; Nudge(v);"), "Vec3 v = Vec3(); Nudge(hot::as_ptr(v));");
    carrier = "v:float";
    EXPECT_EQ(f.body("Nudge(health);"), "Nudge(hot::as_ptr(health));");
    carrier = "v:int32_t";
    EXPECT_EQ(f.body("int n = 0; Nudge(n);"), "int32_t n = 0; Nudge(hot::as_ptr(n));");
    // A native namespace on the parameter type still matches the script name.
    carrier = "v:Urho3D::Vec3";
    EXPECT_EQ(f.body("Vec3 v; Nudge(v);"), "Vec3 v = Vec3(); Nudge(hot::as_ptr(v));");
    // Another type and a handle keep the argument.
    carrier = "v:float";
    EXPECT_EQ(f.body("Vec3 v; Nudge(v);"), "Vec3 v = Vec3(); Nudge(v);");
    carrier = "v:Weapon";
    EXPECT_EQ(f.body("Weapon w = null; Nudge(w);"), "std::shared_ptr<Weapon> w = nullptr; Nudge(w);");
}

// A local "T*" goes to a native "T**" out-pointer parameter: "hot::as_out_ptr(p)".
TEST(HandleLowering, LocalPointerToNativeOutPointer)
{
    const TypeModel types = stub_model("Weapon", "std::shared_ptr<$T>");
    Fixture f;
    f.cx.types = &types;
    f.cx.native_key = [](const ast::CallExpr&) { return std::string("Parse"); };
    f.cx.native_carrier = [](const std::string&, size_t arg, size_t) { return arg == 0 ? std::string("o:char*") : std::string(); };

    EXPECT_EQ(f.body("char* end = null; Parse(end);"), "char* end = nullptr; Parse(hot::as_out_ptr(end));");
}

TEST(HandleLowering, HandleToNativeNeedsTheHooks)
{
    const TypeModel types = stub_model("Weapon", "std::shared_ptr<$T>");
    Fixture f;
    f.cx.types = &types;
    f.cx.native_key = [](const ast::CallExpr&) { return std::string("Use"); };
    EXPECT_EQ(f.body("Weapon w = null; Use(w);"), "std::shared_ptr<Weapon> w = nullptr; Use(w);");
}
