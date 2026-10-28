// Tests for spc::translate with a handle stub on a base class: intrusive and shared handles.
#include <gtest/gtest.h>

#include <string>

#include "spp/spc_lib.h"
#include "support/translator_test_support.h"
#include "support/stub_translate_test_support.h"

using namespace translator_test;
using namespace stub_translate_test;

TEST(InheritedHandleTranslation, IntrusiveField)
{
    const spc::TranslateResult r = hier_translate(kIntrusiveStub, "    Item item;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    " + kIntr + " item;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "using T = " + kIntr + "; new (f) T();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "typeid(" + kIntr + ").name()")) << r.cpp;
}

TEST(InheritedHandleTranslation, OwnerIsTheStubClass)
{
    const spc::TranslateResult r =
        hier_translate(kIntrusiveStub, "    Gadget g;\n    eng.Thing t;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    script::Intrusive<Gadget, &Object::AddRef, &Object::Release> g;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "    script::Intrusive<eng::Thing, &Object::AddRef, &Object::Release> t;\n"))
        << r.cpp;
}

TEST(InheritedHandleTranslation, ParamReturnLocalAndNew)
{
    const spc::TranslateResult r = hier_translate(
        kIntrusiveStub,
        "    Item item;\n"
        "    void Put(Item i) { this.item = i; }\n"
        "    Item Get() { Item x = this.item; return x; }\n"
        "    void Make() { this.item = new Item(3); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const " + kIntr + "& i")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static " + kIntr + " Foo__Get(Foo_Shell* self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, kIntr + " x = st->item;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "st->item = " + kIntr + "(new Item(3));")) << r.cpp;
}

TEST(InheritedHandleTranslation, SharedPtrBase)
{
    const spc::TranslateResult r = hier_translate(kSharedStub, "    Item item;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Item> item;\n")) << r.cpp;
}

TEST(InheritedHandleTranslation, ExactStubWins)
{
    const std::string stubs =
        std::string(kIntrusiveStub) + "[native, handle(\"std::shared_ptr<$T>\")] class Item;\n";
    const spc::TranslateResult r = hier_translate(stubs, "    Item item;\n    Gadget g;\n    Object o;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Item> item;\n")) << r.cpp;
    // Gadget has no stub: the nearest base with a stub is Item.
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Gadget> g;\n")) << r.cpp;
}

TEST(InheritedHandleTranslation, ValueBaseIsNotInherited)
{
    const spc::TranslateResult r =
        hier_translate("[native, value] class Object;\n", "    Item item;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    Item item;\n")) << r.cpp;
}

TEST(InheritedHandleTranslation, PreludeGivesTheSameOutput)
{
    const std::string members = "    Item item;\n    eng.Thing t;\n    void Put(Item i) { this.item = i; }\n";
    const spc::TranslateResult a = hier_translate(kIntrusiveStub, members);
    const spc::TranslateResult b = hier_translate(kIntrusiveStub, members, true);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_TRUE(has(b.cpp, "    " + kIntr + " item;\n")) << b.cpp;
    EXPECT_TRUE(has(b.cpp, "script::Intrusive<eng::Thing, &Object::AddRef, &Object::Release> t;")) << b.cpp;
    EXPECT_TRUE(has(b.cpp, "const " + kIntr + "& i")) << b.cpp;
}
