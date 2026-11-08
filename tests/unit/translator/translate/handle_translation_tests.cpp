// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with handle types: fields, parameters, "T&", "new" and preludes.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

TEST(HandleTranslation, FieldStorage)
{
    const hotc::TranslateResult r = weapon_translate("    Weapon w = null;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Weapon> w;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using T = std::shared_ptr<Weapon>; new (f) T(nullptr);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "typeid(hot::typekey_t<std::shared_ptr<Weapon>>).name()")) << r.cpp;
}

TEST(HandleTranslation, ParamIsConstRef)
{
    // The handle is only read: the generation function keeps the const reference.
    const hotc::TranslateResult r = weapon_translate("    Weapon w = null;\n    void Hit(Weapon src) { src.Use(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "const std::shared_ptr<Weapon>& src";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* __self, " + p + ");")) << r.cpp;
    // A "#line" directive maps the brace of the state member to the script line.
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::Hit(" + p + ")\n#line 5 \"t.hot\"\n{")) << r.cpp;
    // The slot is the state member. No slot adapter exists.
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<hot::SlotFn>(&FooState_g1::Hit)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo__Hit_slot")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit__thunk(Widget* __a, " + p + ")")) << r.cpp;
}

TEST(HandleTranslation, StoredParamTakesACopyInTheBody)
{
    // The body stores the handle: the generation function takes a copy (design 7.10.5).
    // The state member and the thunk keep the canonical form, so a reload keeps the interface.
    const hotc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Hit(Weapon src) { this.w = src; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "const std::shared_ptr<Weapon>& src";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* __self, std::shared_ptr<Weapon> src);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::Hit(" + p.substr(0, p.size() - 3) + "__src)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> const src = std::forward<" + p.substr(0, p.size() - 4) + ">(__src);"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit__thunk(Widget* __a, " + p + ")")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__st->w = src;")) << r.cpp;
}

TEST(HandleTranslation, ReferenceParamIsNotRewritten)
{
    const hotc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Swap(ref Weapon wp, ref Vec3 v) { this.w = wp; ref Vec3 r = v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "Weapon& wp, Vec3& v";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Swap(Foo_Shell* __self, " + p + ");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<hot::SlotFn>(&FooState_g1::Swap)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::shared_ptr<Weapon>& wp")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Vec3& r = v;")) << r.cpp;
}

TEST(HandleTranslation, ReferenceArgumentInACall)
{
    const hotc::TranslateResult r =
        weapon_translate("    void Fill(ref Vec3 v) { }\n    void Run() { Vec3 p; this.Fill(p); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__Fill(__self, p);")) << r.cpp;
}

TEST(HandleTranslation, ParamPassedToAReferenceParamTakesACopyInTheBody)
{
    // "Fill(p)" can rebind "p" through the "T&" parameter: the body takes a copy (design 7.10.5).
    const hotc::TranslateResult r = weapon_translate(
        "    void Fill(ref Weapon v) { }\n    void Run(Weapon p) { this.Fill(p); }\n"
        "    void Read(Weapon v) { }\n    void Look(Weapon p) { this.Read(p); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> p = std::forward<")) << r.cpp;
    EXPECT_EQ(r.cpp.find("std::shared_ptr<Weapon> p = std::forward<"),
        r.cpp.rfind("std::shared_ptr<Weapon> p = std::forward<"))
        << r.cpp;
}

TEST(HandleTranslation, AddressOfAHandleIsTheGeneralError)
{
    const hotc::TranslateResult r = weapon_translate("    void Run() { Weapon h = null; var q = &h; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the prefix '&' is not supported")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "cast a handle with (T*)h")) << r.diagnostics;
}

TEST(HandleTranslation, CastOfAHandleToAPointerGivesTheRawPointer)
{
    const hotc::TranslateResult r =
        weapon_translate("    void Take(Weapon* q) { }\n    void Run() { Weapon h = null; this.Take((Weapon*)h); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::cast_ptr<Weapon*>(h)")) << r.cpp;
}

TEST(HandleTranslation, DereferenceAndMultiplyEmitCorrectly)
{
    const hotc::TranslateResult r = weapon_translate(
        "    void Hit(Weapon src) { }\n"
        "    int Run(int* it, Weapon h) {\n"
        "        if (it == null || h == null) return 0;\n"
        "        int a = *it; int b = (*it) + 1; int n = 2; int c = n * *it; this.Hit(*h); return a + b + c;\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t a = hot::star(it);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "(hot::star(it)) + 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "n * hot::star(it)")) << r.cpp;
}

TEST(HandleTranslation, AmpersandAndAndAreUnchanged)
{
    const hotc::TranslateResult r = weapon_translate(
        "    int Run(int m, int n, bool x, bool y) { int c = m & n; if (x && y) { c = c & 3; } return c; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t c = m & n;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "x && y")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "c = c & 3;")) << r.cpp;
    // A "&" after a binary "&" is the removed prefix operator.
    const hotc::TranslateResult q = weapon_translate("    int Run(int m, int n) { int c = m & &n; return c; }\n");
    EXPECT_FALSE(q.ok);
    EXPECT_TRUE(has(q.diagnostics, "the prefix '&' is not supported")) << q.diagnostics;
}

TEST(HandleTranslation, NewAssignsAField)
{
    const hotc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Arm() { this.w = new Weapon(3); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "__st->w = std::make_shared<Weapon>(3);")) << r.cpp;
}

TEST(HandleTranslation, LocalAndReturn)
{
    const hotc::TranslateResult r = weapon_translate(
        "    Weapon w = null;\n"
        "    Weapon Get() { Weapon x = this.w; return x; }\n"
        "    int Dmg() { return this.w.dmg; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static std::shared_ptr<Weapon> Foo__Get(Foo_Shell* __self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> x = __st->w;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::deref(__st->w).dmg")) << r.cpp;
}

TEST(HandleTranslation, PreludeStub)
{
    const std::string members =
        "    Weapon w = null;\n    void Hit(Weapon src) { this.w = src; }\n"
        "    Weapon Get() { Weapon x = this.w; return x; }\n";
    const hotc::TranslateResult a = weapon_translate(members);
    const hotc::TranslateResult b = weapon_translate(members, true);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    ASSERT_TRUE(b.ok) << b.diagnostics;
    // The stub lines do not change the generated text, except the "#line" numbers.
    EXPECT_TRUE(has(b.cpp, "    std::shared_ptr<Weapon> w;\n")) << b.cpp;
    EXPECT_TRUE(has(b.cpp, "const std::shared_ptr<Weapon>& src")) << b.cpp;
    EXPECT_TRUE(has(b.cpp, "std::shared_ptr<Weapon> x = __st->w;")) << b.cpp;
}

TEST(HandleTranslation, PreludeWithClassIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWeaponHeader);
    hotc::TranslateOptions o = temp_options(dir, weapon_script("", "    int n = 0;\n"));
    o.preludes.push_back({"bad.prelude", "class Foo : Widget {\n}\n"});
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "bad.prelude")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "a prelude can hold only stubs")) << r.diagnostics;
}

// "h.get()" on a handle calls "get" of the pointee, so the member must exist there.
TEST(HandleTranslation, GetOnAHandleIsAMemberOfThePointee)
{
    const hotc::TranslateResult r = weapon_translate("    void Run() { Weapon h = null; h.get(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::deref(h).get()")) << r.cpp;
}

// "this" is a handle where a script handle parameter is expected.
TEST(HandleTranslation, ThisIsAHandleArgumentForAScriptHandleParameter)
{
    const hotc::TranslateResult r = weapon_translate(
        "    void Hurt($Foo h, float d) { }\n    void Run($Foo other) { if (other == null) return; other.Hurt(this, 2.0); this.Hurt(this, 1.0); "
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::handle_from_this<std::shared_ptr<Foo>>(__self), 2.0f")) << r.cpp;
}

// "var me = this;" is a handle. "this" for a native pointer parameter stays the pointer.
TEST(HandleTranslation, VarOfThisIsAHandle)
{
    const hotc::TranslateResult r = weapon_translate(
        "    void Join($Foo h) { }\n    void Nat(Actor* a) { }\n"
        "    void Run() { var me = this; Join(me); Join(this); Nat(this); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto me = hot::handle_from_this<std::shared_ptr<Foo>>(__self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Join(__self, hot::handle_from_this<std::shared_ptr<Foo>>(__self));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Nat(__self, __self);")) << r.cpp;
}

// "T&" of a script class is a reference to a handle. "this" is not a variable: hotc says so.
TEST(HandleTranslation, ThisCannotBindToAScriptClassReferenceParameter)
{
    const hotc::TranslateResult r =
        weapon_translate("    void Take(ref $Foo h) { }\n    void Run($Foo other) { if (other == null) return; other.Take(this); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'this' cannot bind to the reference parameter 'h' of the script class '$Foo'"))
        << r.diagnostics;
    const hotc::TranslateResult ok =
        weapon_translate("    void Take(ref $Foo h) { }\n    void Run($Foo other) { if (other == null) return; $Foo me = this; other.Take(me); }\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
}

// A reload moves a move-only field of a class. The field record uses hot::move_field.
TEST(HandleTranslation, UniquePtrFieldOfAClassMoves)
{
    const hotc::TranslateResult r =
        weapon_translate("    std.unique_ptr<Weapon> u;\n    std.vector<std.unique_ptr<Weapon>> all;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::move_field<std::unique_ptr<Weapon>>")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::move_field<std::vector<std::unique_ptr<Weapon>>>")) << r.cpp;
    const hotc::TranslateResult loc =
        weapon_translate("    void Run(ref std.unique_ptr<Weapon> p) { std.unique_ptr<Weapon> q; }\n");
    EXPECT_TRUE(loc.ok) << loc.diagnostics;
}

// The parameter of a lambda is not a local of the function after the lambda.
TEST(HandleTranslation, LambdaParameterDoesNotLeakItsType)
{
    const hotc::TranslateResult r = weapon_translate(
        "    void Take(int* q) { }\n    void F($Foo f) { }\n"
        "    void Run() { int h = 0; var l = (Weapon h) => 1; this.Take(h); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A cast to "T&", "T**" or a script class pointer would reinterpret the handle. "(T*)h" of a handle is allowed.
TEST(HandleTranslation, CastToAHandleReferenceOrDoublePointerIsAnError)
{
    const char* hint = "use 'as' or static_cast<";
    for (const char* cast : {"(Weapon**)h", "(Foo*)f"})
    {
        const hotc::TranslateResult r =
            weapon_translate(std::string("    void Run(Weapon h, $Foo f) { var x = ") + cast + "; }\n");
        EXPECT_FALSE(r.ok) << cast;
        EXPECT_TRUE(has(r.diagnostics, hint)) << cast << "\n" << r.diagnostics;
    }
}

TEST(HandleTranslation, CastToABuiltinOrNativeValueTypeIsAllowed)
{
    const hotc::TranslateResult r = weapon_translate(
        "    void Run(Vec3* v, float f, int* n) { var a = (Vec3*)v; var c = (int*)n; var d = "
        "(int)f; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// "ref T*" of a script class is a reference to a handle too: "this" cannot bind to it.
TEST(HandleTranslation, ThisCannotBindToAPointerReferenceParameter)
{
    const hotc::TranslateResult r =
        weapon_translate("    void Take(ref Foo* h) { }\n    void Run($Foo other) { if (other == null) return; other.Take(this); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'this' cannot bind to the reference parameter 'h' of the script class 'Foo'"))
        << r.diagnostics;
}
