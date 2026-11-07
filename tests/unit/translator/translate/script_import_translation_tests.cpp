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
    // "\\" (a C string) for each "\". The test changes both to "/" before the search.
    const std::filesystem::path base = dir.path() / "base.hot";
    std::string cpp;
    for (size_t i = 0; i < r.cpp.size(); ++i)
    {
        if (r.cpp[i] != '\\')
            cpp += r.cpp[i];
        else
        {
            cpp += '/';
            if (i + 1 < r.cpp.size() && r.cpp[i + 1] == '\\')
                ++i;
        }
    }
    const bool found = cpp.find(base.generic_string() + "\"") != std::string::npos;
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

// An alias is at file scope in the output. Its name must not be the name of a class of another script.
TEST(ScriptImportTranslation, AliasOfAnImportWithTheNameOfAClassOfTheMainScript)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, "using \"lib.hot\";\nclass Unit : Widget { }\n");
    o.modules.emplace_back("lib.hot", "using \"w.h\";\nusing Unit = int;\nclass Lib : Widget { }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("the type alias 'Unit' has the name of a class"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("lib.hot:2:"), std::string::npos) << r.diagnostics;
}

TEST(ScriptImportTranslation, AliasOfTheMainScriptWithTheNameOfAClassOfAnImport)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, "using \"lib.hot\";\nusing Lib = int;\nclass Main : Widget { }\n");
    o.modules.emplace_back("lib.hot", "using \"w.h\";\nclass Lib : Widget { }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("the type alias 'Lib' has the name of a class"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.hot:2:"), std::string::npos) << r.diagnostics;
}

// The same alias with the same type in two scripts is written one time. Two types are an error.
TEST(ScriptImportTranslation, SameAliasInTwoScripts)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o =
        temp_options(dir, "using \"lib.hot\";\nusing Num = int;\nclass Main : Widget { Num n; }\n");
    o.modules.emplace_back("lib.hot", "using \"w.h\";\nusing Num = int;\nclass Lib : Widget { Num n; }\n");
    hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t first = r.cpp.find("using Num = int32_t;");
    ASSERT_NE(first, std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("using Num = int32_t;", first + 1), std::string::npos) << r.cpp;

    o = temp_options(dir, "using \"lib.hot\";\nusing Num = float;\nclass Main : Widget { Num n; }\n");
    o.modules.emplace_back("lib.hot", "using \"w.h\";\nusing Num = int;\nclass Lib : Widget { Num n; }\n");
    r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("the type alias 'Num' is declared in two scripts with other types"), std::string::npos)
        << r.diagnostics;
}


// A module constant: "const T Name = value;" at file level. An imported script can have it.
// The output has one C++ "static const" before the classes. A case label can use an integral constant.
TEST(ScriptImportTranslation, ImportedConstantIsUsableInTheChild)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir, std::string(kParent) + "const int MaxHp = 3;\nconst float Scale = 0.5f * 2.0f;\n");
    o.script_text = "using \"base.hot\";\nconst int Twice = MaxHp * 2;\nclass Kid : Base {\n"
                    "    override int Power() {\n        switch (hp) {\n            case MaxHp: return Twice;\n"
                    "            default: return (int)Scale;\n        }\n    }\n}\n";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t max_hp = r.cpp.find("static const int32_t MaxHp = 3;");
    ASSERT_NE(max_hp, std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("MaxHp = 3;", max_hp + 30), std::string::npos) << "the constant is in the output one time";
    EXPECT_NE(r.cpp.find("static const float Scale = 0.5f * 2.0f;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("static const int32_t Twice = MaxHp * 2;"), std::string::npos) << r.cpp;
    EXPECT_LT(max_hp, r.cpp.find("static const int32_t Twice")) << "the imported constants come first";
    EXPECT_LT(max_hp, r.cpp.find("case MaxHp:")) << "the constants come before the method bodies";
    EXPECT_NE(r.cpp.find("case MaxHp:"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("hot_mod()->MaxHp"), std::string::npos) << "a constant is not a module variable";
}

// Errors: "const" on a class member, "const" with no value, and a constant that is declared twice.
TEST(ScriptImportTranslation, ConstantErrors)
{
    TempDir dir;
    hotc::TranslateOptions o = options(dir, kParent);
    o.script_text = "using \"base.hot\";\nclass Kid : Base {\n    const int Max = 1;\n}\n";
    const hotc::TranslateResult member = hotc::translate(o);
    EXPECT_NE(member.diagnostics.find("a class member cannot be 'const'; for a class constant, write 'static const' before the type of 'Max'"),
        std::string::npos)
        << member.diagnostics;
    o.script_text = "using \"base.hot\";\nconst int Max;\n";
    const hotc::TranslateResult no_value = hotc::translate(o);
    EXPECT_NE(no_value.diagnostics.find("the constant 'Max' needs a value"), std::string::npos) << no_value.diagnostics;
    const hotc::TranslateResult twice = hotc::translate(options(dir, std::string(kParent) + "const int Max = 1;\n"));
    o = options(dir, std::string(kParent) + "const int Max = 1;\n");
    o.script_text = "using \"base.hot\";\nconst int Max = 2;\n";
    const hotc::TranslateResult dup = hotc::translate(o);
    EXPECT_NE(dup.diagnostics.find("constant 'Max' is declared twice"), std::string::npos) << dup.diagnostics;
    EXPECT_TRUE(twice.ok) << twice.diagnostics;
}

// Modules (step 1): each script is a module. An import gives the declarations of the module.

hotc::TranslateResult translate_alone(const TempDir& dir, const std::string& text)
{
    write_file(dir.path() / "w.h", kHeaderV1);
    return hotc::translate(temp_options(dir, text));
}

// A cycle A <-> B translates. The main script is in the set of the read scripts, so the import
// of the main script does not read it again.
TEST(ScriptImportTranslation, ImportCycleWithTheMainScript)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(options(dir, "using \"t.hot\";\n" + std::string(kParent)));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_EQ(r.imports.front().first, "base.hot") << "the main script is not an import";
    ASSERT_EQ(r.import_hashes.size(), 1u);
    EXPECT_EQ(r.import_hashes.front().first, "base.hot");
}

// A child in another unit runs the field initializers and the "base(...)" arguments of its parent
// (modules step 5), so they are part of the interface hash.
TEST(ScriptImportTranslation, InterfaceHashCoversFieldInitializersAndBaseArguments)
{
    TempDir dir;
    const std::string text = "using \"w.h\";\nclass Unit : Widget {\n    int hp = 3;\n    Unit(int v) { this.hp = v; }\n}\n"
                             "class Elite : Unit {\n    Elite() : base(1) { }\n}\n";
    const hotc::TranslateResult a = translate_alone(dir, text);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    std::string init = text;
    init.replace(init.find("hp = 3"), 6, "hp = 4");
    const hotc::TranslateResult b = translate_alone(dir, init);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(a.iface_hash, b.iface_hash) << b.iface;
    std::string sup = text;
    sup.replace(sup.find("base(1)"), 7, "base(2)");
    const hotc::TranslateResult c = translate_alone(dir, sup);
    ASSERT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(a.iface_hash, c.iface_hash) << c.iface;
}

// The interface hash ignores a body change. A field change gives another hash.
TEST(ScriptImportTranslation, InterfaceHashIsStableForABodyChange)
{
    TempDir dir;
    const hotc::TranslateResult a = translate_alone(dir, kParent);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    std::string body(kParent);
    body.replace(body.find("return 1"), 8, "return 7");
    const hotc::TranslateResult b = translate_alone(dir, body);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(a.iface_hash, 0u);
    EXPECT_EQ(a.iface_hash, b.iface_hash) << a.iface << "\n" << b.iface;
    std::string field(kParent);
    field.replace(field.find("int hp"), 6, "float hp");
    const hotc::TranslateResult c = translate_alone(dir, field);
    ASSERT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(a.iface_hash, c.iface_hash) << c.iface;
}

// The importer sees the same hash of an import as the owner of the import.
TEST(ScriptImportTranslation, ImportHashIsTheHashOfTheOwner)
{
    TempDir dir;
    const hotc::TranslateResult owner = translate_alone(dir, kParent);
    ASSERT_TRUE(owner.ok) << owner.diagnostics;
    const hotc::TranslateResult r = hotc::translate(options(dir, kParent));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.import_hashes.size(), 1u);
    EXPECT_EQ(r.import_hashes.front().second, owner.iface_hash);
    EXPECT_NE(r.iface_hash, owner.iface_hash);
}

// The interface of a class has its parent, its fields and its virtual slots. Class B in b.hot
// derives from class A in a.hot.
TEST(ScriptImportTranslation, InterfaceOfAClassHasItsParentFieldsAndSlots)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"a.hot\";\nclass B : A {\n    int armor = 2;\n    override int Power() { return 3; }\n}\n");
    o.script_name = "b.hot";
    o.modules.emplace_back("a.hot",
        "using \"w.h\";\nclass A : Widget {\n    int hp = 1;\n    virtual int Power() { return 1; }\n}\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.iface.find("class B : A"), std::string::npos) << r.iface;
    EXPECT_NE(r.iface.find("field int32_t armor"), std::string::npos) << r.iface;
    EXPECT_NE(r.iface.find("override int32_t Power()"), std::string::npos) << r.iface;
    EXPECT_EQ(r.iface.find("class A"), std::string::npos) << "the interface has only the own names: " << r.iface;
    const hotc::TranslateResult a = translate_alone(dir,
        "using \"w.h\";\nclass A : Widget {\n    int hp = 1;\n    virtual int Power() { return 1; }\n}\n");
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_NE(a.iface.find("class A : Widget"), std::string::npos) << a.iface;
    EXPECT_NE(a.iface.find("field int32_t hp"), std::string::npos) << a.iface;
    EXPECT_NE(a.iface.find("virtual int32_t Power()"), std::string::npos) << a.iface;
    ASSERT_EQ(r.import_hashes.size(), 1u);
    EXPECT_EQ(r.import_hashes.front().second, a.iface_hash);
}

// An imported script can have module variables and free functions. The importer gets their
// declarations only: the body of the free function is not in the output of the importer.
TEST(ScriptImportTranslation, ImportWithModuleVariableAndFreeFunction)
{
    TempDir dir;
    const hotc::TranslateResult r = hotc::translate(
        options(dir, std::string(kParent) + "int count = 0;\nint Twice(int x) { return x * 2 + 12345; }\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("12345"), std::string::npos) << "the body stays in its own module";
    const hotc::TranslateResult own = translate_alone(dir, "int count = 0;\nint Twice(int x) { return x * 2; }\n");
    ASSERT_TRUE(own.ok) << own.diagnostics;
    EXPECT_NE(own.iface.find("var int32_t count"), std::string::npos) << own.iface;
    EXPECT_NE(own.iface.find("fn int32_t Twice(int32_t x)"), std::string::npos) << own.iface;
}

// "private" at file level hides a name from the importers. The interface does not have it.
TEST(ScriptImportTranslation, PrivateHidesANameFromImporters)
{
    TempDir dir;
    const std::string lib = "using \"w.h\";\nprivate class Base : Widget { }\nprivate struct P { int x = 0; }\n"
                            "private enum E { A }\nprivate const int K = 1;\nprivate int count = 0;\n"
                            "private int F() { return 1; }\nint G() { return F() + count + K; }\n";
    const hotc::TranslateResult own = translate_alone(dir, lib);
    ASSERT_TRUE(own.ok) << own.diagnostics;
    EXPECT_NE(own.iface.find("fn int32_t G()"), std::string::npos) << own.iface;
    for (const char* hidden : {"Base", "P", "E", "K", "count", "F("})
        EXPECT_EQ(own.iface.find(std::string(" ") + hidden), std::string::npos) << hidden << ": " << own.iface;
    const hotc::TranslateResult r = hotc::translate(options(dir, lib));
    ASSERT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("class 'Base' is private in 'base.hot'"), std::string::npos) << r.diagnostics;
}

} // namespace
