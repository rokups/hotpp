// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with weak handles (task 38): "[weak] T x" on fields, parameters and locals.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

// The field style: the attribute is on its own line.
const std::string kWeakField = "    [weak]\n    Weapon w = null;\n";

// A whole script after the Weapon header and stub: a struct, module variables and classes.
hotc::TranslateResult script_translate(const std::string& body)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    write_file(dir->path() / "w.h", kWeaponHeader);
    return hotc::translate(temp_options(*dir, "using \"w.h\";\n" + std::string(kWeaponStub) + body));
}

} // namespace

TEST(WeakHandleTranslation, FieldStorageAndKey)
{
    const hotc::TranslateResult r = weapon_translate(kWeakField);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::weak_ptr<Weapon> w;\n")) << r.cpp;
    // "= null" gives no initializer: an empty weak_ptr is null.
    EXPECT_TRUE(has(r.cpp, "using T = std::weak_ptr<Weapon>; new (f) T();")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "T(nullptr)")) << r.cpp;
    // The migration key is the weak_ptr type: a change from a strong handle resets the field.
    EXPECT_TRUE(has(r.cpp, "typeid(hot::typekey_t<std::weak_ptr<Weapon>>).name()")) << r.cpp;
}

TEST(WeakHandleTranslation, ReadLocks)
{
    const hotc::TranslateResult r = weapon_translate(kWeakField
        + "    int F() { if (w != null) return w.dmg + this.w.dmg; return 0; }\n"
          "    Weapon G() { return w; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "if (hot::lock(w) != nullptr)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::deref(hot::lock(__st->w)).dmg + hot::deref(hot::lock(__st->w)).dmg;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::lock(w);")) << r.cpp;
}

TEST(WeakHandleTranslation, AssignmentWritesTheWeakHandle)
{
    const hotc::TranslateResult r =
        weapon_translate(kWeakField + "    void Set(Weapon s) { w = s; this.w = s; }\n"
                                      "    void Clear() { w = null; this.w = null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "w = s;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__st->w = s;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::lock(w) =")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "w.reset();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__st->w.reset();")) << r.cpp;
}

TEST(WeakHandleTranslation, Parameter)
{
    const hotc::TranslateResult r = weapon_translate("    int F([weak] Weapon p) { return p.dmg; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static int32_t Foo__F(Foo_Shell* __self, std::weak_ptr<Weapon> p);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::deref(hot::lock(p)).dmg;")) << r.cpp;
}

TEST(WeakHandleTranslation, Locals)
{
    const hotc::TranslateResult r = weapon_translate(kWeakField
        + "    void F(Weapon s) {\n"
          "        [weak] Weapon x = s;\n"
          "        Weapon t = x;\n"
          "        [weak] Weapon y = x;\n"
          "        [weak] Weapon z = null;\n"
          "        var v = x;\n"
          "        x = null;\n"
          "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::weak_ptr<Weapon> x = s;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> t = hot::lock(x);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::weak_ptr<Weapon> y = hot::lock(x);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::weak_ptr<Weapon> z = std::weak_ptr<Weapon>();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto v = hot::lock(x);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "x.reset();")) << r.cpp;
}

TEST(WeakHandleTranslation, NonWeakLocalHidesWeakName)
{
    // A later local of the same name without "[weak]" is a strong handle.
    const hotc::TranslateResult r = weapon_translate(
        "    void F(Weapon s) {\n        { [weak] Weapon x = s; }\n        { Weapon x = s; int d = x.dmg; }\n    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t d = hot::deref(x).dmg;")) << r.cpp;
}

TEST(WeakHandleTranslation, LambdaCapturesTheWeakHandle)
{
    const hotc::TranslateResult r = weapon_translate(kWeakField
        + "    void F(Weapon s) {\n"
          "        [weak] Weapon x = s;\n"
          "        var f = () => x != null;\n"
          "        var g = () => w != null;\n"
          "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return hot::lock(x) != nullptr;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::lock(__st->w) != nullptr;")) << r.cpp;
}

TEST(WeakHandleTranslation, ScriptClass)
{
    // A script class without a native base is a std::shared_ptr handle.
    TempDir dir;
    hotc::TranslateOptions o = temp_options(dir,
        "class Node {\n    $Node child;\n    [weak]\n    $Node parent;\n    int hp;\n"
        "    int Up() { return parent.hp; }\n}\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::weak_ptr<Node> parent;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "state_of(hot::shell_of<Node_Shell>(hot::lock(__st->parent), \"Node\"))->hp")) << r.cpp;
}

TEST(WeakHandleTranslation, NeedsASharedHandle)
{
    for (const std::string& members : {std::string("    [weak]\n    int n;\n"),
             std::string("    [weak]\n    Vec3 v;\n"), std::string("    void F([weak] int n) { }\n"),
             std::string("    void F() { [weak] Vec3 v; }\n")})
    {
        const hotc::TranslateResult r = weapon_translate(members);
        EXPECT_FALSE(r.ok) << members;
        EXPECT_TRUE(has(r.diagnostics, "[weak] needs a std::shared_ptr handle type")) << r.diagnostics;
    }
}

TEST(WeakHandleTranslation, IntrusiveHandleIsAnError)
{
    const hotc::TranslateResult r = hier_translate(kIntrusiveStub, "    [weak]\n    Item i;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "[weak] needs a std::shared_ptr handle type, and 'Item' is not one"))
        << r.diagnostics;
}

TEST(WeakHandleTranslation, UnsupportedForms)
{
    const std::string field = kWeakField + "    int k;\n";
    struct Case
    {
        std::string body, message;
    };
    for (const Case& c : {
             Case{"    void F() { [weak] var x = w; }\n", "[weak] needs the handle type of the local"},
             Case{"    void F() { [weak] ref x = w; }\n", "[weak] needs the handle type of the local"},
             Case{"    void F() { ref d = w.dmg; }\n", "a reference into the weak handle 'w' dangles"},
             Case{"    void F() { int* p = &w.dmg; }\n", "a reference into the weak handle 'w' dangles"},
             Case{"    [weak]\n    int G() { return 0; }\n", "[weak] applies to fields, parameters and local"},
             Case{"    void F() { [strong] Weapon x = w; }\n", "unknown attribute 'strong' on a local variable"},
         })
    {
        const hotc::TranslateResult r = weapon_translate(field + c.body);
        EXPECT_FALSE(r.ok) << c.body;
        EXPECT_TRUE(has(r.diagnostics, c.message)) << c.body << r.diagnostics;
    }
}

TEST(WeakHandleTranslation, NotOnAModuleVariable)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, "[weak] int g = 1;\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'weak' is not allowed on a module variable")) << r.diagnostics;
}

TEST(WeakHandleTranslation, FieldOfAScriptStruct)
{
    const hotc::TranslateResult r = script_translate("struct S {\n    [weak]\n    Weapon n = null;\n}\n"
                                                     "class Foo : Widget {\n"
                                                     "    int F(Weapon c) {\n"
                                                     "        S s;\n"
                                                     "        s.n = c;\n"
                                                     "        Weapon h = s.n;\n"
                                                     "        int d = s.n != null ? s.n.dmg : 0;\n"
                                                     "        s.n = null;\n"
                                                     "        return d;\n"
                                                     "    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::deref(s).n = c;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon> h = hot::lock(hot::deref(s).n);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp,
        "int32_t d = hot::lock(hot::deref(s).n) != nullptr ? hot::deref(hot::lock(hot::deref(s).n)).dmg : 0;"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::deref(s).n.reset();")) << r.cpp;
}

TEST(WeakHandleTranslation, NullArgumentAndDefault)
{
    const hotc::TranslateResult r =
        weapon_translate("    void P([weak] Weapon w = null) { }\n    void Q(Weapon c) { P(null); P(); P(c); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static void Foo__P(Foo_Shell* __self, std::weak_ptr<Weapon> w = std::weak_ptr<Weapon>());"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__P(__self, std::weak_ptr<Weapon>()); Foo__P(__self); Foo__P(__self, c);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo__P(__self, nullptr)")) << r.cpp;
}

TEST(WeakHandleTranslation, WeakFieldWinsOverAModuleVariable)
{
    // A script member wins over a module variable of the same name, so the read locks.
    const hotc::TranslateResult r =
        script_translate("int w = 0;\nclass Foo : Widget {\n" + kWeakField + "    int F() { return w.dmg; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return hot::deref(hot::lock(__st->w)).dmg;")) << r.cpp;
}

TEST(WeakHandleTranslation, ShadowEndsWithItsScope)
{
    // A lambda parameter and a block local hide a weak name only inside their scope.
    const hotc::TranslateResult r = script_translate("class Node {\n    [weak]\n    $Node parent = null;\n    int hp;\n"
                                                     "    int F(Weapon s, $Node k) {\n"
                                                     "        [weak] Weapon x = s;\n"
                                                     "        var f = (Weapon x) => x.dmg;\n"
                                                     "        int d = x.dmg;\n"
                                                     "        if (k == null) return 0;\n"
                                                     "        { $Node parent = k; d = parent.hp; }\n"
                                                     "        return d + (parent?.hp ?? 0);\n"
                                                     "    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "[=](const std::shared_ptr<Weapon>& x) { HOT_NODEBUG_LOCAL auto* __st = "
                           "state_of(__self); return hot::deref(x).dmg; }"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t d = hot::deref(hot::lock(x)).dmg;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "d = state_of(hot::shell_of<Node_Shell>(parent, \"Node\"))->hp;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return d + (hot::coalesce(hot::safe_access(hot::lock(parent), "))
        << r.cpp;
}

TEST(WeakHandleTranslation, ForeachOverAWeakHandleIsAnError)
{
    const hotc::TranslateResult r = weapon_translate(kWeakField + "    void F() { foreach (var i in w.dmg) { } }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a reference into the weak handle 'w' dangles")) << r.diagnostics;
}

TEST(WeakHandleTranslation, LambdaParameterIsAnError)
{
    const hotc::TranslateResult r = weapon_translate("    void F() { var f = ([weak] Weapon x) => 0; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "[weak] is not valid on a lambda parameter")) << r.diagnostics;
}

TEST(WeakHandleTranslation, ThisForAWeakParameter)
{
    const hotc::TranslateResult r =
        script_translate("class Node {\n    void SetParent([weak] $Node p) { }\n"
                         "    void F($Node k) { if (k == null) return; k.SetParent(this); SetParent(this); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::handle_from_this<std::shared_ptr<Node>>(__self)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Node__SetParent(__self, __self)")) << r.cpp;
}

TEST(WeakHandleTranslation, SwitchLocalEndsWithTheSwitch)
{
    const hotc::TranslateResult r = script_translate("class Node {\n    [weak]\n    $Node parent = null;\n    int hp;\n"
                                                     "    int F(int k, $Node p) {\n"
                                                     "        switch (k) { case 1: $Node parent = p; break; }\n"
                                                     "        return parent.hp;\n"
                                                     "    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return state_of(hot::shell_of<Node_Shell>(hot::lock(__st->parent), \"Node\"))->hp;"))
        << r.cpp;
}

// A "new" object has no strong handle, so a weak handle to it is null at once. The error is in
// the .hot frontend only.
TEST(WeakHandleTranslation, NewInAWeakHandleIsAnError)
{
    const std::string head = "class Node {\n    [weak]\n    $Node parent = null;\n    int hp;\n";
    struct Case
    {
        std::string body;
        int count; // the number of errors
    };
    for (const Case& c : {
             Case{"    void F() { [weak] $Node x = new $Node(); }\n", 1},
             Case{"    void F() { [weak] $Node x = (new $Node()); }\n", 1},
             Case{"    void F() { parent = new $Node(); }\n", 1},
             Case{"    void F() { this.parent = new $Node(); }\n", 1},
             Case{"    void F($Node k) { if (k == null) return; k.parent = new $Node(); }\n", 1},
             Case{"    void F([weak] $Node p) { p = new $Node(); }\n", 1},
             Case{"    void F() { [weak] $Node x = null; x = new $Node(); }\n", 1},
             Case{"    void P([weak] $Node p) { }\n    void F() { P(new $Node()); }\n", 1},
             Case{"    void P(int k, [weak] $Node p = null) { }\n    void F() { P(1, new $Node()); }\n", 1},
             Case{"    void F($Node k) { if (k == null) return; k.P(new $Node()); }\n    void P([weak] $Node p) { }\n", 1},
             Case{"    Node([weak] $Node p) { }\n    void F() { var n = new $Node(new $Node()); }\n", 1},
             Case{"    void P([weak] $Node p = new $Node()) { }\n", 1},
         })
    {
        const hotc::TranslateResult r = script_translate(head + c.body + "}\n");
        EXPECT_FALSE(r.ok) << c.body;
        EXPECT_TRUE(has(r.diagnostics, "'new' cannot be stored in a weak handle")) << c.body << r.diagnostics;
        // One error for each case, also when the lowering runs more than one time.
        const std::string msg = "'new' cannot be stored in a weak handle";
        int found = 0;
        for (size_t at = r.diagnostics.find(msg); at != std::string::npos; at = r.diagnostics.find(msg, at + 1))
            ++found;
        EXPECT_EQ(found, c.count) << c.body << r.diagnostics;
    }
}

TEST(WeakHandleTranslation, NewInAWeakFieldInitializerIsAnError)
{
    const hotc::TranslateResult r = script_translate("class Node {\n    [weak]\n    $Node parent = new $Node();\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'new' cannot be stored in a weak handle")) << r.diagnostics;
}

TEST(WeakHandleTranslation, ValidSourcesOfAWeakHandle)
{
    // A strong handle, "this", a parameter, a call result and a strong handle made by "new" first.
    const hotc::TranslateResult r = script_translate(
        "class Node {\n    [weak]\n    $Node parent = null;\n    int hp;\n"
        "    $Node Make() { return new $Node(); }\n"
        "    void P([weak] $Node p) { }\n"
        "    void F($Node k) {\n"
        "        if (k == null) return;\n"
        "        $Node s = new $Node();\n"
        "        [weak] $Node a = s;\n"
        "        [weak] $Node b = k;\n"
        "        [weak] $Node c = Make();\n"
        "        parent = s;\n"
        "        k.parent = this;\n"
        "        a = this;\n"
        "        P(s);\n"
        "        P(this);\n"
        "        P(null);\n"
        "        $Node t = new $Node();\n"
        "        parent = t;\n"
        "    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "weak handle")) << r.diagnostics;
}
