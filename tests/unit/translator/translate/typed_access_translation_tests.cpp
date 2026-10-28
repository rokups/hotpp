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
        "[native, value] class Blob;\nclass Box : Unit {\n    int Size(Blob b) { return b.Get(); }\n    int Peek(Blob b) { return 1; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // A member call counts as a possible change (a limit of the analysis).
    EXPECT_TRUE(has(r, "static int32_t Box__Size(Box_Shell* self, Blob b)")) << r.cpp;
    EXPECT_TRUE(has(r, "static int32_t Box__Peek(Box_Shell* self, const Blob& b)")) << r.cpp;
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
