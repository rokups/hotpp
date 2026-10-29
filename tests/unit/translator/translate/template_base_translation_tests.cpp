// Tests for sppc::translate with native bases that come from a class template (design risk 8,
// experiment F1). The script names a plain class. The class can derive from a template instance.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

const char* kTemplateHeader =
    "template <class T> class Tmpl { public: virtual ~Tmpl() {} virtual void Step(T v) {} };\n"
    "template <> class Tmpl<float> { public: virtual ~Tmpl() {} virtual void Step(float v) {} };\n"
    "class FloatBase : public Tmpl<float> {};\n"
    "class IntBase : public Tmpl<int> {};\n";

sppc::TranslateResult run(const std::string& classes)
{
    TempDir dir;
    write_file(dir.path() / "t.h", kTemplateHeader);
    return sppc::translate(temp_options(dir, "using \"t.h\";\n" + classes));
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

// An explicit specialization is a normal class for libclang. Its virtuals get slots.
TEST(TemplateBaseTranslation, ExplicitSpecializationBaseGivesSlots)
{
    const sppc::TranslateResult r = run("class Foo : FloatBase {\n    override void Step(float v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Step = 0,")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Foo_Shell final : FloatBase, spp::ShellBase {")) << r.cpp;
}

// Known limit (risk 8): a base that derives from an implicit instance of a class template gives
// no virtual to the translator. The translator reports the error and does not make a wrong shell.
// A fix needs the type declaration of the base specifier. Update this test when that is done.
TEST(TemplateBaseTranslation, ImplicitInstanceBaseIsAKnownLimit)
{
    const sppc::TranslateResult r = run("class Foo : IntBase {\n    override void Step(int v) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "error:")) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "SLOT_Step")) << r.cpp;
}
