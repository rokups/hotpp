// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of the importer text. A body-only edit of an imported script must not change
// the text of the importer. An interface edit of the imported script must change it.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace translator_test;

// The importer uses a class, a struct, an enum, a constant, a module variable, a free function
// and an alias of the imported script.
const char* kImporter =
    "using \"w.h\";\n"
    "using \"lib.hot\";\n"
    "class Kid : Base {\n"
    "    override int Power() { return Helper() + K + count + this.Twice(); }\n"
    "    int Use() { Pt p; Num n = 1; Mode m = Mode.B; return p.Get() + n + Helper(2); }\n"
    "}\n";

// The structs, the enum, the constant and the alias are above the edited bodies. The edits
// move only classes and free functions.
const char* kLib =
    "using \"w.h\";\n"
    "struct Pt {\n"
    "    int v = 1;\n"
    "    int Get() { return this.v; }\n"
    "}\n"
    "enum Mode { A, B }\n"
    "const int K = 3;\n"
    "using Num = int;\n"
    "int count = 0;\n"
    "int First() { return 1; }\n"
    "class Base : Widget {\n"
    "    int hp = 1;\n"
    "    virtual int Power() { return 1; }\n"
    "    int Twice() { return this.hp * 2; }\n"
    "    static int Make(int a = 1) { return a; }\n"
    "}\n"
    "int Helper(int a = 1) { return a; }\n"
    "class Other : Widget {\n"
    "    int z = K + 2;\n"
    "    Other(int a = 2) { this.z = a; }\n"
    "    void Run() { }\n"
    "    int Get(int a = 1) { return this.z + a; }\n"
    "    override void Update(float dt) { this.z = 3; }\n"
    "}\n";

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

hotc::TranslateResult translate_with(const TempDir& dir, const std::string& lib, const std::string& importer)
{
    write_file(dir.path() / "w.h", kHeaderV1);
    write_file(dir.path() / "ns.h", "#pragma once\nnamespace foo { struct Q { int a = 0; }; }\n");
    hotc::TranslateOptions o = temp_options(dir, importer);
    o.modules.emplace_back("lib.hot", lib);
    return hotc::translate(o);
}

// Translate the importer with "lib" and with "edited". The two texts must be equal.
void expect_same_text(const std::string& lib, const std::string& edited, const std::string& importer = kImporter)
{
    TempDir dir;
    const hotc::TranslateResult a = translate_with(dir, lib, importer);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const hotc::TranslateResult b = translate_with(dir, edited, importer);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp) << edited;
    EXPECT_EQ(a.warnings, b.warnings);
}

// Translate the importer with "lib" and with "edited". The two texts must differ.
void expect_other_text(const std::string& lib, const std::string& edited, const std::string& importer = kImporter)
{
    TempDir dir;
    const hotc::TranslateResult a = translate_with(dir, lib, importer);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const hotc::TranslateResult b = translate_with(dir, edited, importer);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(a.cpp, b.cpp) << edited;
}

// A method body edit of an imported class adds lines. The later methods, the later free
// function and the later class move. The text of the importer does not change.
TEST(ImportStableTextTranslation, ImporterTextIsStableForABodyEditOfAnImport)
{
    expect_same_text(kLib,
        replaced(kLib, "virtual int Power() { return 1; }",
            "virtual int Power()\n    {\n        int t = 1;\n        t = t + 1;\n        return t;\n    }"));
    // The same size edit does not move a line.
    expect_same_text(kLib, replaced(kLib, "return this.hp * 2;", "return this.hp * 3;"));
}

// A body edit above a free function and a class moves them. The text of the importer does not change.
TEST(ImportStableTextTranslation, ImporterTextIsStableForABodyEditAboveAFunctionAndAClass)
{
    expect_same_text(kLib,
        replaced(kLib, "int First() { return 1; }", "int First()\n{\n    int t = 1;\n\n    return t;\n}"));
    // A body edit of the last class removes lines of nothing that the importer uses.
    expect_same_text(kLib, replaced(kLib, "void Run() { }", "void Run()\n    {\n        this.z = 3;\n    }"));
}

// The "using" items of an import can come after a body. A body edit above them moves them, but
// the include and the "using namespace" in the importer keep their text.
TEST(ImportStableTextTranslation, ImporterTextIsStableForABodyEditAboveTheUsingItemsOfAnImport)
{
    const std::string lib = "int F() { return 1; }\nusing \"w.h\";\nusing \"ns.h\";\nusing foo;\n"
                            "class Base : Widget {\n    int hp = 1;\n    virtual int Power() { return 1; }\n"
                            "    int Twice() { return this.hp * 2; }\n}\n"
                            "int Helper(int a = 1) { return a; }\n";
    const std::string importer = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Base {\n"
                                 "    override int Power() { return Helper() + F(); }\n}\n";
    expect_same_text(lib, replaced(lib, "int F() { return 1; }", "int F()\n{\n    int t = 1;\n\n    return t;\n}"),
        importer);
}

// An imported base class has a convert hook and a constructor with base arguments. A body edit
// above them moves them. The importer derives from the class, and its text does not change.
TEST(ImportStableTextTranslation, ImporterTextIsStableForABodyEditAboveAHookAndAConstructor)
{
    const std::string lib = "using \"w.h\";\nint F() { return 1; }\n"
                            "class Root : Widget {\n    int r = 0;\n    Root(int v) { this.r = v; }\n}\n"
                            "class Base : Root {\n    float level = 1.0;\n    int hp = 1;\n"
                            "    Base(int v = 2) : base(v + 1) { this.hp = v; }\n"
                            "    float convert_level(int old) { return old * 2.0; }\n"
                            "    virtual int Power() { return 1; }\n}\n";
    const std::string importer = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Base {\n"
                                 "    Kid() : base(5) { }\n    override int Power() { return 2; }\n}\n";
    expect_same_text(lib, replaced(lib, "int F() { return 1; }", "int F()\n{\n    int t = 1;\n\n    return t;\n}"),
        importer);
}

// An imported base class has constructors with base arguments and no hook. A body edit above
// it moves it. The importer derives from the class, and its text does not change.
TEST(ImportStableTextTranslation, ImporterTextIsStableForABodyEditAboveAnInterfaceParentWithBaseArguments)
{
    // The constant keeps the line of the import, so it is above the edit.
    const std::string lib = "using \"w.h\";\nconst int K = 3;\nint F() { return 1; }\n"
                            "class Root : Widget {\n    int r = 0;\n    Root(int v) { this.r = v; }\n}\n"
                            "class Base : Root {\n    int hp = K + 1;\n"
                            "    Base(int v = 2) : base(v + K) { this.hp = v; }\n"
                            "    Base(float f, int v) : base(v) { }\n"
                            "    virtual int Power() { return 1; }\n}\n";
    const std::string importer = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Base {\n"
                                 "    Kid() : base(5) { }\n    override int Power() { return 2; }\n}\n";
    expect_same_text(lib, replaced(lib, "int F() { return 1; }", "int F()\n{\n    int t = 1;\n\n    return t;\n}"),
        importer);
}

// A struct of an import is copied with its members and bodies. A body edit of a struct is an
// interface edit and changes the text of the importer.
TEST(ImportStableTextTranslation, ImporterTextDiffersForAStructBodyEdit)
{
    expect_other_text(kLib, replaced(kLib, "return this.v;", "return this.v + 1;"));
}

// A field initializer of an imported struct is copied. An edit of it changes the importer text.
TEST(ImportStableTextTranslation, ImporterTextDiffersForAStructFieldInitializerEdit)
{
    expect_other_text(kLib, replaced(kLib, "int v = 1;", "int v = 2;"));
}

// The importer calls Helper() without an argument, so it copies the default. A default edit
// changes the importer text.
TEST(ImportStableTextTranslation, ImporterTextDiffersForAParameterDefaultEdit)
{
    expect_other_text(kLib, replaced(kLib, "int Helper(int a = 1)", "int Helper(int a = 5)"));
}

// A public constant of an import is copied with its value. A value edit changes the importer text.
TEST(ImportStableTextTranslation, ImporterTextDiffersForAPublicConstantEdit)
{
    expect_other_text(kLib, replaced(kLib, "const int K = 3;", "const int K = 4;"));
}

// A new enum value changes the copied enum.
TEST(ImportStableTextTranslation, ImporterTextDiffersForAnEnumEdit)
{
    expect_other_text(kLib, replaced(kLib, "enum Mode { A, B }", "enum Mode { A, C, B }"));
}

// A "[std(...)]" edit of an import is an error of the importer (ImportWithAnotherStdNamespaceIsAnError).
// A "[lib(...)]" edit changes only the lowering of names in that namespace. The importer text
// does not change when the importer does not use such a name, so there is no test of it here.

// A new public name of an import is copied into the importer.
TEST(ImportStableTextTranslation, ImporterTextDiffersForANewName)
{
    expect_other_text(kLib, std::string(kLib) + "int Third() { return 3; }\n");
}

// A struct keeps the position of its imported file: its members hold user expressions of the
// import. An edit above the struct moves it, so the "#line" of the copy changes.
TEST(ImportStableTextTranslation, ImporterTextDiffersForAStructMovedByAnEditAboveIt)
{
    expect_other_text(kLib, replaced(kLib, "using \"w.h\";\n", "using \"w.h\";\n\n\n"));
}

// A lint warning of a class body of the import stays in the import. The importer does not
// copy the class bodies, so it does not report the warning.
TEST(ImportStableTextTranslation, ClassBodyWarningOfAnImportIsNotInTheImporter)
{
    TempDir dir;
    const std::string lib = replaced(kLib, "int Twice() { return this.hp * 2; }",
        "int Twice() { int unused_value = 5; return this.hp * 2; }");
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult own = hotc::translate(temp_options(dir, lib));
    ASSERT_TRUE(own.ok) << own.diagnostics;
    const hotc::TranslateResult r = translate_with(dir, lib, kImporter);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.warnings.find("lib.hot"), std::string::npos) << r.warnings;
    EXPECT_EQ(r.cpp.find("unused_value"), std::string::npos) << "the class body stays in its own module";
}

// An error in an import names the file, the line and the column of the import. The size of the
// main script does not change them, also when the main script is larger than the first import
// offset. Two translates of the same input give the same result.
TEST(ImportStableTextTranslation, ImportDiagnosticPositionDoesNotDependOnTheMainSize)
{
    const std::string bad_lib = replaced(kLib, "int First() { return 1; }", "int main() { return 1; }");
    const std::string small = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Widget { }\n";
    std::string text_sizes[3] = {small, small, small};
    // 3 KiB, 1.5 MiB and 3 MiB of comment lines before the code.
    const size_t comment_bytes[3] = {0, 3u << 10, 3u << 20};
    std::string first;
    for (int i = 0; i < 3; ++i)
    {
        std::string filler;
        while (filler.size() < comment_bytes[i])
            filler += "// filler line of the main script\n";
        text_sizes[i] = small + filler;
        TempDir dir;
        const hotc::TranslateResult a = translate_with(dir, bad_lib, text_sizes[i]);
        const hotc::TranslateResult b = translate_with(dir, bad_lib, text_sizes[i]);
        ASSERT_FALSE(a.ok);
        EXPECT_EQ(a.diagnostics, b.diagnostics);
        EXPECT_NE(a.diagnostics.find("lib.hot:10:1:"), std::string::npos) << a.diagnostics;
        if (i == 0)
            first = a.diagnostics;
        else
            EXPECT_EQ(first, a.diagnostics);
    }
    const hotc::TranslateResult ok_a = [&]
    {
        TempDir dir;
        return translate_with(dir, kLib, text_sizes[2] + "");
    }();
    EXPECT_TRUE(ok_a.ok) << ok_a.diagnostics;
}

} // namespace
