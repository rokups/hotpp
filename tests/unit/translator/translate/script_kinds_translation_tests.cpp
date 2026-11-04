// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the kinds of script classes: shell class, plain class, class.
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
        "    $Squad kid;\n"
        "    override void Update(float dt) { this.kid = new $Squad(3); }\n"
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
        "    $Counter c;\n"
        "    override void Update(float dt) { this.c = new $Counter(1); }\n"
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
    // A class is emitted first, so a later class is fine.
    const hotc::TranslateResult r = translate_script("class Boss : Unit { Stats s; }\nstruct Stats { int a = 0; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptKindsTranslation, HandleClassCanBeUsedBeforeItsDeclaration)
{
    // The translator sorts the classes by use. Squad is emitted before Boss.
    const hotc::TranslateResult r =
        translate_script("class Boss : Unit { $Squad s; }\nclass Squad : Unit { int a = 0; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct Squad_Shell final"), r.cpp.find("struct Boss_Shell final")) << r.cpp;
}

TEST(ScriptKindsTranslation, ClassesCanUseEachOtherThroughHandles)
{
    // The forward block declares both shells. The function bodies come after the last block.
    const hotc::TranslateResult r = translate_script(
        "class A : Unit { $B b; int F() { $B x = b; if (x == null) return 0; return x.G(); } }\nclass B : Unit { $A a; int G() { return a != null ? 1 : 0; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct B_Shell;"), r.cpp.find("struct A_Shell final")) << r.cpp;
    EXPECT_LT(r.cpp.find("struct B_Shell final"), r.cpp.find("int32_t AState_g1::F(")) << r.cpp;
}

TEST(ScriptKindsTranslation, FieldInitializerNewOrdersTheCycle)
{
    // A "new" in a field initializer needs the "new" function of B in the block of A. So B comes
    // first, also when B comes first in the file and uses A.
    const hotc::TranslateResult r =
        translate_script("class B : Unit { $A a = null; }\nclass A : Unit { $B b = new $B(); int x = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct B_Shell final"), r.cpp.find("struct A_Shell final")) << r.cpp;
}

TEST(ScriptKindsTranslation, FieldInitializerWithANewOfALaterClassIsAllowed)
{
    // The translator emits B first. A member read of the new object works too.
    const hotc::TranslateResult r = translate_script(
        "class A : Unit { $B b = new $B(); int k = (new $B()).v; }\nclass B : Unit { int v = 1; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct B_Shell final"), r.cpp.find("struct A_Shell final")) << r.cpp;
}

TEST(ScriptKindsTranslation, FieldInitializerWithANewOfAnEarlierClassIsAllowed)
{
    const hotc::TranslateResult r =
        translate_script("class B : Unit { int v = 1; }\nclass A : Unit { $B b = new $B(); }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptKindsTranslation, FieldInitializersThatBuildEachOtherAreAnError)
{
    // Each class needs the "new" function of the other class in its own block: no order works.
    expect_error("class A : Unit { int k = (new $B()).v; }\nclass B : Unit { int v = (new $A()).k; }\n",
        "class 'B' must come before the class that builds it with 'new'");
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

TEST(ScriptKindsTranslation, TemplateArgumentThatIsAScriptClassUsesTheHandle)
{
    // A script class as a template argument is a handle (task 48). A struct stays a value.
    const std::string h = "std::shared_ptr<Squad>";
    const hotc::TranslateResult field = translate_script(
        "class Squad : Unit { int a = 0; }\nclass Boss : Unit { std.vector<$Squad> all; "
        "std.map<int, std.vector<$Squad>> groups; }\n");
    ASSERT_TRUE(field.ok) << field.diagnostics;
    EXPECT_NE(field.cpp.find("std::vector<" + h + "> all;"), std::string::npos) << field.cpp;
    EXPECT_NE(field.cpp.find("std::map<int32_t, std::vector<" + h + ">> groups;"), std::string::npos) << field.cpp;
    EXPECT_EQ(field.cpp.find("std::vector<Squad>"), std::string::npos) << field.cpp;
    const hotc::TranslateResult param = translate_script(
        "class Squad : Unit { int a = 0; }\nclass Boss : Unit { int F(std.vector<$Squad> v) { return v.size(); } }\n");
    ASSERT_TRUE(param.ok) << param.diagnostics;
    EXPECT_NE(param.cpp.find("std::vector<" + h + ">"), std::string::npos) << param.cpp;
    EXPECT_EQ(param.cpp.find("std::vector<Squad>"), std::string::npos) << param.cpp;
    const hotc::TranslateResult value =
        translate_script("struct Stats { int a = 0; }\nclass Boss : Unit { std.vector<Stats> all; }\n");
    ASSERT_TRUE(value.ok) << value.diagnostics;
    EXPECT_NE(value.cpp.find("std::vector<Stats> all;"), std::string::npos) << value.cpp;
}

TEST(ScriptKindsTranslation, ElementOfAContainerOfScriptClassesIsAHandle)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit { int a = 0; void Hit() { this.a = this.a + 1; } }\n"
        "class Boss : Unit {\n"
        "    std.vector<$Squad> all = std.vector<Squad>();\n"
        "    int sum = 0;\n"
        "    void Run() {\n"
        "        std.vector<$Squad> local;\n"
        "        local.push_back(new $Squad());\n"
        "        this.all.push_back(new $Squad());\n"
        "        this.all[0].Hit();\n"
        "        this.sum = this.all[0].a + local.front().a;\n"
        "        foreach (var s in this.all) { s.Hit(); this.sum += s.a; }\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The element is a handle: the access goes through the shell, with the null and bounds checks.
    EXPECT_NE(r.cpp.find("shell_of<Squad_Shell>(hot::index(__st->all, 0)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("state_of(hot::shell_of<Squad_Shell>(hot::index(__st->all, 0)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("shell_of<Squad_Shell>(s, \"Squad\")"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::vector<std::shared_ptr<Squad>>()"), std::string::npos) << r.cpp;
}

// The functions that name the stored type take the handle type, as the container does.
TEST(ScriptKindsTranslation, VariantAccessFunctionsNameTheHandleType)
{
    const hotc::TranslateResult r = translate_script(
        "using <variant>;\nclass Squad : Unit { int a = 0; }\n"
        "class Boss : Unit {\n"
        "    std.variant<$Squad, int> v;\n"
        "    bool Run() {\n"
        "        return std.holds_alternative<Squad>(this.v) && std.get<Squad>(this.v) != null;\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string h = "std::shared_ptr<Squad>";
    EXPECT_NE(r.cpp.find("std::variant<" + h + ", int32_t>"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("holds_alternative<" + h + ">"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("get<" + h + ">"), std::string::npos) << r.cpp;
}

TEST(ScriptKindsTranslation, FunctionTypeWithAScriptClassUsesTheHandle)
{
    const hotc::TranslateResult r = translate_script(
        "using <functional>;\nclass Squad : Unit { int a = 0; }\n"
        "class Boss : Unit {\n"
        "    std.function<void($Squad)> on_hit;\n"
        "    std.function<$Squad()> make;\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string h = "std::shared_ptr<Squad>";
    EXPECT_NE(r.cpp.find("std::function<void(" + h + ")> on_hit;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::function<" + h + "()> make;"), std::string::npos) << r.cpp;
}

// A cast "($C)x" to the handle of a script class is a cast of the handle. The bare "(C)x" is not a cast.
TEST(ScriptKindsTranslation, CastToAScriptHandle)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit { int a = 0; }\nclass Boss : Unit {\n    int Run($Unit u) {\n"
        "        $Squad s = ($Squad)u;\n        if (s == null) return 0;\n        return s.a;\n    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("std::shared_ptr<Squad> s = "), std::string::npos) << r.cpp;
    // A downcast is checked with the class record: a failed cast gives a null handle.
    EXPECT_NE(r.cpp.find("hot::as_script<Squad_Shell>(u, \"Squad\")"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("static_pointer_cast<Squad>"), std::string::npos) << r.cpp;
}

TEST(ScriptKindsTranslation, UpcastAndSameTypeCastAreStatic)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit { int a = 0; }\nclass Boss : Unit {\n    int Run($Squad s) {\n"
        "        $Unit u = ($Unit)s;\n        $Squad t = ($Squad)s;\n        return t.a;\n    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // A cast to the same type is a checked unwrap: a null handle throws hot::NullReference.
    // The base "Unit" is not a script class, so that upcast stays a static cast.
    EXPECT_NE(r.cpp.find("std::static_pointer_cast<Unit>(s)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("= hot::nonnull(s);"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("static_pointer_cast<Squad>"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("hot::as_script<Unit_Shell>(s"), std::string::npos) << r.cpp;
}

TEST(ScriptKindsTranslation, UpcastToAScriptBaseIsACheckedUnwrap)
{
    const hotc::TranslateResult r = translate_script(
        "class Base { int a = 0; }\nclass Mid : Base { int b = 0; }\nclass Boss {\n    int Run($Mid m) {\n"
        "        $Base u = ($Base)m;\n        $Mid d = ($Mid)u;\n        return u.a;\n    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("std::static_pointer_cast<Base>(hot::nonnull(m))"), std::string::npos) << r.cpp;
    // A downcast keeps hot::as_script: a failed cast gives null.
    EXPECT_NE(r.cpp.find("hot::as_script<Mid_Shell>(u, \"Mid\")"), std::string::npos) << r.cpp;
}

// A loop over a map gives pairs: the variable has no script type. The index of a map gives the mapped type.
TEST(ScriptKindsTranslation, LoopOverAMapDoesNotGiveTheMappedType)
{
    const hotc::TranslateResult r = translate_script(
        "using <map>;\nclass Squad : Unit { int a = 0; }\n"
        "class Boss : Unit {\n"
        "    std.map<int, $Squad> m;\n"
        "    int sum = 0;\n"
        "    void Run() {\n"
        "        this.sum = this.m[1].a;\n"
        "        foreach (var p in this.m) { this.sum += p.first; }\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("shell_of<Squad_Shell>(hot::index(__st->m, 1)"), std::string::npos) << r.cpp;
    // The loop variable "p" is not a Squad: no shell_of on it.
    EXPECT_EQ(r.cpp.find("shell_of<Squad_Shell>(p,"), std::string::npos) << r.cpp;
}

TEST(ScriptKindsTranslation, ClassesThatDeriveFromEachOtherAreAnError)
{
    expect_error("class A : B { int x = 0; }\nclass B : A { int y = 0; }\n", "derive from each other");
}

TEST(ScriptKindsTranslation, ValueClassCannotUseAHandleClass)
{
    expect_error(
        "struct V { $Squad s; }\nclass Squad : Unit { int a = 0; }\n", "a struct can use only native types and structs");
}

TEST(ScriptKindsTranslation, ScriptClassCallWithoutNewIsAnError)
{
    const std::string hint = "'Helper' is a script class. Create it with 'new Helper()'.";
    const std::string cls = "class Helper : Unit { int n = 0; }\n";
    expect_error(cls + "class User : Unit { void run() { $Helper h = Helper(); } }\n", hint);
    expect_error(cls + "class User : Unit { void run() { var h = Helper(); } }\n", hint);
    expect_error(cls + "class User : Unit { void run() { $Helper h = Helper(3); } }\n", hint);
    expect_error(cls + "class User : Unit { $Helper h = Helper(); }\n", hint);
}

TEST(ScriptKindsTranslation, ScriptClassWithNewAndStructCallStayValid)
{
    const hotc::TranslateResult r = translate_script(
        "struct P { int a = 0; }\nclass Helper : Unit { int n = 0; }\n"
        "class User : Unit { $Helper g; void run() { $Helper h = new $Helper(); P p = P(); } }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A ".cpp" field with a declarator in parentheses (a pointer to an array, a function pointer, a
// member pointer) goes through an alias: "int (*) p[3];" is not C++. A std::atomic field builds in place.
TEST(ScriptKindsTranslation, CppFieldDeclaratorsAndInPlaceFields)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "#include \"u.h\"\n#include <atomic>\n"
        "class Q : public Unit { public:\n"
        "    int (*p)[3] = nullptr;\n    void (*cb)(int) = nullptr;\n    int (Q::*mp)(int) = nullptr;\n"
        "    std::atomic<int> n = {0};\n};\n");
    o.script_name = "t.cpp";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "using p__array_t = int (*)[3]; p__array_t p;")) << r.cpp;
    EXPECT_TRUE(has(r, "using cb__array_t = void (*)(int); cb__array_t cb;")) << r.cpp;
    EXPECT_TRUE(has(r, "using mp__array_t = int (Q::*)(int); mp__array_t mp;")) << r.cpp;
    EXPECT_FALSE(has(r, "int (*) p[3]")) << r.cpp;
    // The field has no copy constructor, so the migration runs its initializer (kNotMovable).
    EXPECT_TRUE(has(r, "hot::move_field<std::atomic<int>>")) << r.cpp;
}

// A ".cpp" parameter with a declarator in parentheses gets the name inside the parentheses:
// "int (*)(int) op" is not C++. The size of the array in "int (*p)[3]" is not a default value.
TEST(ScriptKindsTranslation, CppParameterDeclaratorsKeepTheNameInside)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "#include \"u.h\"\n"
        "class Q : public Unit { public:\n"
        "    Q(int (*op)(int), int (*p)[3]) : n(op(p[0][1])) {}\n"
        "    int run(int (*f)(int), int (Q::*mp)(int), int (&r)[3]) { return f(r[1]); }\n"
        "    int n = 0;\n};\n");
    o.script_name = "t.cpp";
    for (const bool optimized : {false, true})
    {
        o.aot = true;
        o.aot_optimized = optimized;
        const hotc::TranslateResult r = hotc::translate(o);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        EXPECT_TRUE(has(r, "(int (*op)(int), int (*p)[3])")) << r.cpp;
        EXPECT_TRUE(has(r, "int (*f)(int), int (Q::*mp)(int), int (&r)[3]")) << r.cpp;
        EXPECT_FALSE(has(r, "(*)(int) op")) << r.cpp;
        EXPECT_FALSE(has(r, "(*)[3] p")) << r.cpp;
        EXPECT_FALSE(has(r, "(&)[3] r")) << r.cpp;
        EXPECT_FALSE(has(r, "(*p)[3] = 3")) << r.cpp;
        EXPECT_FALSE(has(r, "(int (*)[3]) = ")) << r.cpp;
    }
}
