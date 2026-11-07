// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the borrowed-storage rule (design 7.10.4, 7.10.6) and the ERROR rows of design 5.3.
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

namespace
{
const char* kHandleStubs = "[native, handle(\"Arrow<$T>\")] class Handled;\n";
}

TEST(BorrowedStorage, RawPointerFieldIsAnErrorInStrictMode)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon** w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(
        has(r.diagnostics, "t.hot:3:14: error: a field cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Declare a stub with 'handle' or 'intrusive'")) << r.diagnostics;
}

TEST(BorrowedStorage, RawPointerFieldIsAWarningByDefault)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon** w;\n    ~Foo() { w = null; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "t.hot:3:14: warning: a field cannot hold a borrowed pointer")) << r.warnings;
}

TEST(BorrowedStorage, StrictModeIsTheDefault)
{
    EXPECT_TRUE(hotc::TranslateOptions{}.strict_borrowed);
}

TEST(BorrowedStorage, ReferenceFieldIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon& w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "\"&\" is not a type suffix; write ref T x")) << r.diagnostics;
}

TEST(BorrowedStorage, RefFieldIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Vec3& v;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "\"&\" is not a type suffix; write ref T x")) << r.diagnostics;
}

TEST(BorrowedStorage, CollectionElementIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    std.vector<Weapon*> list;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a collection element cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

// A "T*" return of a value struct is valid (rule R4p). A "T**" return stays an error.
TEST(BorrowedStorage, ReturnValueIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon** Find() { return null; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(BorrowedStorage, HandleIsAllowed)
{
    const hotc::TranslateResult r =
        check_translate(kHandleStubs, "    Handled h;\n    Handled Get() { return null; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, DefaultHandleClassHandleIsAllowedButPointersAreNot)
{
    const hotc::TranslateResult ok = check_translate("", "    Mesh m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const hotc::TranslateResult ptr = check_translate("", "    Mesh** m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_FALSE(ptr.ok);
    EXPECT_TRUE(has(ptr.diagnostics, "reference to 'Mesh'")) << ptr.diagnostics;
    const hotc::TranslateResult bad = check_translate("", "    Vec3** v;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "reference to 'Vec3'")) << bad.diagnostics;
}

TEST(BorrowedStorage, ValueAndStdTypesAreAllowed)
{
    const hotc::TranslateResult r =
        check_translate("[native, value] class Plain;\n", "    Plain p;\n    std.vector<Plain> list;\n    int n;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, LambdaCapturesARefParameter)
{
    const hotc::TranslateResult r =
        check_translate("[native, value] class Plain;\n", "    void Run(ref Plain q1) { var f = () => { q1.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: the lambda captures the 'ref' 'q1'"))
        << r.diagnostics;
}

TEST(BorrowedStorage, LambdaCapturesARawPointerParameter)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run(Weapon* q1) { var f = () => { q1.dmg = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q1'")) << r.diagnostics;
}

TEST(BorrowedStorage, LambdaMayUseItsOwnParameterAndAValue)
{
    const hotc::TranslateResult r =
        check_translate("", "    int n;\n    void Run(ref int q1) { var f = (int b) => { this.n = b; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, RefParameterWithoutALambdaIsAllowed)
{
    const hotc::TranslateResult r = check_translate("", "    void Run(ref Vec3 q1, ref int b) { b = 1; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, RefLocalOfAFieldIsALocalReference)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { ref Vec3 r = this.pos; r.x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Vec3& r = __st->pos;")) << r.cpp;
}

TEST(BorrowedStorage, RefInACallArgumentIsAllowed)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Take(ref Vec3 q1) { }\n    void Move() { this.Take(this.pos); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, AddressOfAScriptFunctionIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    void G() { }\n    void Run() { var f = G; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: script function 'G' must be called")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "A script function has no address")) << r.diagnostics;
}

TEST(ErrorRows, AddressOfAScriptFunctionIsAnErrorEvenWithoutStrictMode)
{
    const hotc::TranslateResult r = check_translate("", "    void G() { }\n    void Run() { var f = G; }\n", {}, false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script function 'G' must be called")) << r.diagnostics;
}

TEST(ErrorRows, AScriptFunctionPassedAsAnArgumentIsAnError)
{
    const hotc::TranslateResult r =
        check_translate("", "    void G() { }\n    void Use(int q1) { }\n    void Run() { this.Use(G); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script function 'G' must be called")) << r.diagnostics;
}

TEST(ErrorRows, CallingAScriptFunctionIsAllowed)
{
    const hotc::TranslateResult r = check_translate("", "    void G() { }\n    void Run() { G(); this.G(); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, ALocalWithTheNameOfAFunctionIsNotAnAddress)
{
    const hotc::TranslateResult r = check_translate("", "    void G() { }\n    int Run() { int G = 1; return G; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// ---- a local that is a "T&" (design 7.10.6): allowed in a local, not stored.

TEST(RefLocal, ALambdaCannotCaptureATypedRefLocal)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { ref Vec3 r = this.pos; var f = () => { r.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures the 'ref' 'r'")) << r.diagnostics;
}

TEST(RefLocal, ALaterRefLocalDoesNotHideAnEarlierCapture)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { var f = () => { this.pos.x = 1; }; ref Vec3 r = this.pos; r.x = 2; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, AssigningAVarRefLocalToAValueFieldCopiesTheValue)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    Vec3 saved;\n    void Move() { ref Vec3 r = this.pos; this.saved = r; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, ARefLocalCannotBeStoredInAPointerField)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    Vec3** p;\n    void Move() { ref Vec3 r = this.pos; this.p = r; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer")) << r.diagnostics;
}

TEST(RefLocal, AConditionalOfTwoHandlesInAVarLocalIsAllowed)
{
    const hotc::TranslateResult r = check_translate(kHandleStubs,
        "    Handled a;\n    Handled b;\n    void Move(bool c) { var h = c ? a : b; var f = () => { var k = h; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, ACopyOfAVarRefLocalIsAValueAndMayBeCaptured)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { ref Vec3 r = this.pos; Vec3 c = r; var f = () => { c.x = 1; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// The capture of a "ref" is an error in the non-strict mode too (R3).
TEST(RefLocal, AVarRefLocalCaptureIsAnErrorWithoutStrictMode)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { ref Vec3 r = this.pos; var f = () => { r.x = 1; }; }\n", {}, false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures the 'ref' 'r'")) << r.diagnostics;
}

// S20b: a "var" local from a native call or member gets its type from libclang.
TEST(BorrowedStorage, VarFromANativeMethodIsBorrowed)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run(ref Owner o) { var w = o.GetWeapon(); if (w == null) return; var f = () => { int d = w.dmg; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'w', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(BorrowedStorage, VarFromANativeFieldOrFunctionIsBorrowed)
{
    const hotc::TranslateResult r = check_translate("",
        "    void Run(Owner* o) { var a = o.weapon; var b = FindWeapon(); var c = Owner.Get().weapon;\n"
        "        var f = () => { int d = a.dmg + b.dmg + c.dmg; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'a'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'b'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'c'")) << r.diagnostics;
}

TEST(BorrowedStorage, VarOfAPointerToAHandleTypeIsNotBorrowed)
{
    const hotc::TranslateResult r = check_translate("[native, handle(\"Arrow<$T>\")] class Weapon;\n",
        "    void Run(ref Owner o) { var w = o.GetWeapon(); var f = () => { int d = w?.dmg ?? 0; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, VarOfAnUnknownTypeKeepsTheOldRule)
{
    // "Pick" has two overloads. Libclang gives no single type, so the local has no type.
    const hotc::TranslateResult r =
        check_translate("", "    void Run(ref Owner o) { var w = o.Pick(); var f = () => { int d = w.dmg; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, VarFromAChainOfLocalsIsTyped)
{
    const hotc::TranslateResult r = check_translate(
        "", "    void Run(ref Owner o) { var a = o.GetWeapon(); if (a == null) return; var n = a.dmg; var f = () => { int d = n; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto n = hot::deref(a).dmg;")) << r.cpp;
}

TEST(BorrowedStorage, LambdaMayCaptureAValueCopyOfAReference)
{
    const hotc::TranslateResult r = check_translate("", "    void Run(ref int r) { var c = r; var f = () => c; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, PlainAssignmentAndValueCopyOfAReferenceStayLegal)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run(ref int r) { var c = r; int q = 0; q = 5; c = r; var f = () => c + q; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedArgs, EveryCallFormTakesABorrowedParameterAndLocal)
{
    const hotc::TranslateResult r = check_translate("",
        "    Foo(Vec3* p) { }\n"
        "    void Take(Vec3* p) { }\n"
        "    void See(ref Vec3 v) { }\n"
        "    void Run(ref Vec3 v, Vec3* p, ref Owner o) {\n"
        "        Vec3 x; ref r = x; var q = p;\n"
        "        if (p == null) return;\n"
        "        Take(p); See(v); Take(v); See(r); Take(q); Take(r); See(*p);\n"
        "        var a = new $Foo(x); var b = new $Foo(p); var c = new $Foo(q);\n"
        "        var f = (ref Vec3 w) => { See(w); }; f(v); f(x);\n"
        "        var g = (Vec3* w) => { Take(w); }; g(x); g(p); g(q);\n"
        "        try { Take(x); } catch (std.exception e) { Take(x); }\n"
        "        std.vector<Vec3> xs; foreach (ref e in xs) { See(e); Take(e); }\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A native call that gives a pointer back is not tracked as an alias, but its result is a borrowed
// local. A copy of the pointer cannot be stored or captured.
TEST(BorrowedArgs, ResultOfACallWithABorrowedArgumentStaysBorrowed)
{
    const hotc::TranslateResult s = check_translate("",
        "    void Run(Weapon* w) { var q = Echo(w); var f = () => { q.dmg = 1; }; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "the lambda captures 'q'")) << s.diagnostics;
}

// "base(*p)" is a reference to the pointee. The plan binds it by reference, like "base(r)".
TEST(BorrowedArgs, BaseArgumentOfADerefBindsByReference)
{
    const hotc::TranslateResult r = check_translate("", "    Foo(Widget* p) : base(*p) { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::ref(hot::star(p))")) << r.cpp;
}
