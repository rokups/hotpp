// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the import view (hotc::import_view_hash). A body edit of a class keeps the view. Each
// edit that can change the importer output changes the view. An equal view must give an equal
// importer output: each test with an equal view also translates the importer two times.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace translator_test;

const char* kImporter =
    "using \"w.h\";\n"
    "using \"lib.hot\";\n"
    "class Kid : Base {\n"
    "    override int Power() { return Helper() + K + count + this.Twice(); }\n"
    "    int Use() { Pt p; Num n = 1; Mode m = Mode.B; return p.Get() + n + Helper(2); }\n"
    "}\n";

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

// The importer derives from a class with a hook and a constructor with base arguments.
const char* kHookImporter = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Base {\n"
                            "    Kid() : base(5) { }\n    override int Power() { return 2; }\n}\n";
const char* kHookLib = "using \"w.h\";\nint F() { return 1; }\n"
                       "class Root : Widget {\n    int r = 0;\n    Root(int v) { this.r = v; }\n}\n"
                       "class Base : Root {\n    float level = 1.0;\n    int hp = 1;\n"
                       "    Base(int v = 2) : base(v + 1) { this.hp = v; }\n"
                       "    float convert_level(int old) { return old * 2.0; }\n"
                       "    virtual int Power() { return 1; }\n}\n";

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
    hotc::TranslateOptions o = temp_options(dir, importer);
    o.modules.emplace_back("lib.hot", lib);
    return hotc::translate(o);
}

// The view hash of the translation: the search of "lib.hot". It must be the hash that
// hotc::import_view_hash gives for the text.
uint64_t view_of(const hotc::TranslateResult& r, const std::string& lib)
{
    for (const hotc::ImportLookup& l : r.import_lookups)
        if (l.file == "lib.hot")
        {
            EXPECT_TRUE(l.view_hash.has_value());
            const std::optional<uint64_t> again = hotc::import_view_hash(lib, l.array_tpl);
            EXPECT_TRUE(again.has_value());
            EXPECT_EQ(l.view_hash.value_or(0), again.value_or(1)) << "the translation and import_view_hash differ";
            return l.view_hash.value_or(0);
        }
    ADD_FAILURE() << "no search of lib.hot";
    return 0;
}

// The views are equal, and the importer output is the same for the two texts.
void expect_same_view(const std::string& lib, const std::string& edited, const std::string& importer = kImporter)
{
    TempDir dir;
    const hotc::TranslateResult a = translate_with(dir, lib, importer);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const hotc::TranslateResult b = translate_with(dir, edited, importer);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(view_of(a, lib), view_of(b, edited)) << edited;
    EXPECT_EQ(a.cpp, b.cpp) << edited;
    EXPECT_EQ(a.warnings, b.warnings);
    EXPECT_EQ(a.import_hashes, b.import_hashes);
}

void expect_other_view(const std::string& lib, const std::string& edited, const std::string& importer = kImporter)
{
    TempDir dir;
    const hotc::TranslateResult a = translate_with(dir, lib, importer);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const hotc::TranslateResult b = translate_with(dir, edited, importer);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(view_of(a, lib), view_of(b, edited)) << edited;
}

TEST(ImportViewTranslation, ClassMethodBodyEditThatAddsLinesKeepsTheView)
{
    expect_same_view(kLib,
        replaced(kLib, "virtual int Power() { return 1; }",
            "virtual int Power()\n    {\n        int t = 1;\n        t = t + 1;\n        return t;\n    }"));
    expect_same_view(kLib, replaced(kLib, "return this.hp * 2;", "return this.hp * 3;"));
    expect_same_view(kLib, replaced(kLib, "int First() { return 1; }", "int First()\n{\n    int t = 1;\n\n    return t;\n}"));
    expect_same_view(kLib, replaced(kLib, "void Run() { }", "void Run()\n    {\n        this.z = 3;\n    }"));
}

// The body of a hook and of a constructor of a class stays in its module.
TEST(ImportViewTranslation, HookAndConstructorBodyEditKeepsTheView)
{
    expect_same_view(kHookLib, replaced(kHookLib, "return old * 2.0;", "float t = old * 3.0;\n        return t;"),
        kHookImporter);
    expect_same_view(kHookLib, replaced(kHookLib, "{ this.hp = v; }", "{\n        this.hp = v + 1;\n    }"),
        kHookImporter);
    expect_same_view(kHookLib, replaced(kHookLib, "int F() { return 1; }", "int F()\n{\n    int t = 1;\n\n    return t;\n}"),
        kHookImporter);
}

TEST(ImportViewTranslation, StructBodyEditChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "return this.v;", "return this.v + 1;"));
}

TEST(ImportViewTranslation, ClassFieldInitializerEditChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "int hp = 1;", "int hp = 2;"));
}

TEST(ImportViewTranslation, PublicConstantEditChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "const int K = 3;", "const int K = 4;"));
}

TEST(ImportViewTranslation, EnumEditChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "enum Mode { A, B }", "enum Mode { A, C, B }"));
}

TEST(ImportViewTranslation, TypeAliasEditChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "using Num = int;", "using Num = float;"));
}

TEST(ImportViewTranslation, ParameterDefaultEditChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "int Helper(int a = 1)", "int Helper(int a = 5)"));
    expect_other_view(kLib, replaced(kLib, "static int Make(int a = 1)", "static int Make(int a = 2)"));
}

// A struct keeps its position in the importer output. An edit above it moves it.
TEST(ImportViewTranslation, StructMovedByAnEditAboveItChangesTheView)
{
    expect_other_view(kLib, replaced(kLib, "using \"w.h\";\n", "using \"w.h\";\n\n\n"));
    // A body edit above the struct moves it too.
    const std::string lib = "using \"w.h\";\nint First() { return 1; }\n"
                            "struct Pt {\n    int v = 1;\n    int Get() { return this.v; }\n}\n"
                            "class Base : Widget {\n    virtual int Power() { return 1; }\n    int Twice() { return 2; }\n}\n"
                            "int Helper(int a = 1) { return a; }\n";
    const std::string importer = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Base {\n"
                                 "    override int Power() { Pt p; return p.Get() + Helper(); }\n}\n";
    expect_other_view(lib, replaced(lib, "int First() { return 1; }", "int First()\n{\n    return 1;\n}"), importer);
}

TEST(ImportViewTranslation, NewGenericNameChangesTheView)
{
    expect_other_view(kLib, std::string(kLib) + "struct Box<T> {\n    T value;\n}\n");
}

TEST(ImportViewTranslation, AttributeEditChangesTheView)
{
    expect_other_view(kLib, "[lib(\"foo\")];\n" + std::string(kLib));
}

TEST(ImportViewTranslation, HookConstructorAndBaseArgumentEditChangesTheView)
{
    expect_other_view(kHookLib, replaced(kHookLib, "convert_level(int old)", "convert_level(float old)"), kHookImporter);
    expect_other_view(kHookLib, replaced(kHookLib, "Base(int v = 2)", "Base(int v = 3)"), kHookImporter);
    expect_other_view(kHookLib, replaced(kHookLib, "base(v + 1)", "base(v + 2)"), kHookImporter);
}

TEST(ImportViewTranslation, BadTextHasNoView)
{
    EXPECT_FALSE(hotc::import_view_hash("class {", "std::array<$T, $N>").has_value());
}

} // namespace

// The importer can get a warning with a place in the import: a borrowed pointer field or property
// of an imported class. A body edit above it keeps the view and moves the place. So the view alone
// does not give the warnings; the translate cache keeps no view for an entry with such a warning.
TEST(ImportViewTranslation, WarningOfAnImportedFieldMovesWithAnEqualView)
{
    const std::string lib = "using \"w.h\";\nclass Base : Widget {\n    void f() { }\n"
                            "    Item* p;\n    Item* Q { get; set; }\n}\n";
    const std::string edited = replaced(lib, "void f() { }", "void f()\n    {\n    }");
    const std::string imp = "using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Base {\n    int Use() { return 1; }\n}\n";
    auto run = [&](const std::string& text)
    {
        TempDir dir;
        write_file(dir.path() / "w.h", std::string(kHeaderV1) + "struct Item { int v; };\n");
        hotc::TranslateOptions o = temp_options(dir, imp);
        o.strict_borrowed = false;
        o.modules.emplace_back("lib.hot", text);
        return hotc::translate(o);
    };
    const hotc::TranslateResult a = run(lib);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const hotc::TranslateResult b = run(edited);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(a.warnings.find("lib.hot:4:"), std::string::npos) << a.warnings;
    EXPECT_NE(a.warnings.find("lib.hot:5:"), std::string::npos) << a.warnings;
    EXPECT_NE(b.warnings.find("lib.hot:6:"), std::string::npos) << b.warnings;
    EXPECT_EQ(view_of(a, lib), view_of(b, edited));
}

// A comment edit in a method body keeps the view.
TEST(ImportViewTranslation, CommentEditInABodyKeepsTheView)
{
    expect_same_view(kLib, replaced(kLib, "return this.hp * 2;", "// twice\n        return this.hp * 2; /* x */"));
}
