// Tests for sppc::translate with the "using" imports: system headers and missing headers.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

// Both forms of a system import give #include <name> in the output.
TEST(ImportTranslation, SystemImportForms)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string body = "class Foo : Widget {\n    void f() { }\n}\n";
    sppc::TranslatorSession session;
    for (const char* form : {"using <vector>;\nusing <sys/types.h>;\n",
                             "using \"<vector>\";\nusing \"<sys/types.h>\";\n"}) {
        sppc::TranslateResult r =
            sppc::translate(temp_options(dir, std::string(form) + "using \"w.h\";\n" + body), session);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        EXPECT_NE(r.cpp.find("#include <vector>\n"), std::string::npos) << r.cpp;
        EXPECT_NE(r.cpp.find("#include <sys/types.h>\n"), std::string::npos);
        EXPECT_NE(r.cpp.find("#include \"w.h\"\n"), std::string::npos);
        EXPECT_EQ(r.cpp.find("\"<vector>\""), std::string::npos);
    }
}

// A missing system header gives an error with the script name and the line.
TEST(ImportTranslation, MissingSystemHeaderNamesScriptAndLine)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    sppc::TranslatorSession session;
    for (const char* form : {"using <no_such_header>;", "using \"<no_such_header>\";"}) {
        sppc::TranslateOptions o =
            temp_options(dir, std::string("using \"w.h\";\n") + form +
                                  "\nclass Foo : Widget {\n    void f() { }\n}\n");
        sppc::TranslateResult r = sppc::translate(o, session);
        EXPECT_FALSE(r.ok);
        EXPECT_NE(r.diagnostics.find(o.script_name + ":2:"), std::string::npos) << r.diagnostics;
        EXPECT_NE(r.diagnostics.find("'no_such_header' file not found"), std::string::npos)
            << r.diagnostics;
    }
}
