// Tests for sppc::translate with the reload hooks: onReload(OldState) and convert_<field>.
#include "hot/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

sppc::TranslateResult translate_class(const std::string& members)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    return sppc::translate(temp_options(dir, "using \"w.h\";\nclass Foo : Widget {\n" + members + "\n}\n"));
}

void expect_error(const std::string& members, const std::string& msg)
{
    const sppc::TranslateResult r = translate_class(members);
    EXPECT_FALSE(r.ok) << members;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << members << "\n" << r.diagnostics;
}

} // namespace

TEST(ReloadHookTranslation, OnReloadGetsNoSlotAndGoesToTheRecord)
{
    const sppc::TranslateResult r = translate_class(
        "    float hp = 1.0;\n"
        "    void onReload(OldState old) { if (old.has(\"x\")) this.hp = old.get<float>(\"x\"); }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("static void Foo__onReload(Foo_Shell* __self, const hot::OldState& old)"), std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("hot::deref(old).has(\"x\")"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("hot::deref(old).get<float>(\"x\")"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("    Foo__onReload_hook,\n    &Foo__construct"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("SLOT_onReload"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("__thunk"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.slots_out.find("onReload"), std::string::npos) << r.slots_out;
}

TEST(ReloadHookTranslation, AClassWithoutHooksHasNoHookMembers)
{
    const sppc::TranslateResult r = translate_class("    float hp = 1.0;");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("onReload"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("_hook"), std::string::npos) << r.cpp;
}

TEST(ReloadHookTranslation, OnReloadNeedsTheOldStateParameter)
{
    expect_error("    void onReload() { }", "'onReload' takes one parameter: 'OldState old'");
    expect_error("    void onReload(int old) { }", "'onReload' takes one parameter: 'OldState old'");
    expect_error("    int onReload(OldState old) { return 1; }", "'onReload' must return void");
}

TEST(ReloadHookTranslation, ConvertHookGoesToTheFieldRow)
{
    const sppc::TranslateResult r = translate_class(
        "    float level = 1.0;\n    int other = 0;\n"
        "    float convert_level(int old) { return old * 2.0; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("static float Foo__convert_level(int32_t old)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("typeid(int32_t).name(), Foo__convert_level_hook},"), std::string::npos) << r.cpp;
    // The convert function has no state: it never reads "st".
    const size_t at = r.cpp.find("static float Foo__convert_level(int32_t old)\n");
    ASSERT_NE(at, std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.substr(at, r.cpp.find("\n}\n", at) - at).find("__st->"), std::string::npos) << r.cpp;
    // The field "other" has no convert hook.
    EXPECT_NE(r.cpp.find("hot::field_destroyer<int32_t>()},"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("SLOT_convert_level"), std::string::npos) << r.cpp;
}

TEST(ReloadHookTranslation, ConvertHookChecks)
{
    expect_error("    float level = 1.0;\n    int convert_level(int old) { return 1; }",
        "'convert_level' must return the type of the field");
    expect_error("    float level = 1.0;\n    float convert_level() { return 1.0; }",
        "'convert_level' is a convert hook. It takes one parameter");
    expect_error("    float level = 1.0;\n    void convert_level(int old) { }", "It must return the new field type");
    expect_error(
        "    float level = 1.0;\n    float convert_level(int old) { return this.level; }", "'this' is not allowed");
}

TEST(ReloadHookTranslation, AFunctionWithAConvertNameButNoFieldIsAnOrdinaryFunction)
{
    const sppc::TranslateResult r =
        translate_class("    float level = 1.0;\n    float convert_other(int old) { return 1.0; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("SLOT_convert_other"), std::string::npos) << r.cpp;
}
