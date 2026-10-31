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
    const hotc::TranslateResult r = check_translate("", "    Weapon* w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(
        has(r.diagnostics, "t.hot:3:13: error: a field cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Declare a stub with 'handle' or 'intrusive'")) << r.diagnostics;
}

TEST(BorrowedStorage, RawPointerFieldIsAWarningByDefault)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon* w;\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "t.hot:3:13: warning: a field cannot hold a borrowed pointer")) << r.warnings;
}

TEST(BorrowedStorage, StrictModeIsTheDefault)
{
    EXPECT_TRUE(hotc::TranslateOptions{}.strict_borrowed);
}

TEST(BorrowedStorage, ReferenceFieldIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon& w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(BorrowedStorage, RefFieldIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Vec3& v;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Vec3'")) << r.diagnostics;
}

TEST(BorrowedStorage, CollectionElementIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    std.vector<Weapon*> list;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a collection element cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(BorrowedStorage, ReturnValueIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Weapon* Find() { return null; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(BorrowedStorage, HandleAndConstCharAreAllowed)
{
    const hotc::TranslateResult r =
        check_translate(kHandleStubs, "    Handled h;\n    const char* name;\n    Handled Get() { return null; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, DefaultHandleClassHandleIsAllowedButPointersAreNot)
{
    const hotc::TranslateResult ok = check_translate("", "    Mesh m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const hotc::TranslateResult ptr = check_translate("", "    Mesh* m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_FALSE(ptr.ok);
    EXPECT_TRUE(has(ptr.diagnostics, "reference to 'Mesh'")) << ptr.diagnostics;
    const hotc::TranslateResult bad = check_translate("", "    Vec3* v;\n", {{"engine.prelude", kDefaultShared}});
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
        check_translate("[native, value] class Plain;\n", "    void Run(Plain& q1) { var f = () => { q1.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: the lambda captures 'q1', a borrowed pointer or reference"))
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
        check_translate("", "    int n;\n    void Run(int& q1) { var f = (int b) => { this.n = b; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, RefParameterWithoutALambdaIsAllowed)
{
    const hotc::TranslateResult r = check_translate("", "    void Run(Vec3& q1, int& b) { b = 1; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, RefLocalOfAFieldIsALocalReference)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; r.x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Vec3& r = __st->pos;")) << r.cpp;
}

TEST(BorrowedStorage, RefInACallArgumentIsAllowed)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Take(Vec3& q1) { }\n    void Move() { this.Take(this.pos); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, StoredRefInAnAssignmentIsAnError)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    int* p;\n    void Move() { this.p = &this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a '&' value cannot be stored in an assignment")) << r.diagnostics;
}

TEST(ErrorRows, StoredRefInAReturnIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    Vec3 pos;\n    int Move() { return &this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
}

TEST(ErrorRows, StoredRefIsAWarningByDefault)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    int* p;\n    void Move() { this.p = &this.pos; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "warning: a '&' value cannot be stored")) << r.warnings;
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

// ---- a local that is a "T&" or holds a "&e" (design 7.10.6): allowed in a local, not stored.

TEST(RefLocal, ALambdaCannotCaptureAVarRefLocal)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var r = &this.pos; var f = () => { r.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'r'")) << r.diagnostics;
}

TEST(RefLocal, ALambdaCannotCaptureATypedRefLocal)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; var f = () => { r.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'r'")) << r.diagnostics;
}

TEST(RefLocal, ALaterRefLocalDoesNotHideAnEarlierCapture)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { var f = () => { this.pos.x = 1; }; Vec3& r = this.pos; r.x = 2; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalCannotBeReturned)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    Vec3* Get() { Vec3& r = this.pos; return r; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer")) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalCannotInitializeAField)
{
    const hotc::TranslateResult r = check_translate("", "    Vec3 pos;\n    Vec3 copy = &this.pos;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
}

TEST(RefLocal, AssigningAVarRefLocalToAValueFieldCopiesTheValue)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    Vec3 saved;\n    void Move() { Vec3& r = this.pos; this.saved = r; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, ARefLocalCannotBeStoredInAPointerField)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    Vec3* p;\n    void Move() { Vec3& r = this.pos; this.p = r; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer")) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalInAConditionalIsAnError)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move(bool c) { var r = c ? &this.pos : &this.pos; }\n");
    EXPECT_FALSE(r.ok);
}

TEST(RefLocal, ACopyOfAVarRefLocalIsAValueAndMayBeCaptured)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; Vec3 c = r; var f = () => { c.x = 1; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalIsAWarningWithoutStrictMode)
{
    const hotc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; var f = () => { r.x = 1; }; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "the lambda captures 'r'")) << r.warnings;
}

// S20b: a "var" local from a native call or member gets its type from libclang.
TEST(BorrowedStorage, VarFromANativeMethodIsBorrowed)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run(Owner& o) { var w = o.GetWeapon(); var f = () => { int d = w.dmg; }; }\n");
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
        "    void Run(Owner& o) { var w = o.GetWeapon(); var f = () => { int d = w.dmg; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, VarOfAnUnknownTypeKeepsTheOldRule)
{
    // "Pick" has two overloads. Libclang gives no single type, so the local has no type.
    const hotc::TranslateResult r =
        check_translate("", "    void Run(Owner& o) { var w = o.Pick(); var f = () => { int d = w.dmg; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, VarFromAChainOfLocalsIsTyped)
{
    const hotc::TranslateResult r = check_translate(
        "", "    void Run(Owner& o) { var a = o.GetWeapon(); var n = a.dmg; var f = () => { int d = n; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto n = hot::deref(a).dmg;")) << r.cpp;
}

TEST(AddressOf, AddressOfALocalIsABorrowedLocal)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var p = &this.pos; p.x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto p = &__st->pos;")) << r.cpp;
}

TEST(AddressOf, ABorrowedAddressCannotBeCapturedByALambda)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var p = &this.pos; var f = () => { p.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'p'")) << r.diagnostics;
}

TEST(AddressOf, AddressInACallArgumentIsAllowed)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Take(Vec3* q) { }\n    void Move() { this.Take(&this.pos); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AddressOf, AddressCannotBeStoredInAField)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var p = &this.pos; this.pos = *p; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult s =
        check_translate("", "    Vec3 pos;\n    Vec3* keep;\n    void Move() { this.keep = &this.pos; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "a '&' value cannot be stored")) << s.diagnostics;
}

TEST(AddressOf, AddressAsAStatementOrReturnIsAFinding)
{
    const hotc::TranslateResult r = check_translate("", "    Vec3 pos;\n    void Move() { &this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
    const hotc::TranslateResult s = check_translate("", "    Vec3 pos;\n    int Get() { return &this.pos; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "a '&' value cannot be stored")) << s.diagnostics;
}

TEST(AddressOf, AddressInANewArgumentAndAFieldInitializer)
{
    const hotc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var w = new Weapon(&this.pos); }\n");
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored") == false) << r.diagnostics;
    const hotc::TranslateResult s = check_translate("", "    Vec3 pos;\n    Vec3* copy = &this.pos;\n");
    EXPECT_FALSE(s.ok);
}

// A "var" copy of a borrowed pointer is borrowed too, also through parentheses, a ternary
// expression and a cast. A typed "T*" local is borrowed by its type.
TEST(BorrowedStorage, LambdaCapturesACopyOfABorrowedPointer)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run(bool c) { int x = 1; var p = &x; var q = p; var f = () => *q; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q', a borrowed pointer or reference")) << r.diagnostics;
    const hotc::TranslateResult v = check_translate("",
        "    void Run(bool c) { int x = 1; var p = &x; var a = (p); var b = c ? p : p; var d = (int*)p;\n"
        "        int* t = p; var f = () => *a + *b + *d + *t; }\n");
    EXPECT_FALSE(v.ok);
    for (const char* n : {"'a'", "'b'", "'d'", "'t'"})
        EXPECT_TRUE(has(v.diagnostics, std::string("the lambda captures ") + n)) << n << v.diagnostics;
}

TEST(BorrowedStorage, LambdaMayCaptureAValueCopyOfAReference)
{
    const hotc::TranslateResult r = check_translate("", "    void Run(int& r) { var c = r; var f = () => c; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// An assignment of a borrowed pointer makes the local borrowed from that point on.
TEST(BorrowedStorage, LambdaCapturesALocalThatReceivesABorrowedPointer)
{
    const hotc::TranslateResult r = check_translate("",
        "    std.function<int()> cb;\n"
        "    void Run() { int x = 1; var q = (int*)null; var p = &x; q = p; this.cb = () => *q; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q', a borrowed pointer or reference")) << r.diagnostics;
    const hotc::TranslateResult v = check_translate("",
        "    void Run(bool c) { int x = 1; int y = 2; var p = &x; var r = &y; var a = (int*)null;\n"
        "        var b = (int*)null; var d = (int*)null; int* t = nullptr;\n"
        "        a = c ? p : r; b = (p); d = (int*)p; t = p;\n"
        "        var f = () => *a + *b + *d + *t; }\n");
    EXPECT_FALSE(v.ok);
    for (const char* n : {"'a'", "'b'", "'d'", "'t'"})
        EXPECT_TRUE(has(v.diagnostics, std::string("the lambda captures ") + n)) << n << v.diagnostics;
}

TEST(BorrowedStorage, AssignmentOfAddressOfIsAFindingAndBorrows)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run() { int x = 1; var q = (int*)null; q = &x; var f = () => *q; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q'")) << r.diagnostics;
}

TEST(BorrowedStorage, PlainAssignmentAndValueCopyOfAReferenceStayLegal)
{
    const hotc::TranslateResult r =
        check_translate("", "    void Run(int& r) { var c = r; int q = 0; q = 5; c = r; var f = () => c + q; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// Task 40: a borrowed value is valid for the whole call, so every call form takes it.
TEST(BorrowedArgs, AddressInTernaryParenCastAndDerefIsAllowedInACall)
{
    const hotc::TranslateResult r = check_translate("",
        "    void Take(Vec3* p) { }\n"
        "    void See(Vec3& v) { }\n"
        "    void Run(Vec3& a, Vec3* b, bool c) {\n"
        "        Vec3 x; Vec3 y;\n"
        "        Take(c ? &x : &y); Take(c ? b : &x); Take((&x)); Take((Vec3*)&x); Take(c ? &a : (Vec3*)&y);\n"
        "        See(*(c ? &x : &y)); See(*(Vec3*)&x); Take(b ?? &x);\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "c ? &x : &y);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "(Vec3*)&x);")) << r.cpp;
}

TEST(BorrowedArgs, AddressInTernaryAndCastIsAllowedForALocalAndBorrows)
{
    const hotc::TranslateResult r = check_translate("",
        "    void Take(Vec3* p) { }\n"
        "    void Run(bool c) { Vec3 x; Vec3 y; var p = c ? &x : &y; var q = (Vec3*)&x; Take(p); Take(q); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult s = check_translate("",
        "    void Run(bool c) { Vec3 x; Vec3 y; var p = c ? &x : &y; var f = () => { p.x = 1; }; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "the lambda captures 'p'")) << s.diagnostics;
}

TEST(BorrowedArgs, EveryCallFormTakesABorrowedParameterAndLocal)
{
    const hotc::TranslateResult r = check_translate("",
        "    Foo(Vec3* p) { }\n"
        "    void Take(Vec3* p) { }\n"
        "    void See(Vec3& v) { }\n"
        "    void Run(Vec3& v, Vec3* p, Owner& o) {\n"
        "        Vec3 x; var& r = x; var q = &x;\n"
        "        Take(p); See(v); Take(&v); See(r); Take(q); Take(&r); Take(&*p);\n"
        "        var a = new Foo(&x); var b = new Foo(p); var c = new Foo(q);\n"
        "        var f = (Vec3& w) => { See(w); }; f(v); f(x);\n"
        "        var g = (Vec3* w) => { Take(w); }; g(&x); g(p); g(q);\n"
        "        try { Take(&x); } catch (std.exception& e) { Take(&x); }\n"
        "        std.vector<Vec3> xs; foreach (var& e in xs) { See(e); Take(&e); }\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A native call that gives a pointer back is not tracked as an alias, but its result is a borrowed
// local. A copy of the pointer cannot be stored or captured.
TEST(BorrowedArgs, ResultOfACallWithABorrowedArgumentStaysBorrowed)
{
    const hotc::TranslateResult r = check_translate("",
        "    Weapon* Pick(Weapon* w) { return w; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer")) << r.diagnostics;
    const hotc::TranslateResult s = check_translate("",
        "    void Run(Weapon* w) { var q = Echo(w); var f = () => { q.dmg = 1; }; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "the lambda captures 'q'")) << s.diagnostics;
}

TEST(BorrowedArgs, AddressStaysAnErrorOutsideACallAndALocal)
{
    for (const char* body : {"Vec3 x; Vec3 k = *(&x);", "Vec3 x; var n = &x + 1;", "Vec3 x; (c ? &x : &x);",
             "Vec3 x; last = c ? &x : &x;"})
    {
        const hotc::TranslateResult r = check_translate("",
            std::string("    Vec3* last;\n    void Run(bool c) { ") + body + " }\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << body << r.diagnostics;
    }
}

TEST(BorrowedArgs, CastNotInACallArgumentIsStillAnErrorToStore)
{
    const hotc::TranslateResult r = check_translate("",
        "    Vec3 keep;\n    Vec3* Get(Vec3& v) { return (Vec3*)&v; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
}

// "base(*p)" is a reference to the pointee. The plan binds it by reference, like "base(r)".
TEST(BorrowedArgs, BaseArgumentOfADerefBindsByReference)
{
    const hotc::TranslateResult r = check_translate("", "    Foo(Widget* p) : base(*p) { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::ref(hot::star(p))")) << r.cpp;
}
