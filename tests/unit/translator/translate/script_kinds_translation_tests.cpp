// Tests for sppc::translate with the kinds of script classes: shell class, plain class, [value] class.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

const char* kHeader =
    "#include <string>\n"
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

void expect_error(const std::string& body, const std::string& msg)
{
    const sppc::TranslateResult r = translate_script(body);
    EXPECT_FALSE(r.ok) << body;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << body << "\n" << r.diagnostics;
}

bool has(const sppc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

const char* kStats =
    "[value] class Stats {\n"
    "    float hp = 1.0;\n"
    "    Stats(float h) { this.hp = h; }\n"
    "    float Twice() { return this.hp * 2.0; }\n"
    "}\n";

}  // namespace

TEST(ScriptKindsTranslation, NewOfAShellClassGivesAHandle)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit { int size = 5; Squad(int n) { this.size = n; } }\n"
        "class Boss : Unit {\n"
        "    Squad kid;\n"
        "    override void Update(float dt) { this.kid = new Squad(3); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "std::shared_ptr<Squad> kid")) << r.cpp;
    EXPECT_TRUE(has(r, "std::shared_ptr<Squad>(Squad__new(3))")) << r.cpp;
}

TEST(ScriptKindsTranslation, PlainClassUsesThePlainObjectBase)
{
    const sppc::TranslateResult r = translate_script(
        "class Counter { int count = 0; Counter(int c) { this.count = c; } }\n"
        "class Boss : Unit {\n"
        "    Counter c;\n"
        "    override void Update(float dt) { this.c = new Counter(1); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "spp::PlainObject")) << r.cpp;
    EXPECT_TRUE(has(r, "std::shared_ptr<Counter>(Counter__new(1))")) << r.cpp;
}

TEST(ScriptKindsTranslation, ValueClassIsAStructWithCopySemantics)
{
    const sppc::TranslateResult r = translate_script(
        std::string(kStats) +
        "class Boss : Unit {\n"
        "    Stats s = Stats(2.0);\n"
        "    override void Update(float dt) { Stats t = this.s; var u = new Stats(3.0); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "struct Stats")) << r.cpp;
    EXPECT_TRUE(has(r, "Stats(3.0f)")) << r.cpp;
    EXPECT_FALSE(has(r, "Stats__new")) << r.cpp;
    EXPECT_FALSE(has(r, "shared_ptr<Stats>")) << r.cpp;
    EXPECT_TRUE(has(r, "Stats_layout")) << r.cpp;
}

TEST(ScriptKindsTranslation, ValueClassErrors)
{
    expect_error("[value] class V : Unit { int a = 0; }\n", "a [value] class 'V' has no base class");
    expect_error("[value] class V { int a = 0; ~V() { } }\n", "cannot have a destructor");
    expect_error("[value] class V { int a = 0; virtual void F() { } }\n", "has no virtual methods");
    expect_error("[value] class V { int a = 0; void onReload() { } }\n", "cannot have the hook 'onReload'");
}

TEST(ScriptKindsTranslation, SuperNeedsABase)
{
    expect_error("class P { int a = 0; P(int x) : super(x) { } }\n", "has no base class");
}

TEST(ScriptKindsTranslation, ClassOrderIsChecked)
{
    // A [value] class is emitted first, so a later [value] class is fine.
    const sppc::TranslateResult r =
        translate_script("class Boss : Unit { Stats s; }\n[value] class Stats { int a = 0; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptKindsTranslation, HandleClassCanBeUsedBeforeItsDeclaration)
{
    // The translator sorts the classes by use. Squad is emitted before Boss.
    const sppc::TranslateResult r =
        translate_script("class Boss : Unit { Squad s; }\nclass Squad : Unit { int a = 0; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct Squad_Shell"), r.cpp.find("struct Boss_Shell")) << r.cpp;
}

TEST(ScriptKindsTranslation, ClassesThatUseEachOtherInACycleAreAnError)
{
    expect_error("class A : Unit { B b; }\nclass B : Unit { A a; }\n",
                 "the classes use each other in a cycle");
}

TEST(ScriptKindsTranslation, ValueClassCannotUseAHandleClass)
{
    expect_error("[value] class V { Squad s; }\nclass Squad : Unit { int a = 0; }\n",
                 "a [value] class can use only native types and [value] classes");
}
