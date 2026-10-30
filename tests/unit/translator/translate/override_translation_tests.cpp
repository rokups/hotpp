// Tests for sppc::translate with the override rules of the native virtual methods.
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

TEST(OverrideTranslation, ExplicitOverrideUsesTheVirtualSlot)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // An override gets no entry in the slot file. A plain function gets one.
    EXPECT_EQ(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;
    // The state member is the slot. No wrapper frame exists.
    EXPECT_NE(r.cpp.find("void FooState_g1::Update(float dt)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("reinterpret_cast<spp::SlotFn>(&FooState_g1::Update)"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Foo__Update"), std::string::npos) << r.cpp;
}

TEST(OverrideTranslation, MissingOverrideIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Update(float dt)")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(
        r.diagnostics.find("method 'Update' overrides a virtual method of Widget; write 'override'"), std::string::npos)
        << r.diagnostics;
}

TEST(OverrideTranslation, OverrideWithoutBaseMethodIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Nothing()", "override ")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'override' has no virtual method to override"), std::string::npos) << r.diagnostics;
}

TEST(OverrideTranslation, NewOnAVirtualMethodIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Update(float dt)", "new ")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'new' cannot hide a virtual method; write 'override'"), std::string::npos)
        << r.diagnostics;
}

TEST(OverrideTranslation, ScriptOnlyMethodNeedsNoModifier)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Extra()")));
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(OverrideTranslation, SameNameWithOtherParametersHidesTheVirtualMethod)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Update(int n)")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("function 'Update' hides the virtual method of Widget. "
                                 "Candidates: "),
        std::string::npos)
        << r.diagnostics;
}

TEST(OverrideTranslation, FinalMethodStaysAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} virtual void Lock() final {} };\n");
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Lock()", "override ")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("is final and cannot be overridden"), std::string::npos) << r.diagnostics;
}

TEST(OverrideTranslation, FunctionChecksUseTheWordFunction)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string head = "using \"w.h\";\nclass Foo : Widget {\n";
    sppc::TranslateResult r = sppc::translate(temp_options(dir, head + "    void f() { }\n    void f() { }\n}\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("function 'f' is declared twice (overloads are not supported)"), std::string::npos)
        << r.diagnostics;
    r = sppc::translate(temp_options(dir, head + "    int f;\n    void f() { }\n}\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'f' is a field and a function"), std::string::npos) << r.diagnostics;
}
