// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of script imports (S20a, design 4.5 and 4.10): "using "parent.hot";" adds the
// classes of another script module to the generation. The child can derive from them.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

using namespace translator_test;

const char* kParent =
    "using \"w.h\";\n"
    "class Base : Widget {\n"
    "    int hp = 1;\n"
    "    virtual int Power() { return 1; }\n"
    "}\n";

const char* kChild =
    "using \"base.hot\";\n"
    "class Kid : Base {\n"
    "    override int Power() { return 2; }\n"
    "}\n";

hotc::TranslateOptions options(const TempDir& dir, const std::string& parent)
{
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, kChild);
    o.modules.emplace_back("base.hot", parent);
    return o;
}

// The child derives from the class of the imported module. The parent code names its own file.
TEST(ScriptImportTranslation, ChildDerivesFromAClassOfAnotherModule)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("struct Kid_Shell"), std::string::npos);
    EXPECT_NE(r.cpp.find("&Base_record"), std::string::npos) << "the child record points to the parent record";
    EXPECT_NE(r.cpp.find("#line 4 \"base.hot\""), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("#line 3 \"t.hot\""), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("#include \"base.hot\""), std::string::npos) << "a script import is not a C++ include";
    EXPECT_NE(r.slots_out.find("Base::Power"), std::string::npos) << r.slots_out;
}

// The source map has segments for the imported script too. The file number tells the script.
TEST(ScriptImportTranslation, SourceMapHasSegmentsOfTheImportedScript)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string child = kChild, parent = kParent;
    ASSERT_EQ(r.source_map.files().size(), 1u);
    EXPECT_EQ(r.source_map.files()[0].name, "base.hot");
    EXPECT_EQ(r.source_map.files()[0].text, parent);
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_EQ(r.imports[0].first, "base.hot");
    size_t imported = 0, main = 0;
    for (const hotc::SourceSegment& s : r.source_map.segments())
    {
        if (s.file == 0)
        {
            ++main;
            EXPECT_LE(s.script_begin + s.script_len, child.size());
        }
        else
        {
            ++imported;
            EXPECT_EQ(s.file, 1u);
            EXPECT_LE(s.script_begin + s.script_len, parent.size());
        }
    }
    EXPECT_GT(main, 0u);
    EXPECT_GT(imported, 0u);
}

// A generated position of the imported script maps back to the real line and column of that
// script. map_to_script() gives no value for it, because it is not in the main script.
TEST(ScriptImportTranslation, LocateGivesTheImportedFileLineAndColumn)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // kParent line 4: "    virtual int Power() { return 1; }". The name "Power" is at column 17.
    const auto gen = r.source_map.map_to_generated(1, 4, 17);
    ASSERT_TRUE(gen.has_value());
    const auto loc = r.source_map.locate(gen->line, gen->col);
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->file, 1u);
    EXPECT_EQ(loc->pos.line, 4);
    EXPECT_EQ(loc->pos.col, 17);
    EXPECT_FALSE(r.source_map.map_to_script(gen->line, gen->col).has_value());
    // The same position in the main script does not hit the imported segment.
    EXPECT_FALSE(r.source_map.map_to_generated(3, 17).has_value() && r.source_map.map_to_generated(3, 17) == gen);
    // The main script still maps. kChild line 3: "    override int Power() { return 2; }".
    const auto mg = r.source_map.map_to_generated(3, 18);
    ASSERT_TRUE(mg.has_value());
    const auto mloc = r.source_map.locate(mg->line, mg->col);
    ASSERT_TRUE(mloc.has_value());
    EXPECT_EQ(mloc->file, 0u);
    EXPECT_EQ(mloc->pos.line, 3);
}

// A translator error in the imported script names its file, line and column and shows its line.
TEST(ScriptImportTranslation, TranslatorErrorHasRealFileLineColumnAndSnippet)
{
    TempDir dir;
    const std::string bad = "using \"w.h\";\nclass Base : Widget {\n    int hp = 1;\n    int hp = 2;\n}\n";
    const hotc::TranslateResult r = hotc::translate(options(dir, bad));
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.imports.size(), 1u) << "the imports are there also when the translation fails";
    const std::string out = hotc::rewrite_translator_diagnostic(r.diagnostics, hotc::script_texts("t.hot", kChild, r));
    EXPECT_NE(out.find("base.hot:4:9: error:"), std::string::npos) << out;
    EXPECT_NE(out.find("    4 |     int hp = 2;\n"), std::string::npos) << out;
}

// A compiler error in the imported script gets its file, line, column and source line.
TEST(ScriptImportTranslation, CompilerErrorHasRealFileLineColumnAndSnippet)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The physical line and column of "return 1" in the generated text.
    const size_t at = r.cpp.find("return 1");
    ASSERT_NE(at, std::string::npos);
    const int line = 1 + static_cast<int>(std::count(r.cpp.begin(), r.cpp.begin() + static_cast<long>(at), '\n'));
    const int col = static_cast<int>(at - (r.cpp.rfind('\n', at) + 1)) + 1;
    // Clang prints the position of the "#line" directive: the file and the script line. It shows
    // the generated line. Several generated lines can have the same script line.
    const size_t line_end = r.cpp.find('\n', at);
    const std::string shown = r.cpp.substr(r.cpp.rfind('\n', at) + 1, line_end - (r.cpp.rfind('\n', at) + 1));
    const std::string clang =
        "base.hot:4:" + std::to_string(col) + ": error: expected ';'\n    4 | " + shown + "\n      | ^\n";
    const std::string out =
        hotc::rewrite_diagnostic(clang, {{"t.hot", kChild}}, {{"/w/t.hot.cpp", r.cpp}}, &r.source_map);
    // "    virtual int Power() { return 1; }": "return" is at column 27.
    EXPECT_EQ(out, "base.hot:4:27: error: expected ';'\n    4 |     virtual int Power() { return 1; }\n");
}

// An error in the imported script names the file and the line of that script.
TEST(ScriptImportTranslation, ErrorInTheImportNamesItsFile)
{
    TempDir dir;
    const std::string bad = "using \"w.h\";\nclass Base : Widget {\n    int hp = 1;\n    int hp = 2;\n}\n";
    const hotc::TranslateResult r = hotc::translate(options(dir, bad));
    ASSERT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("base.hot:4:"), std::string::npos) << r.diagnostics;
}

// The import is read from the directory of the script when TranslateOptions::modules has no text.
TEST(ScriptImportTranslation, ImportFromDisk)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    write_file(dir.path() / "base.hot", kParent);
    hotc::TranslateOptions o = temp_options(dir, kChild);
    o.script_name = (dir.path() / "t.hot").string();
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The "#line" directive has the path of the import. On Windows the path can have "/" or
    // "\\" (a C string) for each "\".
    const std::filesystem::path base = dir.path() / "base.hot";
    std::string escaped;
    for (char c : base.string())
        escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    const bool found = r.cpp.find(base.string() + "\"") != std::string::npos
        || r.cpp.find(escaped + "\"") != std::string::npos
        || r.cpp.find(base.generic_string() + "\"") != std::string::npos;
    EXPECT_TRUE(found) << base.generic_string() << "\n" << r.cpp.substr(0, 2000);
}

// An imported script uses the namespace of the main script. Another value is an error.
TEST(ScriptImportTranslation, ImportWithAnotherStdNamespaceIsAnError)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir, std::string("[std(\"other_std\")];\n") + kParent);
    o.preludes.emplace_back("host.prelude", "[std(\"mystd\")];\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("sets the namespace 'other_std' but the program uses 'mystd'"), std::string::npos)
        << r.diagnostics;
}

// An imported script uses the array template of the main script. Another value is an error.
TEST(ScriptImportTranslation, ImportWithAnotherArrayTemplateIsAnError)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir, std::string("[array(\"other::Arr<$T, $N>\")];\n") + kParent);
    o.preludes.emplace_back("host.prelude", "[array(\"my::Fixed<$T, $N>\")];\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("sets the array template 'other::Arr<$T, $N>' but the program uses 'my::Fixed<$T, $N>'"),
        std::string::npos)
        << r.diagnostics;
}

// The same value in the import is valid, and the import uses the template of the main script.
TEST(ScriptImportTranslation, ImportUsesTheArrayTemplateOfTheProgram)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir,
        "using \"w.h\";\nclass Base : Widget {\n    int[2] hp;\n    virtual int Power() { return 1; }\n}\n");
    o.script_text = "[array(\"my::Fixed<$T, $N>\")];\n" + o.script_text;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("my::Fixed<int32_t, 2> hp;"), std::string::npos) << r.cpp;
}

// An import that sets the same template as the main script is valid.
TEST(ScriptImportTranslation, ImportWithTheSameArrayTemplateIsValid)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir,
        "[array(\"my::Fixed<$T, $N>\")];\nusing \"w.h\";\nclass Base : Widget {\n    int[2] hp;\n"
        "    virtual int Power() { return 1; }\n}\n");
    o.script_text = "[array(\"my::Fixed<$T, $N>\")];\n" + o.script_text;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("my::Fixed<int32_t, 2> hp;"), std::string::npos) << r.cpp;
}

// The byte 0x01 is the internal placeholder of the standard namespace. An import cannot hold it.
TEST(ScriptImportTranslation, TheByte0x01IsAnErrorInAnImport)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(options(dir, std::string(kParent) + "// a\x01 b\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("the byte 0x01 is not allowed in a script"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("base.hot"), std::string::npos) << r.diagnostics;
}

// Errors: a missing script, module variables in an import, and a class in two modules.
TEST(ScriptImportTranslation, ImportErrors)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir, kParent);
    o.modules.clear();
    EXPECT_NE(hotc::translate(o).diagnostics.find("the script 'base.hot' is not found"), std::string::npos);
    const hotc::TranslateResult v = hotc::translate(options(dir, std::string(kParent) + "int count = 0;\n"));
    EXPECT_NE(v.diagnostics.find("an imported script cannot have module variables"), std::string::npos)
        << v.diagnostics;
    const hotc::TranslateResult twice =
        hotc::translate(options(dir, std::string(kParent) + "class Kid : Widget { }\n"));
    EXPECT_NE(twice.diagnostics.find("class 'Kid' is declared in 'base.hot' and in 't.hot'"), std::string::npos)
        << twice.diagnostics;
}

// An import of an import comes first. A script that two modules import is parsed one time.
TEST(ScriptImportTranslation, NestedImports)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir, "using \"root.hot\";\nclass Base : Root { }\n");
    o.modules.emplace_back("root.hot",
        "using \"w.h\";\nusing \"base.hot\";\nclass Root : Widget {\n    virtual int Power() { return 0; }\n}\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("struct Root_Shell"), r.cpp.find("struct Base_Shell"));
    EXPECT_LT(r.cpp.find("struct Base_Shell"), r.cpp.find("struct Kid_Shell"));
}

} // namespace
