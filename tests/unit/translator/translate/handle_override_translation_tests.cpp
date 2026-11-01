// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with an override that takes the plain handle type of a native
// parameter: "Weapon w" for "Weapon*", "const Weapon&" or "std::shared_ptr<Weapon>" (task 33).
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

const char* kGunHeader =
    "#include <memory>\n"
    "struct Weapon : std::enable_shared_from_this<Weapon> { int dmg = 0; };\n"
    "class Gun {\n"
    "public:\n"
    "    virtual ~Gun() {}\n"
    "    virtual void HitP(Weapon* w) {}\n"
    "    virtual void HitR(const Weapon& w) {}\n"
    "    virtual void HitS(std::shared_ptr<Weapon> w) {}\n"
    "    virtual void HitC(const std::shared_ptr<Weapon>& w, int n) {}\n"
    "    virtual void Two(Weapon* w) {}\n"
    "    virtual void Two(std::shared_ptr<Weapon> w) {}\n"
    "    virtual void Exact(Weapon* w) {}\n"
    "    virtual void Exact(std::shared_ptr<Weapon> w) {}\n"
    "    virtual std::shared_ptr<Weapon> Spawn() { return nullptr; }\n"
    "    virtual Weapon* Borrow() { return nullptr; }\n"
    "};\n";

hotc::TranslateResult gun_translate(const std::string& members)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kGunHeader);
    return hotc::translate(temp_options(dir, weapon_script(kWeaponStub, members, "Gun")));
}

} // namespace

TEST(HandleOverrideTranslation, RawPointerParameter)
{
    const hotc::TranslateResult r = gun_translate("    override void HitP(Weapon w) { w.dmg = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The slot keeps the native type. The body binds the handle on the first line.
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::HitP(Weapon* __w)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, " w = hot::bind<std::shared_ptr<Weapon>>(std::forward<Weapon*>(__w));")) << r.cpp;
}

TEST(HandleOverrideTranslation, ConstReferenceParameter)
{
    const hotc::TranslateResult r = gun_translate("    override void HitR(Weapon w) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::HitR(const Weapon& __w)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, " w = hot::bind<std::shared_ptr<Weapon>>(std::forward<const Weapon&>(__w));")) << r.cpp;
}

TEST(HandleOverrideTranslation, SharedPtrParameter)
{
    const hotc::TranslateResult r = gun_translate("    override void HitS(Weapon w) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::HitS(std::shared_ptr<Weapon> __w)")) << r.cpp;
    EXPECT_TRUE(
        has(r.cpp, " w = hot::bind<std::shared_ptr<Weapon>>(std::forward<std::shared_ptr<Weapon>>(__w));"))
        << r.cpp;
}

TEST(HandleOverrideTranslation, MixedParameters)
{
    const hotc::TranslateResult r = gun_translate("    override void HitC(Weapon w, int n) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::HitC(const std::shared_ptr<Weapon>& __w, int32_t n)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "std::forward<int32_t>")) << r.cpp;
}

TEST(HandleOverrideTranslation, ExactSpellingWins)
{
    // "Exact" has two native overloads. The exact spelling picks one without an error and without bind.
    const hotc::TranslateResult r = gun_translate("    override void Exact(Weapon* w) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void FooState_g1::Exact_WP(Weapon* w)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::bind")) << r.cpp;
}

TEST(HandleOverrideTranslation, TwoNativeCarriersAreAmbiguous)
{
    const hotc::TranslateResult r = gun_translate("    override void Two(Weapon w) { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the override 'Two' matches more than one native method: ")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Two(Weapon*)")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Two(std::shared_ptr<Weapon>)")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Write the exact parameter type.")) << r.diagnostics;
}

TEST(HandleOverrideTranslation, OtherTypeStillHides)
{
    const hotc::TranslateResult r = gun_translate("    override void HitP(int w) { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "function 'HitP' hides the virtual method of Gun")) << r.diagnostics;
}

TEST(HandleOverrideTranslation, PlainHandleReturn)
{
    const hotc::TranslateResult r = gun_translate("    Weapon w;\n    override Weapon Spawn() { return this.w; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The slot keeps the native return type.
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> FooState_g1::Spawn()")) << r.cpp;
}

TEST(HandleOverrideTranslation, RawPointerReturnIsNotRelaxed)
{
    const hotc::TranslateResult r = gun_translate("    override Weapon Borrow() { return null; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "return type 'Weapon' differs from 'Weapon*'")) << r.diagnostics;
}
