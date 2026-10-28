// Tests for the borrowed-storage rule (design 7.10.4, 7.10.6) and the ERROR rows of design 5.3.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/check_translate_test_support.h"

using namespace check_translate_test;

namespace {
const char* kHandleStubs = "[native, handle(\"Arrow<$T>\")] class Handled;\n";
}

TEST(BorrowedStorage, RawPointerFieldIsAnErrorInStrictMode)
{
    const sppc::TranslateResult r = check_translate("", "    Weapon* w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "t.spp:3:13: error: a field cannot hold a borrowed pointer or reference to 'Weapon'"))
        << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Declare a stub with 'handle' or 'intrusive'")) << r.diagnostics;
}

TEST(BorrowedStorage, RawPointerFieldIsAWarningByDefault)
{
    const sppc::TranslateResult r = check_translate("", "    Weapon* w;\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.diagnostics.empty()) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "t.spp:3:13: warning: a field cannot hold a borrowed pointer")) << r.warnings;
}

TEST(BorrowedStorage, ReferenceFieldIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    Weapon& w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(BorrowedStorage, RefFieldIsAnError)
{
    const sppc::TranslateResult r = check_translate("", "    ref Vec3 v;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a 'ref' or 'out' value cannot be a field")) << r.diagnostics;
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

TEST(BorrowedStorage, HandlePointerAndConstCharAreAllowed)
{
    const sppc::TranslateResult r = check_translate(
        kHandleStubs, "    Handled* h;\n    const char* name;\n    Handled Get() { return null; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, DefaultHandleClassPointerIsAllowedButPlainDataIsNot)
{
    const sppc::TranslateResult ok = check_translate("", "    Mesh* m;\n", {{"engine.prelude", kDefaultShared}});
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
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
    const sppc::TranslateResult r = check_translate(
        "[native, value] class Plain;\n",
        "    void Run(ref Plain q1) { var f = () => { q1.x = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: the lambda captures 'q1', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(BorrowedStorage, LambdaCapturesARawPointerParameter)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void Run(Weapon* q1) { var f = () => { q1.dmg = 1; }; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'q1'")) << r.diagnostics;
}

TEST(BorrowedStorage, LambdaMayUseItsOwnParameterAndAValue)
{
    const sppc::TranslateResult r = check_translate(
        "", "    int n;\n    void Run(ref int q1) { var f = (int b) => { this.n = b; }; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(BorrowedStorage, RefParameterWithoutALambdaIsAllowed)
{
    const sppc::TranslateResult r = check_translate("", "    void Run(ref Vec3 q1, out int b) { b = 1; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

TEST(BorrowedStorage, RefLocalOfAFieldIsALocalReference)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Move() { var r = ref this.pos; r.x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto& r = st->pos;")) << r.cpp;
}

TEST(BorrowedStorage, RefInACallArgumentIsAllowed)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    void Take(ref Vec3 q1) { }\n    void Move() { this.Take(ref this.pos); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, StoredRefInAnAssignmentIsAnError)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    int* p;\n    void Move() { this.p = ref this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: a 'ref' value cannot be stored in an assignment")) << r.diagnostics;
}

TEST(ErrorRows, StoredRefInAReturnIsAnError)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    int Move() { return ref this.pos; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a 'ref' value cannot be stored")) << r.diagnostics;
}

TEST(ErrorRows, StoredRefIsAWarningByDefault)
{
    const sppc::TranslateResult r = check_translate(
        "", "    Vec3 pos;\n    int* p;\n    void Move() { this.p = ref this.pos; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "warning: a 'ref' value cannot be stored")) << r.warnings;
}

TEST(ErrorRows, AddressOfAScriptFunctionIsAnError)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void G() { }\n    void Run() { var f = G; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error: script function 'G' must be called")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "A script function has no address")) << r.diagnostics;
}

TEST(ErrorRows, AddressOfAScriptFunctionIsAnErrorEvenWithoutStrictMode)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void G() { }\n    void Run() { var f = G; }\n", {}, false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script function 'G' must be called")) << r.diagnostics;
}

TEST(ErrorRows, AScriptFunctionPassedAsAnArgumentIsAnError)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void G() { }\n    void Use(int q1) { }\n    void Run() { this.Use(G); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script function 'G' must be called")) << r.diagnostics;
}

TEST(ErrorRows, CallingAScriptFunctionIsAllowed)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void G() { }\n    void Run() { G(); this.G(); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ErrorRows, ALocalWithTheNameOfAFunctionIsNotAnAddress)
{
    const sppc::TranslateResult r = check_translate(
        "", "    void G() { }\n    int Run() { int G = 1; return G; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
