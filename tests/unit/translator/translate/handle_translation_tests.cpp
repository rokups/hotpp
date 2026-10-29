// Tests for sppc::translate with handle types: fields, parameters, "ref", "new" and preludes.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"
#include "support/stub_translate_test_support.h"

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
    const sppc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Hit(Weapon src) { src.Use(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "const std::shared_ptr<Weapon>& src";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* self, " + p + ");")) << r.cpp;
    // A "#line" directive maps the brace of the generation function to the script line.
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* self, " + p + ")\n#line 5 \"t.spp\"\n{")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit_slot(Widget* a, " + p + ")")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit__thunk(Widget* a, " + p + ")")) << r.cpp;
}

TEST(HandleTranslation, StoredParamTakesACopyInTheBody)
{
    // The body stores the handle: the generation function takes a copy (design 7.10.5).
    // The slot adapter and the thunk keep the canonical form, so a reload keeps the interface.
    const sppc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Hit(Weapon src) { this.w = src; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "const std::shared_ptr<Weapon>& src";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit(Foo_Shell* self, std::shared_ptr<Weapon> src);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit_slot(Widget* a, " + p + ")")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Hit__thunk(Widget* a, " + p + ")")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "st->w = src;")) << r.cpp;
}

TEST(HandleTranslation, RefParamIsAHandleReference)
{
    const sppc::TranslateResult r = weapon_translate(
        "    Weapon w = null;\n    void Swap(ref Weapon wp, out Vec3 v) { this.w = wp; var r = ref v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string p = "std::shared_ptr<Weapon>& wp, Vec3& v";
    EXPECT_TRUE(has(r.cpp, "static void Foo__Swap(Foo_Shell* self, " + p + ");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static void Foo__Swap_slot(Widget* a, " + p + ")")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::shared_ptr<Weapon>& wp")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto& r = v;")) << r.cpp;
}

TEST(HandleTranslation, RefArgumentInACall)
{
    const sppc::TranslateResult r = weapon_translate(
        "    void Fill(out Vec3 v) { }\n    void Run() { Vec3 p; this.Fill(out p); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__Fill(self, spp::ref_arg(p));")) << r.cpp;
}

TEST(HandleTranslation, PrefixAmpersandIsAnError)
{
    const sppc::TranslateResult r =
        weapon_translate("    void Run() { Vec3 p; var q = &p; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "use 'ref' instead of '&'")) << r.diagnostics;
}

TEST(HandleTranslation, NewAssignsAField)
{
    const sppc::TranslateResult r =
        weapon_translate("    Weapon w = null;\n    void Arm() { this.w = new Weapon(3); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "st->w = std::make_shared<Weapon>(3);")) << r.cpp;
}

TEST(HandleTranslation, LocalAndReturn)
{
    const sppc::TranslateResult r = weapon_translate(
        "    Weapon w = null;\n"
        "    Weapon Get() { Weapon x = this.w; return x; }\n"
        "    int Dmg() { return this.w.dmg; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static std::shared_ptr<Weapon> Foo__Get(Foo_Shell* self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> x = st->w;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "spp::deref(st->w).dmg")) << r.cpp;
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
    EXPECT_TRUE(has(b.cpp, "std::shared_ptr<Weapon> x = st->w;")) << b.cpp;
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
