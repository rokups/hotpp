// Tests for the type-directed member access on script handles, "return this" as a handle and
// the parameter analysis of value parameters (design 7.10.1, 7.10.5).
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

const char* kHeader =
    "#include <string>\n"
    "struct Vec3 { float x, y, z; };\n"
    "struct Blob { std::string s; Blob() {} Blob(const Blob&) {} Blob(Blob&&) noexcept {} void Add(int) {} int Get() const { return 0; } };\n"
    "struct Rec { int N = 0; static Rec* Get() { return nullptr; } };\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "};\n";

sppc::TranslateResult translate_script(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return sppc::translate(temp_options(dir, "using \"u.h\";\nusing <string>;\n" + body));
}

bool has(const sppc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

const char* kTwoClasses =
    "class Squad : Unit {\n"
    "    int size = 3;\n"
    "    int Power { get { return this.size * 2; } set { this.size = value; } }\n"
    "    int Count() { return this.size; }\n"
    "    void Grow(int n) { this.size = this.size + n; }\n"
    "}\n";

}  // namespace

TEST(TypedAccess, FieldOfAScriptHandleUsesTheState)
{
    const sppc::TranslateResult r = translate_script(
        std::string(kTwoClasses) + "class Boss : Unit {\n    Squad s;\n    int Run() { return this.s.size; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "state_of(spp::shell_of<Squad_Shell>(st->s, \"Squad\"))->size")) << r.cpp;
}

TEST(TypedAccess, MethodOfAScriptHandleIsADirectCall)
{
    const sppc::TranslateResult r = translate_script(
        std::string(kTwoClasses) +
        "class Boss : Unit {\n    Squad s;\n    int Run(Squad other) { other.Grow(2); return this.s.Count(); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__Grow(spp::shell_of<Squad_Shell>(other, \"Squad\"), 2)")) << r.cpp;
    EXPECT_TRUE(has(r, "Squad__Count(spp::shell_of<Squad_Shell>(st->s, \"Squad\"))")) << r.cpp;
}

TEST(TypedAccess, PropertyOfAScriptHandleUsesItsAccessors)
{
    const sppc::TranslateResult r = translate_script(
        std::string(kTwoClasses) +
        "class Boss : Unit {\n    Squad s;\n    int Run() { this.s.Power = 4; return this.s.Power; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__set_Power(spp::shell_of<Squad_Shell>(st->s, \"Squad\"), 4)")) << r.cpp;
    EXPECT_TRUE(has(r, "Squad__get_Power(spp::shell_of<Squad_Shell>(st->s, \"Squad\"))")) << r.cpp;
}

TEST(TypedAccess, LocalFromNewHasTheTypeOfTheClass)
{
    const sppc::TranslateResult r = translate_script(
        std::string(kTwoClasses) + "class Boss : Unit {\n    int Run() { var q = new Squad(); return q.size; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "state_of(spp::shell_of<Squad_Shell>(q, \"Squad\"))->size")) << r.cpp;
}

TEST(TypedAccess, ScriptFunctionUsedAsAValueIsAnError)
{
    const sppc::TranslateResult r = translate_script(
        std::string(kTwoClasses) + "class Boss : Unit {\n    Squad s;\n    int Run() { var f = this.s.Count; return 0; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("script function 'Count' must be called"), std::string::npos) << r.diagnostics;
}

TEST(TypedAccess, ReturnThisAsAHandleUsesSharedFromThis)
{
    const sppc::TranslateResult r = translate_script(
        "class Node : Unit {\n    Node Self() { return this; }\n}\n"
        "class Plain {\n    Plain Me() { return this; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "return spp::handle_from_this<std::shared_ptr<Node>>(self);")) << r.cpp;
    EXPECT_TRUE(has(r, "return spp::handle_from_this<std::shared_ptr<Plain>>(self);")) << r.cpp;
    EXPECT_TRUE(has(r, "spp::ShellBase, std::enable_shared_from_this<Node_Shell> {")) << r.cpp;
}

TEST(TypedAccess, BareThisAsAPointerNeedsNoSharedFromThis)
{
    const sppc::TranslateResult r =
        translate_script("class Node : Unit {\n    int Id() { return this.Hash(); }\n    int Hash() { return 1; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r, "enable_shared_from_this")) << r.cpp;
}

// ------------------------------------------------------------------ parameter analysis
TEST(ParamAnalysis, ReadOnlyValueParamIsAConstReference)
{
    const sppc::TranslateResult r = translate_script(
        "[native, value] class Blob;\nclass Box : Unit {\n    int Size(Blob b) { return b.Get(); }\n    int Peek(Blob b) { return 1; }\n"
        "    void Grow(Blob b) { b.Add(1); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // A call of a const native member function is not a change (S20b). Libclang gives the constness.
    EXPECT_TRUE(has(r, "static int32_t Box__Size(Box_Shell* self, const Blob& b)")) << r.cpp;
    EXPECT_TRUE(has(r, "static int32_t Box__Peek(Box_Shell* self, const Blob& b)")) << r.cpp;
    // A non-const member function can change the object: the body gets a copy.
    EXPECT_TRUE(has(r, "static void Box__Grow(Box_Shell* self, Blob b)")) << r.cpp;
}

TEST(ParamAnalysis, StoredValueParamIsACopy)
{
    const sppc::TranslateResult r = translate_script(
        "[native, value] class Blob;\nclass Box : Unit {\n    Blob kept;\n    void Keep(Blob b) { this.kept = b; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Box__Keep(Box_Shell* self, Blob b)")) << r.cpp;
}

TEST(ParamAnalysis, SmallPlainValueIsPassedByValue)
{
    const sppc::TranslateResult r =
        translate_script("[native, value] class Vec3;\nclass Box : Unit {\n    float Len(Vec3 v) { return 1.0; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static float Box__Len(Box_Shell* self, Vec3 v)")) << r.cpp;
}

TEST(ParamAnalysis, SlotAdapterKeepsTheCanonicalForm)
{
    const sppc::TranslateResult r = translate_script(
        "[native, value] class Blob;\nclass Box : Unit {\n    int Peek(Blob b) { return 1; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static int32_t Box__Peek_slot(Unit* a, Blob b)")) << r.cpp;
}

// S20b: the analysis follows a parameter into the script functions that get it by value.
TEST(ParamAnalysis, ParamPassedToAScriptFunctionGetsItsUse)
{
    const sppc::TranslateResult r = translate_script(
        "[native, value] class Blob;\nclass Box : Unit {\n    void Change(Blob b) { b.Add(1); }\n"
        "    int Read(Blob b) { return b.Get(); }\n    void PassChange(Blob b) { this.Change(b); }\n"
        "    void PassChangeBare(Blob b) { Change(b); }\n    int PassRead(Blob b) { return this.Read(b); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Box__PassChange(Box_Shell* self, Blob b)")) << r.cpp;
    EXPECT_TRUE(has(r, "static void Box__PassChangeBare(Box_Shell* self, Blob b)")) << r.cpp;
    EXPECT_TRUE(has(r, "static int32_t Box__PassRead(Box_Shell* self, const Blob& b)")) << r.cpp;
}

TEST(ParamAnalysis, ParamPassedToAStoringScriptFunctionIsACopy)
{
    const sppc::TranslateResult r = translate_script(
        "[native, value] class Blob;\nclass Box : Unit {\n    Blob kept;\n    void Keep(Blob b) { this.kept = b; }\n"
        "    void Pass(Blob b) { this.Keep(b); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Box__Pass(Box_Shell* self, Blob b)")) << r.cpp;
}

// A "const T&" can alias a field of the class. When the function writes such a field, it takes a copy.
TEST(ParamAnalysis, ParamThatCanAliasAWrittenFieldIsACopy)
{
    const sppc::TranslateResult r = translate_script(
        "[native, value] class Blob;\nclass Box : Unit {\n    Blob cur;\n    int hp = 0;\n"
        "    int Swap(Blob b) { this.cur.Add(1); return b.Get(); }\n"
        "    int Hit(Blob b) { this.hp = 2; return b.Get(); }\n"
        "    void Inner() { this.cur.Add(2); }\n    int Deep(Blob b) { this.Inner(); return b.Get(); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static int32_t Box__Swap(Box_Shell* self, Blob b)")) << r.cpp;
    EXPECT_TRUE(has(r, "static int32_t Box__Hit(Box_Shell* self, const Blob& b)")) << r.cpp;
    EXPECT_TRUE(has(r, "static int32_t Box__Deep(Box_Shell* self, Blob b)")) << r.cpp;
}

TEST(ParamAnalysis, HandleParamThatCanAliasAWrittenFieldIsACopy)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit {\n    int size = 3;\n}\nclass Box : Unit {\n    Squad lead;\n"
        "    int Set(Squad s) { this.lead = new Squad(); return s.size; }\n    int Get(Squad s) { return s.size; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static int32_t Box__Set(Box_Shell* self, std::shared_ptr<Squad> s)")) << r.cpp;
    EXPECT_TRUE(has(r, "static int32_t Box__Get(Box_Shell* self, const std::shared_ptr<Squad>& s)")) << r.cpp;
}

// S20b: "obj.P" on a known native type is the native member, not a property of the class.
TEST(TypedAccess, PropertyNameOnAKnownNativeTypeIsTheNativeMember)
{
    const sppc::TranslateResult r = translate_script(
        "class Box : Unit {\n    int N { get; set; }\n"
        "    int Run(Rec& r) { r.N = 2; var q = Rec.Get(); q.N = 3; return r.N + q.N; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "spp::deref(r).N = 2;")) << r.cpp;
    EXPECT_TRUE(has(r, "spp::deref(q).N = 3;")) << r.cpp;
    EXPECT_FALSE(has(r, "shell_of")) << r.cpp;
}
