// Tests for sppc::translate with script inheritance (S6a, design 4.5, 7.6 and 7.7): a child of a
// script class, script virtuals, "override" and "new" of a parent function, "base" and "is"/"as".
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

// A plain parent A with a virtual f and a child B. "members" is the body of B.
std::string family(const std::string& members, const std::string& parent = "")
{
    return "class A {\n    int hp = 1;\n    virtual int f() { return 1; }\n    int g() { return f() + 1; }\n" + parent
        + "}\nclass B : A {\n" + members + "\n}\n";
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(InheritanceTranslation, ChildShellAndStateDeriveFromTheParent)
{
    const sppc::TranslateResult r = translate_text(family("    int rage = 2;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct A_Shell : spp::PlainObject, spp::ShellBase {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct B_Shell final : A_Shell {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct BState_g1 : AState_g1 {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "&B_layout, &A_record,")) << r.cpp;
    // The field table of B has the parent field first.
    EXPECT_TRUE(has(r.cpp, "{\"hp\", nullptr, typeid(int32_t).name(), offsetof(BState_g1, hp)")) << r.cpp;
}

TEST(InheritanceTranslation, VirtualCallUsesTheSlotOfTheObject)
{
    const sppc::TranslateResult r = translate_text(family("    override int f() { return 2; }"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return A__f_virt(__self) + 1;")) << r.cpp;
    // The override fills the slot of A.f in the table of B. It adds no manifest entry.
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<spp::SlotFn>(&BState_g1::f),")) << r.cpp;
    EXPECT_FALSE(has(r.slots_out, "B::f")) << r.slots_out;
    // B inherits A.g: the slot of A.g in the table of B is the one of A.
    EXPECT_TRUE(has(r.cpp, "reinterpret_cast<spp::SlotFn>(&AState_g1::g),")) << r.cpp;
}

TEST(InheritanceTranslation, SuperCallsTheParentImplementation)
{
    const sppc::TranslateResult r = translate_text(family("    override int f() { return base.f() + base.g(); }"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return A__f(__self) + A__g(__self);")) << r.cpp;
}

TEST(InheritanceTranslation, NewHidesAParentFunction)
{
    const sppc::TranslateResult r = translate_text(family("    new int g() { return 5; }"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.slots_out, "B::g 1")) << r.slots_out;
}

TEST(InheritanceTranslation, ParentConstructorRunsFirst)
{
    const sppc::TranslateResult r = translate_text(
        family("    B(int x) : base(x + 1) { this.hp = this.hp + x; }", "    A(int v) { this.hp = v; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The plan evaluates "x + 1" one time. The chain gives the value to the constructor of A.
    EXPECT_TRUE(has(r.cpp, "auto __up = std::make_tuple(x + 1);")) << r.cpp;
    EXPECT_EQ(r.cpp.find("x + 1"), r.cpp.rfind("x + 1")) << r.cpp;
    // A "#line" directive precedes each line of the chain. The parent runs before the own constructor.
    const size_t parent_first = r.cpp.find(
        "A__ctor_chain(__self, __chain.second, std::forward<decltype(x)>(x)...); }, std::move(__chain.first));\n");
    ASSERT_NE(parent_first, std::string::npos) << r.cpp;
    EXPECT_LT(parent_first, r.cpp.find("    B__ctor(__self, x);\n", parent_first)) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "B__ctor_chain(__self, __plan.second, x);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "&B__new_default")) << r.cpp;
}

TEST(InheritanceTranslation, Errors)
{
    EXPECT_TRUE(has(diag_of(family("    int g() { return 5; }")), "function 'g' hides the function of A; write 'new'"));
    EXPECT_TRUE(has(diag_of(family("    int f() { return 5; }")),
        "function 'f' hides the virtual function of A; write 'override' or 'new'"));
    EXPECT_TRUE(
        has(diag_of(family("    override int g() { return 5; }")), "'override' has no virtual method to override"));
    EXPECT_TRUE(has(diag_of(family("    override int f(int k) { return k; }")), "has other parameter types"));
    EXPECT_TRUE(has(diag_of(family("    override float f() { return 1.0; }")), "return type 'float' differs from"));
    EXPECT_TRUE(has(diag_of(family("    int hp = 3;")), "field 'hp' hides a field of the script parent 'A'"));
    EXPECT_TRUE(has(diag_of(family("    B() { }", "    A(int v) { this.hp = v; }\n")),
        "class 'B' must call 'base(...)': the constructor of 'A' has parameters"));
    EXPECT_TRUE(has(diag_of(family("    B() : base(1, 2) { }", "    A(int v) { this.hp = v; }\n")),
        "'base(...)' has 2 arguments; the constructor of 'A' takes 1"));
    EXPECT_TRUE(
        has(diag_of("struct V { int x = 0; }\nclass W : V { }\n"), "class 'W' cannot derive from the struct 'V'"));
}

TEST(InheritanceTranslation, IsAndAsUseTheClassRecords)
{
    const sppc::TranslateResult r = translate_text(
        family("") + "class C {\n    A a = null;\n    int run() {\n        var b = this.a as B;\n"
                     "        return this.a is B ? b.hp : 0;\n    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto b = spp::as_script<B_Shell>(__st->a, \"B\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "spp::is_script(__st->a, \"B\")")) << r.cpp;
    EXPECT_TRUE(has(diag_of("struct V { int x = 0; }\nclass C {\n    bool t(V v) { return v is V; }\n}\n"),
        "'is' needs a class with a handle: 'V' is a struct"));
}

TEST(InheritanceTranslation, IsAndAsOnANativeTypeUseRtti)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir,
        "using \"w.h\";\nclass Foo : Widget {\n    bool t(Widget* w) { var p = w as Panel; return w is Panel; }\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto p = spp::as_native<Panel>(w);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return spp::is_native<Panel>(w);")) << r.cpp;
}
