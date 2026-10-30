// Tests for sppc::translate with the [alias] attribute of a field.
#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

TEST(AliasTranslation, AliasGoesToTheReflectionRow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const sppc::TranslateResult r = sppc::translate(temp_options(dir,
        "using \"w.h\";\nclass Foo : Widget {\n"
        "    [alias(\"old\")] int n = 0;\n    int m = 0;\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("{\"n\", \"old\","), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("{\"m\", nullptr,"), std::string::npos) << r.cpp;
}
