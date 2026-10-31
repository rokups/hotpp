// Tests for hotc::translate with the kinds of script classes: shell class, plain class, [value] class.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kHeader =
    "#include <string>\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "};\n";

hotc::TranslateResult translate_script(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"u.h\";\nusing <string>;\n" + body));
}

void expect_error(const std::string& body, const std::string& msg)
{
    const hotc::TranslateResult r = translate_script(body);
    EXPECT_FALSE(r.ok) << body;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << body << "\n" << r.diagnostics;
}

bool has(const hotc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

const char* kStats =
    "struct Stats {\n"
    "    float hp = 1.0;\n"
    "    Stats(float h) { this.hp = h; }\n"
    "    float Twice() { return this.hp * 2.0; }\n"
    "}\n";

} // namespace

TEST(ScriptKindsTranslation, NewOfAShellClassGivesAHandle)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit { int size = 5; Squad(int n) { this.size = n; } }\n"
        "class Boss : Unit {\n"
        "    Squad kid;\n"
        "    override void Update(float dt) { this.kid = new Squad(3); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "std::shared_ptr<Squad> kid")) << r.cpp;
    EXPECT_TRUE(has(r, "std::shared_ptr<Squad>(Squad__new_shared(3))")) << r.cpp;
}

TEST(ScriptKindsTranslation, PlainClassUsesThePlainObjectBase)
{
    const hotc::TranslateResult r = translate_script(
        "class Counter { int count = 0; Counter(int c) { this.count = c; } }\n"
        "class Boss : Unit {\n"
        "    Counter c;\n"
        "    override void Update(float dt) { this.c = new Counter(1); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "hot::PlainObject")) << r.cpp;
    EXPECT_TRUE(has(r, "std::shared_ptr<Counter>(Counter__new_shared(1))")) << r.cpp;
}

TEST(ScriptKindsTranslation, ValueClassIsAStructWithCopySemantics)
{
    const hotc::TranslateResult r = translate_script(
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
    expect_error("struct V : Unit { int a = 0; }\n", "a struct 'V' has no base class");
    expect_error("struct V { int a = 0; ~V() { } }\n", "cannot have a destructor");
    expect_error("struct V { int a = 0; virtual void F() { } }\n", "has no virtual methods");
    expect_error("struct V { int a = 0; void onReload() { } }\n", "cannot have the hook 'onReload'");
}

TEST(ScriptKindsTranslation, SuperNeedsABase)
{
    expect_error("class P { int a = 0; P(int x) : base(x) { } }\n", "has no base class");
}

TEST(ScriptKindsTranslation, ClassOrderIsChecked)
{
    // A [value] class is emitted first, so a later [value] class is fine.
    const hotc::TranslateResult r = translate_script("class Boss : Unit { Stats s; }\nstruct Stats { int a = 0; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptKindsTranslation, HandleClassCanBeUsedBeforeItsDeclaration)
{
    // The translator sorts the classes by use. Squad is emitted before Boss.
    const hotc::TranslateResult r =
        translate_script("class Boss : Unit { Squad s; }\nclass Squad : Unit { int a = 0; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct Squad_Shell final"), r.cpp.find("struct Boss_Shell final")) << r.cpp;
}

TEST(ScriptKindsTranslation, ClassesCanUseEachOtherThroughHandles)
{
    // The forward block declares both shells. The function bodies come after the last block.
    const hotc::TranslateResult r = translate_script(
        "class A : Unit { B b; int F() { return b.G(); } }\nclass B : Unit { A a; int G() { return a != null ? 1 : 0; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct B_Shell;"), r.cpp.find("struct A_Shell final")) << r.cpp;
    EXPECT_LT(r.cpp.find("struct B_Shell final"), r.cpp.find("int32_t AState_g1::F(")) << r.cpp;
}

TEST(ScriptKindsTranslation, FieldInitializerNewOrdersTheCycle)
{
    // A "new" in a field initializer needs the "new" function of B in the block of A. So B comes
    // first, also when B comes first in the file and uses A.
    const hotc::TranslateResult r =
        translate_script("class B : Unit { A a = null; }\nclass A : Unit { B b = new B(); int x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct B_Shell final"), r.cpp.find("struct A_Shell final")) << r.cpp;
}

TEST(ScriptKindsTranslation, CppBodyDoesNotSeeLaterDeclarations)
{
    // A ".cpp" script writes the bodies of a block before the next text of the file. So the body
    // of A calls pick(long), as in C++. The overload pick(int) after the class is not visible.
    hotc::TranslateOptions o;
    o.script_name = "t.cpp";
    o.script_text = "#include \"Actor.h\"\n\nstatic int pick(long) { return 1; }\n\n"
                    "class A : public Actor\n{\npublic:\n    int n = 0;\n"
                    "    void Tick(float dt) override { n = pick(0); }\n};\n\n"
                    "static int pick(int) { return 2; }\n";
    o.generation = 1;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t body = r.cpp.find("void AState_g1::Tick(");
    ASSERT_NE(body, std::string::npos) << r.cpp;
    EXPECT_LT(r.cpp.find("static int pick(long)"), body) << r.cpp;
    EXPECT_LT(body, r.cpp.find("static int pick(int)")) << r.cpp;
}

TEST(ScriptKindsTranslation, StructsThatHoldEachOtherAreAnError)
{
    expect_error("struct A { B b; }\nstruct B { A a; }\n", "the structs use each other in a cycle: A -> B -> A");
}

TEST(ScriptKindsTranslation, TemplateArgumentThatIsAScriptClassIsAnError)
{
    // Task 48 adds the support. A struct is a value, so it is a good template argument.
    const std::string msg = "a template argument cannot be a script class yet: 'Squad'";
    expect_error("class Squad : Unit { int a = 0; }\nclass Boss : Unit { std.vector<Squad> all; }\n", msg);
    expect_error("class Squad : Unit { int a = 0; }\nclass Boss : Unit { void F(std.vector<Squad> v) { } }\n", msg);
    const hotc::TranslateResult r =
        translate_script("struct Stats { int a = 0; }\nclass Boss : Unit { std.vector<Stats> all; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptKindsTranslation, ClassesThatDeriveFromEachOtherAreAnError)
{
    expect_error("class A : B { int x = 0; }\nclass B : A { int y = 0; }\n", "derive from each other");
}

TEST(ScriptKindsTranslation, ValueClassCannotUseAHandleClass)
{
    expect_error(
        "struct V { Squad s; }\nclass Squad : Unit { int a = 0; }\n", "a struct can use only native types and structs");
}

TEST(ScriptKindsTranslation, ScriptClassCallWithoutNewIsAnError)
{
    const std::string hint = "'Helper' is a script class. Create it with 'new Helper()'.";
    const std::string cls = "class Helper : Unit { int n = 0; }\n";
    expect_error(cls + "class User : Unit { void run() { Helper h = Helper(); } }\n", hint);
    expect_error(cls + "class User : Unit { void run() { var h = Helper(); } }\n", hint);
    expect_error(cls + "class User : Unit { void run() { Helper h = Helper(3); } }\n", hint);
    expect_error(cls + "class User : Unit { Helper h = Helper(); }\n", hint);
}

TEST(ScriptKindsTranslation, ScriptClassWithNewAndStructCallStayValid)
{
    const hotc::TranslateResult r = translate_script(
        "struct P { int a = 0; }\nclass Helper : Unit { int n = 0; }\n"
        "class User : Unit { Helper g; void run() { Helper h = new Helper(); P p = P(); } }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
