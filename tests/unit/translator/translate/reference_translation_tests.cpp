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
        EXPECT_TRUE(has(r.diagnostics, "\"ref\" is only for local variables, foreach and parameters"))
            << members << r.diagnostics;
    }
    const hotc::TranslateResult m = translate_text("ref int g;\n");
    EXPECT_FALSE(m.ok);
    EXPECT_TRUE(has(m.diagnostics, "\"ref\" is only for local variables, foreach and parameters")) << m.diagnostics;
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

TEST(ReferenceTranslation, ForeachVarRefEmitsAutoRef)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (ref x in this.xs) { x = 1; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto& x : ")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachRefThatIsNotWrittenEmitsConstAutoRef)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n    int n = 0;\n"
        "    void Run() { foreach (ref x in this.xs) { this.n = x; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (const auto& x : ")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachPlainVarCopies)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (var x in this.xs) { } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto x : ")) << r.cpp;
}

TEST(ReferenceTranslation, VarRefLocalIsABorrowedLocalForALambda)
{
    const hotc::TranslateResult r =
        translate_text(hero("    void Run() { ref r = this.n; var f = () => { r = 1; }; }\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'r', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(ReferenceTranslation, ForeachVarRefIsABorrowedLocalForALambda)
{
    const hotc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (ref x in this.xs) { var f = () => { x = 1; }; } }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'x', a borrowed pointer or reference")) << r.diagnostics;
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
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon** wp2;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(ReferenceHole, RawPointerOfANativeHandleIsAWarningByDefault)
{
    const hotc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon** wp2;\n", {}, false);
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
