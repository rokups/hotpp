// Tests for sppc::translate with handle types: fields, parameters, "T&", "&x", "new" and preludes.
#include "spp/sppc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

TEST(HandleTranslation, FieldStorage)
{
    const sppc::TranslateResult r = weapon_translate("    Weapon w = null;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Weapon> w;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using T = std::shared_ptr<Weapon>; new (f) T(nullptr);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "typeid(std::shared_ptr<Weapon>).name()")) << r.cpp;
}

TEST(HandleTranslation, ParamIsConstRef)
{
    // The handle is only read: the generation function keeps the const reference.
    const sppc::TranslateResult r = weapon_translate("    Weapon w = null;\n    void Hit(Weapon src) { src.Use(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "const std::shared_ptr<Weapon>& src";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* __self, " + p + ");")) << r.cpp;
    // A "#line" directive maps the brace of the state member to the script line.
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::Hit(" + p + ")\n#line 5 \"t.hot\"\n{")) << r.cpp;
    // The slot is the state member. No slot adapter exists.
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<spp::SlotFn>(&FooState_g1::Hit)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo__Hit_slot")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit__thunk(Widget* __a, " + p + ")")) << r.cpp;
}

TEST(HandleTranslation, StoredParamTakesACopyInTheBody)
{
    // The body stores the handle: the generation function takes a copy (design 7.10.5).
    // The state member and the thunk keep the canonical form, so a reload keeps the interface.
    const sppc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Hit(Weapon src) { this.w = src; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "const std::shared_ptr<Weapon>& src";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* __self, std::shared_ptr<Weapon> src);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::Hit(" + p.substr(0, p.size() - 3) + "__src)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> src = std::forward<" + p.substr(0, p.size() - 4) + ">(__src);"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit__thunk(Widget* __a, " + p + ")")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__st->w = src;")) << r.cpp;
}

TEST(HandleTranslation, ReferenceParamIsNotRewritten)
{
    const sppc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Swap(Weapon& wp, Vec3& v) { this.w = wp; Vec3& r = v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "Weapon& wp, Vec3& v";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Swap(Foo_Shell* __self, " + p + ");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<spp::SlotFn>(&FooState_g1::Swap)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::shared_ptr<Weapon>& wp")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Vec3& r = v;")) << r.cpp;
}

TEST(HandleTranslation, ReferenceArgumentInACall)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Fill(Vec3& v) { }\n    void Run() { Vec3 p; this.Fill(p); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__Fill(__self, p);")) << r.cpp;
}

TEST(HandleTranslation, ParamPassedToAReferenceParamTakesACopyInTheBody)
{
    // "Fill(p)" can rebind "p" through the "T&" parameter: the body takes a copy (design 7.10.5).
    const sppc::TranslateResult r = weapon_translate(
        "    void Fill(Weapon& v) { }\n    void Run(Weapon p) { this.Fill(p); }\n"
        "    void Read(Weapon v) { }\n    void Look(Weapon p) { this.Read(p); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> p = std::forward<")) << r.cpp;
    EXPECT_EQ(r.cpp.find("std::shared_ptr<Weapon> p = std::forward<"),
        r.cpp.rfind("std::shared_ptr<Weapon> p = std::forward<"))
        << r.cpp;
}

TEST(HandleTranslation, AddressOfAPointerLocalGivesANativeOutParameter)
{
    // "Find(&w)" fills a "Weapon*": the parameter is a "Weapon**".
    const sppc::TranslateResult r =
        weapon_translate("    void Find(Weapon** out) { }\n    void Run() { Weapon* w = null; this.Find(&w); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__Find(__self, &w);")) << r.cpp;
}

TEST(HandleTranslation, AddressOfAValueLocalIsAPointer)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Vec3* q) { }\n    void Run() { Vec3 p; var q = &p; this.Take(&p); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto q = &p;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Take(__self, &p);")) << r.cpp;
}

TEST(HandleTranslation, AddressOfAHandleLocalIsAnError)
{
    const sppc::TranslateResult r = weapon_translate("    void Run() { Weapon h = null; var q = &h; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics,
        "'&' on the handle 'h' is not allowed. Pass 'h' for a handle parameter, "
        "'&*h' for a 'T*' parameter, or '*h' for a 'T&' parameter"))
        << r.diagnostics;
}

TEST(HandleTranslation, AddressOfAHandleParameterIsAnError)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Weapon* q) { }\n    void Run(Weapon h) { this.Take(&h); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'&' on the handle 'h' is not allowed")) << r.diagnostics;
}

TEST(HandleTranslation, AddressOfAHandleFieldIsAnError)
{
    const sppc::TranslateResult r = weapon_translate(
        "    Weapon w = null;\n    void Take(Weapon* q) { }\n"
        "    void Run() { this.Take(&w); this.Take(&this.w); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'&' on the handle 'w' is not allowed")) << r.diagnostics;
}

TEST(HandleTranslation, AddressOfThisIsAnError)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Foo* q) { }\n    void Run() { this.Take(&this); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'&' on the handle 'this' is not allowed")) << r.diagnostics;
}

TEST(HandleTranslation, AddressOfADereferencedHandleIsAllowed)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Weapon* q) { }\n    void Run() { Weapon h = null; this.Take(&*h); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "&*h")) << r.cpp;
}

TEST(HandleTranslation, AddressOfAMemberOfAHandleIsAllowed)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(int* q) { }\n    void Run() { Weapon h = null; this.Take(&h.dmg); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(HandleTranslation, AddressOfAnUntypedVarIsNotChecked)
{
    // The type of the var is not known to the translator. Clang reports a wrong use.
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Vec3* q) { }\n    void Run() { var p = new Weapon(); this.Take(&p); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(HandleTranslation, DereferenceAndMultiplyEmitCorrectly)
{
    const sppc::TranslateResult r = weapon_translate(
        "    void Hit(Weapon src) { }\n"
        "    int Run(int* it, Weapon h) {\n"
        "        int a = *it; int b = (*it) + 1; int n = 2; int c = n * *it; this.Hit(*h); return a + b + c;\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t a = *it;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "(*it) + 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "n * *it")) << r.cpp;
}

TEST(HandleTranslation, AmpersandAndAndAreUnchanged)
{
    const sppc::TranslateResult r = weapon_translate(
        "    int Run(int m, int n, bool x, bool y) { int c = m & n; if (x && y) { c = c & 3; } return c; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t c = m & n;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "x && y")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "c = c & 3;")) << r.cpp;
    // A "&" after a binary "&" is the prefix operator. The checker allows it only for a call argument.
    const sppc::TranslateResult q = weapon_translate("    int Run(int m, int n) { int c = m & &n; return c; }\n");
    EXPECT_FALSE(q.ok);
    EXPECT_TRUE(has(q.diagnostics, "a '&' value cannot be stored")) << q.diagnostics;
}

TEST(HandleTranslation, NewAssignsAField)
{
    const sppc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Arm() { this.w = new Weapon(3); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "__st->w = std::make_shared<Weapon>(3);")) << r.cpp;
}

TEST(HandleTranslation, LocalAndReturn)
{
    const sppc::TranslateResult r = weapon_translate(
        "    Weapon w = null;\n"
        "    Weapon Get() { Weapon x = this.w; return x; }\n"
        "    int Dmg() { return this.w.dmg; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static std::shared_ptr<Weapon> Foo__Get(Foo_Shell* __self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> x = __st->w;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "spp::deref(__st->w).dmg")) << r.cpp;
}

TEST(HandleTranslation, PreludeStub)
{
    const std::string members =
        "    Weapon w = null;\n    void Hit(Weapon src) { this.w = src; }\n"
        "    Weapon Get() { Weapon x = this.w; return x; }\n";
    const sppc::TranslateResult a = weapon_translate(members);
    const sppc::TranslateResult b = weapon_translate(members, true);
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
    sppc::TranslateOptions o = temp_options(dir, weapon_script("", "    int n = 0;\n"));
    o.preludes.push_back({"bad.prelude", "class Foo : Widget {\n}\n"});
    const sppc::TranslateResult r = sppc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "bad.prelude")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "a prelude can hold only stubs")) << r.diagnostics;
}

// The message does not offer "h.get()": "." on a handle always names a member of the pointee.
TEST(HandleTranslation, AddressOfAHandleMessageDoesNotOfferGet)
{
    const sppc::TranslateResult r = weapon_translate("    void Run() { Weapon h = null; var q = &h; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(has(r.diagnostics, ".get()")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "'&*h' for a 'T*' parameter")) << r.diagnostics;
}

// "h.get()" on a handle calls "get" of the pointee, so the member must exist there.
TEST(HandleTranslation, GetOnAHandleIsAMemberOfThePointee)
{
    const sppc::TranslateResult r = weapon_translate("    void Run() { Weapon h = null; h.get(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "spp::deref(h).get()")) << r.cpp;
}

// "this" is a handle where a script handle parameter is expected.
TEST(HandleTranslation, ThisIsAHandleArgumentForAScriptHandleParameter)
{
    const sppc::TranslateResult r = weapon_translate(
        "    void Hurt(Foo h, float d) { }\n    void Run(Foo other) { other.Hurt(this, 2.0); this.Hurt(this, 1.0); "
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "spp::handle_from_this<std::shared_ptr<Foo>>(__self), 2.0f")) << r.cpp;
}

// "var me = this;" is a handle. "this" for a native pointer parameter stays the pointer.
TEST(HandleTranslation, VarOfThisIsAHandle)
{
    const sppc::TranslateResult r = weapon_translate(
        "    void Join(Foo h) { }\n    void Nat(Actor* a) { }\n"
        "    void Run() { var me = this; Join(me); Join(this); Nat(this); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto me = spp::handle_from_this<std::shared_ptr<Foo>>(__self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Join(__self, spp::handle_from_this<std::shared_ptr<Foo>>(__self));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Nat(__self, __self);")) << r.cpp;
}

// "T&" of a script class is a reference to a handle. "this" is not a variable: sppc says so.
TEST(HandleTranslation, ThisCannotBindToAScriptClassReferenceParameter)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Foo& h) { }\n    void Run(Foo other) { other.Take(this); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'this' cannot bind to the reference parameter 'h' of the script class 'Foo'"))
        << r.diagnostics;
    const sppc::TranslateResult ok =
        weapon_translate("    void Take(Foo& h) { }\n    void Run(Foo other) { Foo me = this; other.Take(me); }\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
}

// A reload copies each field, so a move-only field type is an error.
TEST(HandleTranslation, UniquePtrFieldIsAnError)
{
    const sppc::TranslateResult r = weapon_translate("    std.unique_ptr<Weapon> u;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "field of move-only type 'std::unique_ptr<Weapon>' is not supported"))
        << r.diagnostics;
    const sppc::TranslateResult loc =
        weapon_translate("    void Run(std.unique_ptr<Weapon>& p) { std.unique_ptr<Weapon> q; }\n");
    EXPECT_TRUE(loc.ok) << loc.diagnostics;
}

// The parameter of a lambda is not a local of the function after the lambda.
TEST(HandleTranslation, LambdaParameterDoesNotLeakItsType)
{
    const sppc::TranslateResult r = weapon_translate(
        "    void Take(int* q) { }\n    void F(Foo f) { }\n"
        "    void Run() { int h = 0; var l = (Weapon h) => 1; this.Take(&h); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "'&' on the handle")) << r.diagnostics;
}

// A cast to "T&" or "T*" of a script class or a handle would reinterpret the handle.
TEST(HandleTranslation, CastToAHandleReferenceOrPointerIsAnError)
{
    const char* hint = "use 'as' or static_cast<";
    for (const char* cast : {"(Weapon&)h", "(Weapon*)h", "(Weapon**)h", "(const Weapon&)h", "(Foo&)f", "(Foo*)f"})
    {
        const sppc::TranslateResult r =
            weapon_translate(std::string("    void Run(Weapon h, Foo f) { var x = ") + cast + "; }\n");
        EXPECT_FALSE(r.ok) << cast;
        EXPECT_TRUE(has(r.diagnostics, hint)) << cast << "\n" << r.diagnostics;
    }
}

TEST(HandleTranslation, CastToABuiltinOrNativeValueTypeIsAllowed)
{
    const sppc::TranslateResult r = weapon_translate(
        "    void Run(Vec3* v, float f, int* n) { var a = (Vec3*)v; var b = (float&)f; var c = (int*)n; var d = "
        "(int)f; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// "T*&" of a script class is a reference to a handle too: "this" cannot bind to it.
TEST(HandleTranslation, ThisCannotBindToAPointerReferenceParameter)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Take(Foo*& h) { }\n    void Run(Foo other) { other.Take(this); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'this' cannot bind to the reference parameter 'h' of the script class 'Foo'"))
        << r.diagnostics;
}
