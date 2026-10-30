// Tests for hotc::translate with the "using" imports: system headers and missing headers.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

// Both forms of a system import give #include <name> in the output.
TEST(ImportTranslation, SystemImportForms)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string body = "class Foo : Widget {\n    void f() { }\n}\n";
    hotc::TranslatorSession session;
    for (const char* form :
        {"using <vector>;\nusing <sys/types.h>;\n", "using \"<vector>\";\nusing \"<sys/types.h>\";\n"})
    {
        hotc::TranslateResult r =
            hotc::translate(temp_options(dir, std::string(form) + "using \"w.h\";\n" + body), session);
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
    hotc::TranslatorSession session;
    for (const char* form : {"using <no_such_header>;", "using \"<no_such_header>\";"})
    {
        hotc::TranslateOptions o =
            temp_options(dir, std::string("using \"w.h\";\n") + form + "\nclass Foo : Widget {\n    void f() { }\n}\n");
        hotc::TranslateResult r = hotc::translate(o, session);
        EXPECT_FALSE(r.ok);
        EXPECT_NE(r.diagnostics.find(o.script_name + ":2:"), std::string::npos) << r.diagnostics;
        EXPECT_NE(r.diagnostics.find("'no_such_header' file not found"), std::string::npos) << r.diagnostics;
    }
}

// Many std types in the members must not give "too many errors" from libclang. The facts file
// can have more than 20 errors (one for each unknown type), and they are not errors of the imports.
TEST(ImportTranslation, ManyStdMembersAreNotImportErrors)
{
    TempDir dir;
    write_file(dir.path() / "u.h", "#pragma once\n#include <map>\n#include <utility>\n#include <vector>\n");
    const std::string script =
        "using \"u.h\";\n"
        "class P {\n"
        "    std.pair<int,int> a;\n"
        "    std.map<std.string,int> m;\n"
        "    std.vector<int> v;\n"
        "    std.pair<int,int> F() { return a; }\n"
        "}\n";
    hotc::TranslatorSession session;
    hotc::TranslateResult r = hotc::translate(temp_options(dir, script), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.diagnostics.find("the imports have errors"), std::string::npos) << r.diagnostics;
}
