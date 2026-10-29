// Tests for the reference types of the script: "T&" of a script class, reference locals
// ("var&"), the foreach with "var&", the hole of the borrowed field and the script text of
// the messages.
#include <gtest/gtest.h>

#include <string>

#include "builtin_types.h"
#include "spp/sppc_lib.h"
#include "support/check_translate_test_support.h"
#include "support/translator_test_support.h"

namespace {

using translator_test::translate_text;

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A script class Weapon and a class Hero with the given members.
std::string hero(const std::string& members)
{
    return "class Weapon {\n    int dmg = 1;\n}\nclass Hero {\n    Weapon wp;\n    int n = 0;\n" + members + "}\n";
}

}  // namespace

TEST(ReferenceTranslation, ScriptClassRefParameterIsAReferenceToTheHandle)
{
    const sppc::TranslateResult r = translate_text(hero("    void Make(Weapon& w) { w = new Weapon(); }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static void Hero__Make(Hero_Shell* __self, std::shared_ptr<Weapon>& w);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "void HeroState_g1::Make(std::shared_ptr<Weapon>& w)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon>& w)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Weapon& w")) << r.cpp;
}

TEST(ReferenceTranslation, ScriptClassRefParameterTakesAFieldAsTheArgument)
{
    const sppc::TranslateResult r =
        translate_text(hero("    void Make(Weapon& w) { w = new Weapon(); }\n    void Run() { Make(this.wp); }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Hero__Make(__self, __st->wp);")) << r.cpp;
}

TEST(ReferenceTranslation, ConstScriptClassRefParameterIsAConstHandleReference)
{
    const sppc::TranslateResult r = translate_text(hero("    void Look(const Weapon& w) { }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "void HeroState_g1::Look(const std::shared_ptr<Weapon>& w)")) << r.cpp;
}

TEST(ReferenceTranslation, MemberAccessThroughAScriptClassReferenceUsesTheClass)
{
    const std::string field = "    int A(Weapon w) { return w.dmg; }\n";
    const std::string ref = "    int B(Weapon& w) { return w.dmg; }\n";
    const std::string cref = "    int C(const Weapon& w) { return w.dmg; }\n";
    const sppc::TranslateResult r = translate_text(hero(field + ref + cref));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The three forms lower the access in the same way.
    EXPECT_FALSE(has(r.cpp, "w.dmg")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return spp::deref(w).dmg;") ||
                has(r.cpp, "spp::shell_of<Weapon_Shell>(w, \"Weapon\")"))
        << r.cpp;
}

TEST(ReferenceTranslation, ScriptClassRefLocalIsAReferenceToTheHandle)
{
    const sppc::TranslateResult r = translate_text(hero("    void Run() { Weapon& w = this.wp; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Weapon>& w = __st->wp;")) << r.cpp;
}

TEST(ReferenceTranslation, PointerToAScriptClassIsAnError)
{
    for (const char* members : {"    Weapon* p;\n", "    void Run(Weapon* p) { }\n", "    void Run() { Weapon* p = null; }\n",
                                "    Weapon* Get() { return null; }\n"}) {
        const sppc::TranslateResult r = translate_text(hero(members));
        EXPECT_FALSE(r.ok) << members;
        EXPECT_TRUE(has(r.diagnostics, "a pointer to the script class 'Weapon' is not allowed: use 'Weapon' (a handle)"))
            << members << r.diagnostics;
    }
}

TEST(ReferenceTranslation, VarRefLocalEmitsAutoRef)
{
    const sppc::TranslateResult r = translate_text(hero("    void Run() { var& r = this.n; r = 2; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto& r = ")) << r.cpp;
}

TEST(ReferenceTranslation, ConstVarRefLocalEmitsConstAutoRef)
{
    const sppc::TranslateResult r = translate_text(hero("    int Run() { const var& r = this.n; return r; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const auto& r = ")) << r.cpp;
}

TEST(ReferenceTranslation, PlainVarStillCopies)
{
    const sppc::TranslateResult r = translate_text(hero("    void Run() { var r = this.n; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto r = ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "auto& r")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachVarRefEmitsAutoRef)
{
    const sppc::TranslateResult r =
        translate_text("using <vector>;\nclass A {\n    std.vector<int> xs;\n"
                       "    void Run() { foreach (var& x in this.xs) { x = 1; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto& x : ")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachConstVarRefEmitsConstAutoRef)
{
    const sppc::TranslateResult r =
        translate_text("using <vector>;\nclass A {\n    std.vector<int> xs;\n    int n = 0;\n"
                       "    void Run() { foreach (const var& x in this.xs) { this.n = x; } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (const auto& x : ")) << r.cpp;
}

TEST(ReferenceTranslation, ForeachPlainVarCopies)
{
    const sppc::TranslateResult r =
        translate_text("using <vector>;\nclass A {\n    std.vector<int> xs;\n"
                       "    void Run() { foreach (var x in this.xs) { } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto x : ")) << r.cpp;
}

TEST(ReferenceTranslation, VarRefLocalIsABorrowedLocalForALambda)
{
    const sppc::TranslateResult r =
        translate_text(hero("    void Run() { var& r = this.n; var f = () => { r = 1; }; }\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'r', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(ReferenceTranslation, ForeachVarRefIsABorrowedLocalForALambda)
{
    const sppc::TranslateResult r = translate_text(
        "using <vector>;\nclass A {\n    std.vector<int> xs;\n"
        "    void Run() { foreach (var& x in this.xs) { var f = () => { x = 1; }; } }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'x', a borrowed pointer or reference")) << r.diagnostics;
}

// The messages speak script names.
TEST(ReferenceMessages, BorrowedFieldNamesIntNotInt32)
{
    const sppc::TranslateResult r = check_translate_test::check_translate("", "    int* p;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'int'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "Declare a stub with 'handle' or 'intrusive' for 'int'")) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "int32_t")) << r.diagnostics;
}

TEST(ReferenceMessages, BorrowedReturnValueNamesUlongNotUint64)
{
    const sppc::TranslateResult r = check_translate_test::check_translate("", "    ulong& Get() { return 0; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a return value cannot hold a borrowed pointer or reference to 'ulong'"))
        << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "uint64_t")) << r.diagnostics;
}

TEST(ReferenceMessages, TypeTextUsesScriptNames)
{
    EXPECT_EQ(sppc::script_type_text("int32_t&"), "int&");
    EXPECT_EQ(sppc::script_type_text("const uint8_t*"), "const byte*");
    EXPECT_EQ(sppc::script_type_text("std::vector<int64_t>"), "std::vector<long>");
    EXPECT_EQ(sppc::script_type_text("ns::int32_t"), "ns::int32_t");
}

// The hole of the borrowed field: "T*" of a native handle type is a borrowed pointer.
// The exception is the intrusive stub, where "T*" is the handle.
TEST(ReferenceHole, RawPointerOfANativeHandleFieldIsAFinding)
{
    const sppc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon* wp2;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(ReferenceHole, RawPointerOfANativeHandleIsAWarningByDefault)
{
    const sppc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon* wp2;\n", {}, false);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.warnings, "warning: a field cannot hold a borrowed pointer")) << r.warnings;
}

TEST(ReferenceHole, ReferenceFieldOfANativeHandleIsAFinding)
{
    const sppc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n", "    Weapon& w;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a field cannot hold a borrowed pointer or reference to 'Weapon'")) << r.diagnostics;
}

TEST(ReferenceHole, IntrusivePointerFieldIsTheHandle)
{
    const sppc::TranslateResult r = check_translate_test::check_translate(
        "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Counted;\n", "    Counted* c;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.warnings.empty()) << r.warnings;
}
