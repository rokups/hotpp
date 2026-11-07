// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the reference types of the script: "T&" of a script class, reference locals
// ("ref"), the foreach with "ref", the hole of the borrowed field and the script text of
// the messages.
#include "builtin_types.h"
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using translator_test::translate_text;

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A script class Weapon and a class Hero with the given members.
std::string hero(const std::string& members)
{
    return "class Weapon {\n    int dmg = 1;\n}\nclass Hero {\n    $Weapon wp;\n    int n = 0;\n" + members + "}\n";
}

} // namespace

TEST(ReferenceTranslation, ScriptClassRefParameterIsAReferenceToTheHandle)
{
    const hotc::TranslateResult r = translate_text(hero("    void Make(ref $Weapon w) { w = new $Weapon(); }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static void Hero__Make(Hero_Shell* __self, std::shared_ptr<Weapon>& w);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "void HeroState_g1::Make(std::shared_ptr<Weapon>& w)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon>& w)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Weapon& w")) << r.cpp;
}

TEST(ReferenceTranslation, ScriptClassRefParameterTakesAFieldAsTheArgument)
{
    const hotc::TranslateResult r =
        translate_text(hero("    void Make(ref $Weapon w) { w = new $Weapon(); }\n    void Run() { Make(this.wp); }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Hero__Make(__self, __st->wp);")) << r.cpp;
}

TEST(ReferenceTranslation, ConstScriptClassRefParameterIsAConstHandleReference)
{
    const hotc::TranslateResult r = translate_text(hero("    void Look(in $Weapon w) { }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void HeroState_g1::Look(const std::shared_ptr<Weapon>& w)")) << r.cpp;
}

TEST(ReferenceTranslation, MemberAccessThroughAScriptClassReferenceUsesTheClass)
{
    const std::string field = "    int A($Weapon w) { if (w == null) return 0; return w.dmg; }\n";
    const std::string ref = "    int B(ref $Weapon w) { if (w == null) return 0; return w.dmg; }\n";
    const std::string cref = "    int C(in $Weapon w) { if (w == null) return 0; return w.dmg; }\n";
    const hotc::TranslateResult r = translate_text(hero(field + ref + cref));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The three forms lower the access in the same way.
    EXPECT_FALSE(has(r.cpp, "w.dmg")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::deref(w).dmg;") || has(r.cpp, "hot::shell_of<Weapon_Shell>(w, \"Weapon\")"))
        << r.cpp;
}

TEST(ReferenceTranslation, ScriptClassRefLocalIsAReferenceToTheHandle)
{
    const hotc::TranslateResult r = translate_text(hero("    void Run() { $Weapon l = this.wp; ref $Weapon w = l; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // A "ref" to a nullable field is an error (nullcheck.h): the reference binds to a local.
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon>& w = l;")) << r.cpp;
}

TEST(ReferenceTranslation, PointerToAScriptClassIsAnError)
{
    for (const char* members : {"    Weapon* p;\n", "    void Run(Weapon* p) { }\n",
             "    void Run() { Weapon* p = null; }\n", "    Weapon* Get() { return null; }\n"})
    {
        const hotc::TranslateResult r = translate_text(hero(members));
        EXPECT_FALSE(r.ok) << members;
        EXPECT_TRUE(
            has(r.diagnostics, "a pointer to the script class 'Weapon' is not allowed: use '$Weapon' (a handle)"))
            << members << r.diagnostics;
    }
}

// "ref" is for locals, foreach and parameters. A field, a property and a nested type argument cannot use it.
TEST(ReferenceTranslation, RefIsNotAStorageType)
{
    for (const char* members : {"    ref Weapon last;\n", "    ref int P { get; set; }\n", "    List<ref int> items;\n"})
    {
        const hotc::TranslateResult r = translate_text(hero(members));
        EXPECT_FALSE(r.ok) << members;
        EXPECT_TRUE(has(r.diagnostics, "'ref' cannot be stored in a field, a collection"))
            << members << r.diagnostics;
    }
    const hotc::TranslateResult m = translate_text("ref int g;\n");
    EXPECT_FALSE(m.ok);
    EXPECT_TRUE(has(m.diagnostics, "'ref' cannot be stored in a field, a collection")) << m.diagnostics;
}

TEST(ReferenceTranslation, VarRefLocalEmitsAutoRef)
{
    const hotc::TranslateResult r = translate_text(hero("    void Run() { ref r = this.n; r = 2; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto& r = ")) << r.cpp;
}

TEST(ReferenceTranslation, ReadOnlyRefLocalStaysNonConst)
{
    const hotc::TranslateResult r = translate_text(hero("    int Run() { ref r = this.n; return r; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto& r = ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const auto& r = ")) << r.cpp;
}

TEST(ReferenceTranslation, PlainVarStillCopies)
{
    const hotc::TranslateResult r = translate_text(hero("    void Run() { var r = this.n; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto r = ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "auto& r")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachRefUsesTheGuard)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (ref x in this.xs) { x = 1; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "__it0.next();) { { HOT_TRACE_LINE(); __it0.ref() = 1; } }")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachRefThatIsNotWrittenReadsTheGuard)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n    int n = 0;\n"
        "    void Run() { foreach (ref x in this.xs) { this.n = x; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "__st->n = __it0.ref();")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachPlainVarCopies)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (var x in this.xs) { } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto x = __it0.ref();")) << r.cpp;
}

TEST(ReferenceTranslation, VarRefLocalIsABorrowedLocalForALambda)
{
    const hotc::TranslateResult r =
        translate_text(hero("    void Run() { ref r = this.n; var f = () => { r = 1; }; }\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures the 'ref' 'r'")) << r.diagnostics;
}

TEST(ReferenceTranslation, ForeachVarRefIsABorrowedLocalForALambda)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (ref x in this.xs) { var f = () => { x = 1; }; } }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures the 'ref' 'x'")) << r.diagnostics;
}

// The messages speak script names.
TEST(ReferenceMessages, BorrowedFieldNamesIntNotInt32)
{
    const hotc::TranslateResult r = check_translate_test::check_translate("", "    int** p;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'int'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Declare a stub with 'handle' or 'intrusive' for 'int'")) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "int32_t")) << r.diagnostics;
}

TEST(ReferenceMessages, BorrowedReturnValueNamesUlongNotUint64)
{
    const hotc::TranslateResult r = check_translate_test::check_translate("", "    ulong* Get() { return null; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'ulong'"))
        << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "uint64_t")) << r.diagnostics;
}

TEST(ReferenceMessages, TypeTextUsesScriptNames)
{
    EXPECT_EQ(hotc::script_type_text("int32_t&"), "int&");
    EXPECT_EQ(hotc::script_type_text("const uint8_t*"), "const byte*");
    EXPECT_EQ(hotc::script_type_text("std::vector<int64_t>"), "std::vector<long>");
    EXPECT_EQ(hotc::script_type_text("ns::int32_t"), "ns::int32_t");
}

// The hole of the borrowed field: "T*" of a native handle type is a borrowed pointer.
// The exception is the intrusive stub, where "T*" is the handle.
TEST(ReferenceHole, RawPointerOfANativeHandleFieldIsAFinding)
{
    const hotc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon** wp2;\n    ~Foo() { wp2 = null; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(ReferenceHole, RawPointerOfANativeHandleIsAWarningByDefault)
{
    const hotc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon** wp2;\n    ~Foo() { wp2 = null; }\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "warning: a field cannot hold a borrowed pointer")) << r.warnings;
}

TEST(ReferenceHole, ReferenceFieldOfANativeHandleIsAFinding)
{
    const hotc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon& w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "\"&\" is not a type suffix; write ref T x")) << r.diagnostics;
}

TEST(ReferenceHole, IntrusivePointerFieldIsTheHandle)
{
    const hotc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Counted;\n", "    Counted* c;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}

// ------------------------------------------------------------------ parameter modifiers
namespace
{
std::string one_param(const std::string& param)
{
    return "class P {\n    void f(" + param + ") { }\n}\n";
}
} // namespace

TEST(ReferenceTranslation, ParameterModifiersSetTheCppParameterText)
{
    struct Case
    {
        const char* script;
        const char* cpp;
    };
    for (const Case& c : {Case{"in Vec3 v", "const Vec3& v"}, Case{"ref Vec3 v", "Vec3& v"},
             Case{"out Vec3 v", "Vec3& v"}, Case{"[byval] Vec3 v", "Vec3 v"}, Case{"Vec3 v", "hot::In<Vec3> v"}})
    {
        const std::string script = std::string("class P {\n    void f(") + c.script + ") { }\n}\n";
        const bool is_out = std::string(c.script).rfind("out ", 0) == 0;
        const hotc::TranslateResult r = translate_text(is_out ? "class P {\n    void f(out Vec3 v) { v = Vec3(); }\n}\n"
                                                              : script);
        ASSERT_TRUE(r.ok) << c.script << "\n" << r.diagnostics;
        EXPECT_TRUE(has(r.cpp, std::string("P::f(") + c.cpp + ")")
            || has(r.cpp, std::string("f(") + c.cpp + ")")
            || has(r.cpp, std::string(c.cpp) + ")"))
            << c.script << "\n" << r.cpp;
    }
}

TEST(ReferenceTranslation, AmpersandIsNotATypeSuffix)
{
    const hotc::TranslateResult r = translate_text(one_param("Vec3& v"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "\"&\" is not a type suffix; write ref T x")) << r.diagnostics;
}

TEST(ReferenceTranslation, OutParameterAssignedOnEveryPathIsAccepted)
{
    const char* bodies[] = {
        "v = 1;",
        "if (c) { v = 1; } else { v = 2; }",
        "if (c) { v = 1; return; } v = 2;",
        "if (c) { throw 1; } v = 2;",
        "switch (c) { case 1: v = 1; break; default: v = 2; break; }",
        "try { v = 1; } catch (...) { v = 2; }",
        "v = 1; while (c) { v = v + 1; }",
    };
    for (const char* body : bodies)
    {
        const hotc::TranslateResult r = translate_text(
            std::string("class P {\n    void f(bool c, out int v) { ") + body + " }\n}\n");
        EXPECT_TRUE(r.ok) << body << "\n" << r.diagnostics;
    }
}

TEST(ReferenceTranslation, OutParameterNotAssignedOnEveryPathIsAnError)
{
    const char* bodies[] = {
        "",
        "if (c) { v = 1; }",
        "if (c) { v = 1; } else { return; }",
        "while (c) { v = 1; }",
        "for (int i = 0; i < 3; i++) { v = 1; }",
        "switch (c) { case 1: v = 1; break; }",
        "try { v = 1; } catch (...) { }",
        "do { if (c) { continue; } v = 1; } while (c);",
        "do { if (c) { continue; } v = 1; } while (c); return;",
    };
    for (const char* body : bodies)
    {
        const hotc::TranslateResult r = translate_text(
            std::string("class P {\n    void f(bool c, out int v) { ") + body + " }\n}\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "out parameter \"v\" is not assigned on every path")) << body << "\n" << r.diagnostics;
    }
}

TEST(ReferenceTranslation, OutParameterAssignedBeforeEveryContinueInDoWhileIsAccepted)
{
    const char* bodies[] = {
        "do { v = 1; if (c) { continue; } } while (c);",
        "do { if (c) { v = 1; continue; } v = 2; } while (c);",
    };
    for (const char* body : bodies)
    {
        const hotc::TranslateResult r = translate_text(
            std::string("class P {\n    void f(bool c, out int v) { ") + body + " }\n}\n");
        EXPECT_TRUE(r.ok) << body << "\n" << r.diagnostics;
    }
}

// The call site does not show the mode of the callee. A whole argument counts as assigned, so
// the walk does not report this read. This test documents the limit.
TEST(ReferenceTranslation, OutParameterAsWholeCallArgumentCountsAsAssigned)
{
    const hotc::TranslateResult r = translate_text(
        "class P {\n    void g(int x) { }\n    void f(out int v) { g(v); }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ReferenceTranslation, OutParameterReadBeforeAssignmentIsAnError)
{
    const hotc::TranslateResult r = translate_text("class P {\n    void f(out int v) { int a = v; v = a; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "out parameter \"v\" is read before it is assigned")) << r.diagnostics;
}

TEST(ReferenceTranslation, OutParameterReturnBeforeAssignmentIsAnError)
{
    const hotc::TranslateResult r =
        translate_text("class P {\n    void f(bool c, out int v) { if (c) { return; } v = 1; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "out parameter \"v\" is not assigned on every path")) << r.diagnostics;
}

// A "ref" local needs a place with a lifetime (R1). These forms name a place: no diagnostic.
TEST(ReferenceTranslation, RefLocalOfAPlaceIsAllowed)
{
    const char* const prelude =
        "using <vector>;\nstruct Pos {\n    int x = 0;\n}\nclass A {\n    std.vector<int> xs;\n    Pos pos;\n    int g = 0;\n";
    for (const char* body : {"ref r = xs[i];", "ref r = this.pos.x;", "ref r = xs.back();", "ref r = xs.front();",
             "ref r = *p;", "ref r = c ? a : b;", "ref r = a; ref s = r;", "ref r = unknownGlobal;", "ref r = (a);"})
    {
        const hotc::TranslateResult r = translate_text(
            std::string(prelude) + "    void Run(int i, int* p, bool c, int a, int b) { " + body + " }\n}\n");
        EXPECT_FALSE(has(r.diagnostics, "binds to a temporary value")) << body << r.diagnostics;
    }
}

TEST(ReferenceTranslation, RefLocalOfATemporaryIsAnError)
{
    for (const char* body : {"ref r = 5;", "ref r = a + b;", "ref r = new Pos();", "ref r = p?.x;", "ref r = Make();",
             "ref r = xs.size();"})
    {
        const hotc::TranslateResult r = translate_text(
            std::string("using <vector>;\nstruct Pos {\n    int x = 0;\n}\nclass A {\n    std.vector<int> xs;\n"
                        "    int Make() { return 1; }\n    void Run(Pos* p, int a, int b) { ")
            + body + " }\n}\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "'ref r' binds to a temporary value: it ends at the end of the statement"))
            << body << r.diagnostics;
    }
}

// R3: a lambda cannot capture a "ref". A copy is fine.
TEST(ReferenceTranslation, LambdaMayUseACopyOfARef)
{
    const hotc::TranslateResult r =
        translate_text(hero("    void Run() { ref r = this.n; var c = r; var f = () => { c = 1; }; }\n"));
    EXPECT_FALSE(has(r.diagnostics, "the lambda captures")) << r.diagnostics;
}

TEST(ReferenceTranslation, LambdaCannotCaptureARefParameter)
{
    const hotc::TranslateResult r = translate_text(hero("    void Run(ref int v) { var f = () => { v = 1; }; }\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures the 'ref' 'v'")) << r.diagnostics;
}

// R3: a "ref" cannot be live across an "await".
static hotc::TranslateResult translate_cpp20(const std::string& script)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = script;
    o.generation = 1;
    o.std_version = "c++20";
    return hotc::translate(o);
}

TEST(ReferenceTranslation, RefInABlockThatEndsBeforeAwaitIsAllowed)
{
    const hotc::TranslateResult r = translate_cpp20(hero(
        "    async int Get() { return 1; }\n"
        "    async void Run() { { ref r = this.n; r = 2; } var v = await this.Get(); }\n"));
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ReferenceTranslation, RefLiveAcrossAwaitIsAnError)
{
    const hotc::TranslateResult r = translate_cpp20(hero(
        "    async int Get() { return 1; }\n"
        "    async void Run() { ref r = this.n; var v = await this.Get(); r = 2; }\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the 'ref' 'r' is live across 'await'")) << r.diagnostics;
}

// R2: a change of the shape of a container while a "ref" points to an element.
TEST(ReferenceTranslation, ShapeChangeAfterTheRefBlockIsAllowed)
{
    const char* const prelude = "using <vector>;\nclass P {\n    int hp = 0;\n    P* next = null;\n}\n"
                                "class A {\n    std.vector<int> xs;\n    std.vector<int> ys;\n    P p;\n";
    for (const char* body : {"{ ref r = xs[0]; r = 1; } xs.push_back(1);", "ref r = xs[0]; ys.push_back(r);",
             "ref r = this.p.hp; p.hp = 2;", "foreach (ref x in xs) { x = x + 1; }", "foreach (ref x in xs) { ys.push_back(x); }",
             "foreach (ref x in xs) { x = 1; } xs.clear();"})
    {
        const hotc::TranslateResult r = translate_text(std::string(prelude) + "    void Run() { " + body + " }\n}\n");
        EXPECT_FALSE(has(r.diagnostics, "can move or remove")) << body << r.diagnostics;
        EXPECT_FALSE(has(r.diagnostics, "the loop variable can dangle")) << body << r.diagnostics;
    }
}

// The foreach rule for "var": element writes and a change of a different container are allowed.
TEST(ReferenceTranslation, ForeachVarElementWriteAndOtherContainerAreAllowed)
{
    const char* const prelude = "using <vector>;\nclass P {\n    int hp = 0;\n}\n"
                                "class A {\n    std.vector<int> xs;\n    std.vector<int> ys;\n    std.vector<P> ps;\n";
    for (const char* body : {"foreach (var x in xs) { ys.push_back(x); ys.clear(); }", "foreach (var x in xs) { xs[0] = x; }",
             "foreach (var p in ps) { p.hp = 1; ps[0].hp = 2; }", "foreach (ref x in xs) { x = 1; }",
             "foreach (var x in xs) { xs[0] += 1; } xs.push_back(1);",
             "foreach (var x in xs) { var f = () => { xs.clear(); }; }",
             "foreach (var x in xs) { } xs = ys; xs.clear();",
             "foreach (var x in xs) { var xs = ys; xs.clear(); }"})
    {
        const hotc::TranslateResult r = translate_text(std::string(prelude) + "    void Run() { " + body + " }\n}\n");
        EXPECT_FALSE(has(r.diagnostics, "inside the 'foreach' over")) << body << r.diagnostics;
    }
}

// R4: a method with a "ref T" return. The C++ return type is "T&" and the return keeps the place.
TEST(ReferenceTranslation, RefReturnOfAFieldEmitsAReferenceReturn)
{
    const hotc::TranslateResult r = translate_text(hero(
        "    ref int N() { return this.n; }\n    virtual ref int V() => this.n;\n"
        "    void Run() { ref a = this.N(); a = 1; ref b = this.V(); b = 2; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t& HeroState_g1::N()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t& HeroState_g1::V()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return __st->n;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto& a = ")) << r.cpp;
}

TEST(ReferenceTranslation, RefReturnOfAStructElementKeepsTheReference)
{
    const hotc::TranslateResult r = translate_text(
        "struct Vec3 {\n    float[3] data;\n    ref float At(int i) { return this.data[i]; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static float& Vec3__At(Vec3* __self, int32_t i)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::index(__st->data, i);")) << r.cpp;
}

TEST(ReferenceTranslation, RefReturnOfAStaticAndAFreeFunctionEmitsAReference)
{
    const hotc::TranslateResult r = translate_text(hero(
        "    static ref int Pick(ref int a, ref int b) { return a > 0 ? a : b; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t& Hero__s_Pick(int32_t& a, int32_t& b)")) << r.cpp;
    const hotc::TranslateResult f = translate_text("ref int First(ref int a, ref int b) { return a; }\n");
    ASSERT_TRUE(f.ok) << f.diagnostics;
    EXPECT_TRUE(has(f.cpp, "int32_t& ")) << f.cpp;
}

// R5: the roots of a call result are the object and the arguments at "ref" parameters.
TEST(ReferenceTranslation, RefReturnThroughACallOfARefMethodFollowsTheRoots)
{
    const char* const id = "    ref int Id(ref int a) { return a; }\n";
    const hotc::TranslateResult ok = translate_text(
        hero(std::string(id) + "    ref int G(ref int k) { return this.Id(k); }\n"
                               "    ref int H() { return this.Id(this.n); }\n"
                               "    void Run() { int x = 0; ref r = this.Id(x); r = 1; }\n"));
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const hotc::TranslateResult local = translate_text(
        hero(std::string(id) + "    ref int F() { int x = 0; return this.Id(x); }\n"));
    EXPECT_FALSE(local.ok);
    EXPECT_TRUE(has(local.diagnostics, "'return' of a 'ref' to the local 'x'")) << local.diagnostics;
    const hotc::TranslateResult param = translate_text(
        hero(std::string(id) + "    ref int F(int p) { return this.Id(p); }\n"));
    EXPECT_FALSE(param.ok);
    EXPECT_TRUE(has(param.diagnostics, "'return' of a 'ref' to the parameter 'p'")) << param.diagnostics;
}

TEST(ReferenceTranslation, RefReturnOfARefLocalFollowsItsRoot)
{
    const hotc::TranslateResult ok = translate_text(
        hero("    ref int A(ref int k) { ref r = k; return r; }\n    ref int B() { ref r = this.n; return r; }\n"));
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const hotc::TranslateResult bad = translate_text(hero("    ref int A() { int x = 0; ref r = x; return r; }\n"));
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "'return' of a 'ref' to the local 'x'")) << bad.diagnostics;
}

TEST(ReferenceTranslation, RefReturnOfAContainerElementOfAFieldIsAllowed)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n    ref int At(int i) { return this.xs[i]; }\n"
        "    ref int Last() { return xs.back(); }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// R5: the result of a call of a "ref" method can be inside any field of the object.
TEST(ReferenceTranslation, ShapeChangeAfterARefCallIsAnError)
{
    const char* const prelude = "using <vector>;\nclass A {\n    std.vector<int> xs;\n    std.vector<int> ys;\n    int n = 0;\n"
                                "    ref int Item(int i) { return this.xs[i]; }\n";
    for (const char* body : {"ref r = this.Item(0); this.xs.push_back(2); r = 9;",
             "ref r = this.Item(0); this.xs = o; r = 9;", "ref r = this.Item(0); xs.clear(); r = 9;",
             "ref r = Item(0); ys.push_back(1); r = 9;"})
    {
        const hotc::TranslateResult r = translate_text(
            std::string(prelude) + "    void T(std.vector<int> o) { " + body + " }\n}\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "can move or remove the elements of")) << body << r.diagnostics;
    }
}

TEST(ReferenceTranslation, ARefCallIsValidWhileNoFieldChangesItsShape)
{
    const char* const prelude = "using <vector>;\nclass A {\n    std.vector<int> xs;\n    int n = 0;\n"
                                "    ref int Item(int i) { return this.xs[i]; }\n";
    for (const char* body : {"ref r = this.Item(0); this.n = 3; r = 9;", "{ ref r = this.Item(0); r = 9; } this.xs.push_back(2);"})
    {
        const hotc::TranslateResult r = translate_text(std::string(prelude) + "    void T() { " + body + " }\n}\n");
        EXPECT_TRUE(r.ok) << body << r.diagnostics;
    }
}

// A place behind a handle, a pointer, a "T?" or a "*" ends when the owner is replaced.
TEST(ReferenceTranslation, ReplacingTheOwnerOfARefIsAnError)
{
    const char* const prelude = "class P {\n    int hp = 0;\n}\nclass A {\n    P p;\n    $P hp;\n    int? o;\n";
    for (const char* body : {"ref r = this.p.hp; this.p = new P(); r = 1;", "ref r = this.hp.hp; this.hp = new $P(); r = 1;",
             "$P h = new $P(); ref v = h.hp; h = null; v = 1;", "int? h = 1; ref v = *h; h = null; v = 1;",
             "ref v = *this.o; this.o = null; v = 1;", "var u = new P(); ref h = u.hp; u = new P(); h = 1;",
             "var u = this.o; ref v = *u; u = null; v = 1;", "ref u = this.p; ref h = u.hp; this.p = new P(); h = 1;"})
    {
        const hotc::TranslateResult r = translate_text(std::string(prelude) + "    void T() { " + body + " }\n}\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "can move or remove the elements of")) << body << r.diagnostics;
    }
}

TEST(ReferenceTranslation, ReplacingAStructOwnerOfARefIsAllowed)
{
    const hotc::TranslateResult r = translate_text(
        "struct S {\n    int x = 0;\n}\nclass A {\n    S s;\n    void T() { ref r = this.s.x; this.s = new S(); r = 1; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A root "this.name" and a parameter or a local with the same name are two places.
TEST(ReferenceTranslation, RefReturnOfAFieldIsNotAParameterOfTheSameName)
{
    const hotc::TranslateResult r = translate_text(
        "class A {\n    int hp = 0;\n    int n = 0;\n    ref int F(int hp) { return this.hp; }\n"
        "    ref int G() { { var n = 1; } return this.n; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ReferenceTranslation, RefToAMemberOfARefLocalFollowsTheLocal)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nstruct W {\n    int damage = 0;\n}\nstruct Pl {\n    std.vector<W> weapons;\n}\n"
        "class A {\n    ref int D(ref Pl p) { ref w = p.weapons[0]; return w.damage; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ReferenceTranslation, ForeachVariableThatHidesARefGivesItBack)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n    std.vector<int> ys;\n"
        "    void T() { ref r = xs[0]; foreach (var r in ys) { } xs.push_back(1); r = 2; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "can move or remove the elements of 'xs'")) << r.diagnostics;
}

// A "ref" argument of a call through "this" or the class name can change the shape of the container.
TEST(ReferenceTranslation, RefArgumentOfAMemberOrStaticCallChangesTheShape)
{
    const char* const prelude = "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
                                "    void Fill(ref std.vector<int> v) { v.push_back(1); }\n"
                                "    static void SFill(ref std.vector<int> v) { v.push_back(1); }\n";
    for (const char* body : {"ref r = xs[0]; this.Fill(xs); r = 1;", "ref r = xs[0]; A.SFill(this.xs); r = 1;"})
    {
        const hotc::TranslateResult r = translate_text(std::string(prelude) + "    void T() { " + body + " }\n}\n");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(has(r.diagnostics, "passed by 'ref'")) << body << r.diagnostics;
    }
}

// The entry type of an interface method that returns "ref int" is "int32_t&", as in the class.
TEST(ReferenceTranslation, InterfaceMethodWithARefReturnUsesAReferenceType)
{
    const hotc::TranslateResult r = translate_text(
        "interface I { ref int Get(); }\nclass A : I {\n    int n = 0;\n    ref int Get() { return this.n; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "inline int32_t& I__Get(const hot::Iface<I__iface>& h)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t& Get();")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "ref int32_t")) << r.cpp;
}

// A struct field can hold a container: a ref call can point into it.
TEST(ReferenceTranslation, ReplacingAStructFieldThatHoldsAContainerEndsARefCall)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nstruct Inv {\n    std.vector<int> items;\n}\nclass A {\n    Inv inv;\n"
        "    ref int Item(int i) { return this.inv.items[i]; }\n"
        "    void T() { ref r = this.Item(0); this.inv = Inv(); r = 1; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "assignment to 'inv'")) << r.diagnostics;
}

TEST(ReferenceTranslation, RefToAStructFieldThroughARefLocalAllowsReplacingTheStruct)
{
    const hotc::TranslateResult r = translate_text(
        "struct S {\n    int x = 0;\n}\nclass A {\n    S s;\n"
        "    void T() { ref t = this.s; ref r = t.x; this.s = S(); r = 1; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// The flags of the object do not go to the roots of the arguments.
TEST(ReferenceTranslation, RefArgumentOfARefCallKeepsItsOwnFlags)
{
    const char* const prelude = "class A {\n    int n = 0;\n    ref int Id(ref int a) { return a; }\n";
    for (const char* body : {"int x = 0; ref r = this.Id(x); x = 5; r = 1;", "ref r = this.Id(this.n); this.n = 3; r = 1;"})
    {
        const hotc::TranslateResult r = translate_text(std::string(prelude) + "    void T() { " + body + " }\n}\n");
        EXPECT_TRUE(r.ok) << body << r.diagnostics;
    }
}

TEST(ReferenceTranslation, RefInALambdaOfAFieldInitializerIsChecked)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    std.function<void()> f = () => { ref r = xs[0]; xs.push_back(1); r = 1; };\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "can move or remove the elements of")) << r.diagnostics;
}
