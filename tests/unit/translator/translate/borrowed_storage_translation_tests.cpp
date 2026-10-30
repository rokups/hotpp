// Tests for the borrowed-storage rule (design 7.10.4, 7.10.6) and the ERROR rows of design 5.3.
#include "hot/sppc_lib.h"
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
    const sppc::TranslateResult r = check_translate("", "    Weapon* w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(
        has(r.diagnostics, "t.hot:3:13: error: a field cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Declare a stub with 'handle' or 'intrusive'")) << r.diagnostics;
}

TEST(BorrowedStorage, RawPointerFieldIsAWarningByDefault)
{
    const sppc::TranslateResult r = check_translate("", "    Weapon* w;\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "t.hot:3:13: warning: a field cannot hold a borrowed pointer")) << r.warnings;
}

TEST(BorrowedStorage, StrictModeIsTheDefault)
{
    EXPECT_TRUE(sppc::TranslateOptions{}.strict_borrowed);
}

TEST(BorrowedStorage, ReferenceFieldIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    Weapon& w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(BorrowedStorage, RefFieldIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    Vec3& v;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Vec3'")) << r.diagnostics;
}

TEST(BorrowedStorage, CollectionElementIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    std.vector<Weapon*> list;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a collection element cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(BorrowedStorage, ReturnValueIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    Weapon* Find() { return null; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
}

TEST(BorrowedStorage, HandleAndConstCharAreAllowed)
{
    const sppc::TranslateResult r =
        check_translate(kHandleStubs, "    Handled h;\n    const char* name;\n    Handled Get() { return null; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, DefaultHandleClassHandleIsAllowedButPointersAreNot)
{
    const sppc::TranslateResult ok = check_translate("", "    Mesh m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const sppc::TranslateResult ptr = check_translate("", "    Mesh* m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_FALSE(ptr.ok);
    EXPECT_TRUE(has(ptr.diagnostics, "reference to 'Mesh'")) << ptr.diagnostics;
    const sppc::TranslateResult bad = check_translate("", "    Vec3* v;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "reference to 'Vec3'")) << bad.diagnostics;
}

TEST(BorrowedStorage, ValueAndStdTypesAreAllowed)
{
    const sppc::TranslateResult r =
        check_translate("[native, value] class Plain;\n", "    Plain p;\n    std.vector<Plain> list;\n    int n;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, LambdaCapturesARefParameter)
{
    const sppc::TranslateResult r =
        check_translate("[native, value] class Plain;\n", "    void Run(Plain& q1) { var f = () => { q1.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: the lambda captures 'q1', a borrowed pointer or reference"))
        << r.diagnostics;
}

TEST(BorrowedStorage, LambdaCapturesARawPointerParameter)
{
    const sppc::TranslateResult r =
        check_translate("", "    void Run(Weapon* q1) { var f = () => { q1.dmg = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q1'")) << r.diagnostics;
}

TEST(BorrowedStorage, LambdaMayUseItsOwnParameterAndAValue)
{
    const sppc::TranslateResult r =
        check_translate("", "    int n;\n    void Run(int& q1) { var f = (int b) => { this.n = b; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, RefParameterWithoutALambdaIsAllowed)
{
    const sppc::TranslateResult r = check_translate("", "    void Run(Vec3& q1, int& b) { b = 1; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, RefLocalOfAFieldIsALocalReference)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; r.x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Vec3& r = __st->pos;")) << r.cpp;
}

TEST(BorrowedStorage, RefInACallArgumentIsAllowed)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Take(Vec3& q1) { }\n    void Move() { this.Take(this.pos); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, StoredRefInAnAssignmentIsAnError)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    int* p;\n    void Move() { this.p = &this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a '&' value cannot be stored in an assignment")) << r.diagnostics;
}

TEST(ErrorRows, StoredRefInAReturnIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    Vec3 pos;\n    int Move() { return &this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
}

TEST(ErrorRows, StoredRefIsAWarningByDefault)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    int* p;\n    void Move() { this.p = &this.pos; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "warning: a '&' value cannot be stored")) << r.warnings;
}

TEST(ErrorRows, AddressOfAScriptFunctionIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    void G() { }\n    void Run() { var f = G; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: script function 'G' must be called")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "A script function has no address")) << r.diagnostics;
}

TEST(ErrorRows, AddressOfAScriptFunctionIsAnErrorEvenWithoutStrictMode)
{
    const sppc::TranslateResult r = check_translate("", "    void G() { }\n    void Run() { var f = G; }\n", {}, false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script function 'G' must be called")) << r.diagnostics;
}

TEST(ErrorRows, AScriptFunctionPassedAsAnArgumentIsAnError)
{
    const sppc::TranslateResult r =
        check_translate("", "    void G() { }\n    void Use(int q1) { }\n    void Run() { this.Use(G); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script function 'G' must be called")) << r.diagnostics;
}

TEST(ErrorRows, CallingAScriptFunctionIsAllowed)
{
    const sppc::TranslateResult r = check_translate("", "    void G() { }\n    void Run() { G(); this.G(); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, ALocalWithTheNameOfAFunctionIsNotAnAddress)
{
    const sppc::TranslateResult r = check_translate("", "    void G() { }\n    int Run() { int G = 1; return G; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// ---- a local that is a "T&" or holds a "&e" (design 7.10.6): allowed in a local, not stored.

TEST(RefLocal, ALambdaCannotCaptureAVarRefLocal)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var r = &this.pos; var f = () => { r.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'r'")) << r.diagnostics;
}

TEST(RefLocal, ALambdaCannotCaptureATypedRefLocal)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; var f = () => { r.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'r'")) << r.diagnostics;
}

TEST(RefLocal, ALaterRefLocalDoesNotHideAnEarlierCapture)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { var f = () => { this.pos.x = 1; }; Vec3& r = this.pos; r.x = 2; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalCannotBeReturned)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    Vec3* Get() { Vec3& r = this.pos; return r; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer")) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalCannotInitializeAField)
{
    const sppc::TranslateResult r = check_translate("", "    Vec3 pos;\n    Vec3 copy = &this.pos;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
}

TEST(RefLocal, AssigningAVarRefLocalToAValueFieldCopiesTheValue)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    Vec3 saved;\n    void Move() { Vec3& r = this.pos; this.saved = r; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, ARefLocalCannotBeStoredInAPointerField)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    Vec3* p;\n    void Move() { Vec3& r = this.pos; this.p = r; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer")) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalInAConditionalIsAnError)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move(bool c) { var r = c ? &this.pos : &this.pos; }\n");
    EXPECT_FALSE(r.ok);
}

TEST(RefLocal, ACopyOfAVarRefLocalIsAValueAndMayBeCaptured)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; Vec3 c = r; var f = () => { c.x = 1; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(RefLocal, AVarRefLocalIsAWarningWithoutStrictMode)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { Vec3& r = this.pos; var f = () => { r.x = 1; }; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "the lambda captures 'r'")) << r.warnings;
}

// S20b: a "var" local from a native call or member gets its type from libclang.
TEST(BorrowedStorage, VarFromANativeMethodIsBorrowed)
{
    const sppc::TranslateResult r =
        check_translate("", "    void Run(Owner& o) { var w = o.GetWeapon(); var f = () => { int d = w.dmg; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'w', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(BorrowedStorage, VarFromANativeFieldOrFunctionIsBorrowed)
{
    const sppc::TranslateResult r = check_translate("",
        "    void Run(Owner* o) { var a = o.weapon; var b = FindWeapon(); var c = Owner.Get().weapon;\n"
        "        var f = () => { int d = a.dmg + b.dmg + c.dmg; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'a'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'b'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'c'")) << r.diagnostics;
}

TEST(BorrowedStorage, VarOfAPointerToAHandleTypeIsNotBorrowed)
{
    const sppc::TranslateResult r = check_translate("[native, handle(\"Arrow<$T>\")] class Weapon;\n",
        "    void Run(Owner& o) { var w = o.GetWeapon(); var f = () => { int d = w.dmg; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, VarOfAnUnknownTypeKeepsTheOldRule)
{
    // "Pick" has two overloads. Libclang gives no single type, so the local has no type.
    const sppc::TranslateResult r =
        check_translate("", "    void Run(Owner& o) { var w = o.Pick(); var f = () => { int d = w.dmg; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, VarFromAChainOfLocalsIsTyped)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void Run(Owner& o) { var a = o.GetWeapon(); var n = a.dmg; var f = () => { int d = n; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto n = hot::deref(a).dmg;")) << r.cpp;
}

TEST(AddressOf, AddressOfALocalIsABorrowedLocal)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var p = &this.pos; p.x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto p = &__st->pos;")) << r.cpp;
}

TEST(AddressOf, ABorrowedAddressCannotBeCapturedByALambda)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var p = &this.pos; var f = () => { p.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'p'")) << r.diagnostics;
}

TEST(AddressOf, AddressInACallArgumentIsAllowed)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Take(Vec3* q) { }\n    void Move() { this.Take(&this.pos); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AddressOf, AddressCannotBeStoredInAField)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var p = &this.pos; this.pos = *p; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const sppc::TranslateResult s =
        check_translate("", "    Vec3 pos;\n    Vec3* keep;\n    void Move() { this.keep = &this.pos; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "a '&' value cannot be stored")) << s.diagnostics;
}

TEST(AddressOf, AddressAsAStatementOrReturnIsAFinding)
{
    const sppc::TranslateResult r = check_translate("", "    Vec3 pos;\n    void Move() { &this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
    const sppc::TranslateResult s = check_translate("", "    Vec3 pos;\n    int Get() { return &this.pos; }\n");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(has(s.diagnostics, "a '&' value cannot be stored")) << s.diagnostics;
}

TEST(AddressOf, AddressInANewArgumentAndAFieldInitializer)
{
    const sppc::TranslateResult r =
        check_translate("", "    Vec3 pos;\n    void Move() { var w = new Weapon(&this.pos); }\n");
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored") == false) << r.diagnostics;
    const sppc::TranslateResult s = check_translate("", "    Vec3 pos;\n    Vec3* copy = &this.pos;\n");
    EXPECT_FALSE(s.ok);
}

// A "var" copy of a borrowed pointer is borrowed too, also through parentheses, a ternary
// expression and a cast. A typed "T*" local is borrowed by its type.
TEST(BorrowedStorage, LambdaCapturesACopyOfABorrowedPointer)
{
    const sppc::TranslateResult r =
        check_translate("", "    void Run(bool c) { int x = 1; var p = &x; var q = p; var f = () => *q; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q', a borrowed pointer or reference")) << r.diagnostics;
    const sppc::TranslateResult v = check_translate("",
        "    void Run(bool c) { int x = 1; var p = &x; var a = (p); var b = c ? p : p; var d = (int*)p;\n"
        "        int* t = p; var f = () => *a + *b + *d + *t; }\n");
    EXPECT_FALSE(v.ok);
    for (const char* n : {"'a'", "'b'", "'d'", "'t'"})
        EXPECT_TRUE(has(v.diagnostics, std::string("the lambda captures ") + n)) << n << v.diagnostics;
}

TEST(BorrowedStorage, LambdaMayCaptureAValueCopyOfAReference)
{
    const sppc::TranslateResult r = check_translate("", "    void Run(int& r) { var c = r; var f = () => c; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// An assignment of a borrowed pointer makes the local borrowed from that point on.
TEST(BorrowedStorage, LambdaCapturesALocalThatReceivesABorrowedPointer)
{
    const sppc::TranslateResult r = check_translate("",
        "    std.function<int()> cb;\n"
        "    void Run() { int x = 1; var q = (int*)null; var p = &x; q = p; this.cb = () => *q; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q', a borrowed pointer or reference")) << r.diagnostics;
    const sppc::TranslateResult v = check_translate("",
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
    const sppc::TranslateResult r =
        check_translate("", "    void Run() { int x = 1; var q = (int*)null; q = &x; var f = () => *q; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a '&' value cannot be stored")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q'")) << r.diagnostics;
}

TEST(BorrowedStorage, PlainAssignmentAndValueCopyOfAReferenceStayLegal)
{
    const sppc::TranslateResult r =
        check_translate("", "    void Run(int& r) { var c = r; int q = 0; q = 5; c = r; var f = () => c + q; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
