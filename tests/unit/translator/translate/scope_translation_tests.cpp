// Tests for sppc::translate with scopes. The script writes "." for a scope.
// The generated C++ uses "::".
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kScopeHeader =
    "#include <string>\n#include <vector>\n"
    "namespace Engine { class Actor { public: virtual ~Actor() {} virtual void Tick(float dt) {} }; }\n";

} // namespace

TEST(ScopeTranslation, FieldTypesAndBaseClassUseDoubleColons)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kScopeHeader);
    const std::string script =
        "using \"w.h\";\nclass P : Engine.Actor {\n    std.string title;\n"
        "    std.vector<int> history;\n    override void Tick(float dt) { }\n}\n";
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("std::string title"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::vector<int32_t> history"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Engine::Actor"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Engine.Actor"), std::string::npos) << r.cpp;
}

TEST(ScopeTranslation, MemberAccessUsesDeref)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kScopeHeader);
    const std::string script =
        "using \"w.h\";\nclass P : Engine.Actor {\n    std.string title;\n"
        "    override void Tick(float dt) { var n = this.title.size(); }\n}\n";
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("spp::deref(__st->title).size()"), std::string::npos) << r.cpp;
}

TEST(ScopeTranslation, DoubleColonInABaseClassIsAnError)
{
    const sppc::TranslateResult r = translate_text("class P : Engine::Actor {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("use '.' instead of '::'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:1:"), std::string::npos) << r.diagnostics;
}

TEST(ScopeTranslation, DoubleColonInAFieldTypeIsAnError)
{
    const sppc::TranslateResult r = translate_text("class P : B {\n    std::string title;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("use '.' instead of '::'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:2:"), std::string::npos) << r.diagnostics;
}

TEST(ScopeTranslation, ArrowInABodyIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, lambda_script("        p->foo();")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("use '.' instead of '->'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:5:"), std::string::npos) << r.diagnostics;
}
