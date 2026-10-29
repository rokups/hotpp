// Translation tests of script imports (S20a, design 4.5 and 4.10): "using "parent.spp";" adds the
// classes of another script module to the generation. The child can derive from them.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

namespace {

using namespace translator_test;

const char* kParent =
    "using \"w.h\";\n"
    "class Base : Widget {\n"
    "    int hp = 1;\n"
    "    virtual int Power() { return 1; }\n"
    "}\n";

const char* kChild =
    "using \"base.spp\";\n"
    "class Kid : Base {\n"
    "    override int Power() { return 2; }\n"
    "}\n";

sppc::TranslateOptions options(const TempDir& dir, const std::string& parent)
{
    write_file(dir.path() / "w.h", kHeaderV1);
    sppc::TranslateOptions o = temp_options(dir, kChild);
    o.modules.emplace_back("base.spp", parent);
    return o;
}

// The child derives from the class of the imported module. The parent code names its own file.
TEST(ScriptImportTranslation, ChildDerivesFromAClassOfAnotherModule)
{
    TempDir dir;
    const sppc::TranslateResult r = sppc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("struct Kid_Shell"), std::string::npos);
    EXPECT_NE(r.cpp.find("&Base_record"), std::string::npos) << "the child record points to the parent record";
    EXPECT_NE(r.cpp.find("#line 4 \"base.spp\""), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("#line 3 \"t.spp\""), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("#include \"base.spp\""), std::string::npos) << "a script import is not a C++ include";
    EXPECT_NE(r.slots_out.find("Base::Power"), std::string::npos) << r.slots_out;
}

// The source map covers the importing script only.
TEST(ScriptImportTranslation, SourceMapHasOnlyTheMainScript)
{
    TempDir dir;
    const sppc::TranslateResult r = sppc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string child = kChild;
    for (const sppc::SourceSegment& s : r.source_map.segments()) EXPECT_LE(s.script_begin + s.script_len, child.size());
}

// An error in the imported script names the file and the line of that script.
TEST(ScriptImportTranslation, ErrorInTheImportNamesItsFile)
{
    TempDir dir;
    const std::string bad = "using \"w.h\";\nclass Base : Widget {\n    int hp = 1;\n    int hp = 2;\n}\n";
    const sppc::TranslateResult r = sppc::translate(options(dir, bad));
    ASSERT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("base.spp:4:"), std::string::npos) << r.diagnostics;
}

// The import is read from the directory of the script when TranslateOptions::modules has no text.
TEST(ScriptImportTranslation, ImportFromDisk)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    write_file(dir.path() / "base.spp", kParent);
    sppc::TranslateOptions o = temp_options(dir, kChild);
    o.script_name = (dir.path() / "t.spp").string();
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find((dir.path() / "base.spp").string() + "\""), std::string::npos);
}

// Errors: a missing script, module variables in an import, and a class in two modules.
TEST(ScriptImportTranslation, ImportErrors)
{
    TempDir dir;
    sppc::TranslateOptions o = options(dir, kParent);
    o.modules.clear();
    EXPECT_NE(sppc::translate(o).diagnostics.find("the script 'base.spp' is not found"), std::string::npos);
    const sppc::TranslateResult v = sppc::translate(options(dir, std::string(kParent) + "int count = 0;\n"));
    EXPECT_NE(v.diagnostics.find("an imported script cannot have module variables"), std::string::npos) << v.diagnostics;
    const sppc::TranslateResult twice = sppc::translate(options(dir, std::string(kParent) + "class Kid : Widget { }\n"));
    EXPECT_NE(twice.diagnostics.find("class 'Kid' is declared in 'base.spp' and in 't.spp'"), std::string::npos)
        << twice.diagnostics;
}

// An import of an import comes first. A script that two modules import is parsed one time.
TEST(ScriptImportTranslation, NestedImports)
{
    TempDir dir;
    sppc::TranslateOptions o = options(dir, "using \"root.spp\";\nclass Base : Root { }\n");
    o.modules.emplace_back("root.spp", "using \"w.h\";\nusing \"base.spp\";\nclass Root : Widget {\n    virtual int Power() { return 0; }\n}\n");
    const sppc::TranslateResult r = sppc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct Root_Shell"), r.cpp.find("struct Base_Shell"));
    EXPECT_LT(r.cpp.find("struct Base_Shell"), r.cpp.find("struct Kid_Shell"));
}

}  // namespace
