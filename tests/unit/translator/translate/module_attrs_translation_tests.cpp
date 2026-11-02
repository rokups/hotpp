// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the module attribute list "[std(\"name\")];" (the standard namespace of the host).
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using hotc::SourcePos;

using namespace check_translate_test;

namespace
{

const char* kMystd = "[std(\"mystd\")];\n";
const char* kDefaultHandle = "[native, default_handle(\"$std::shared_ptr<$T>\")] class *;\n";

} // namespace

TEST(ModuleAttrsTranslation, TheNamespaceReplacesStdInTheGeneratedText)
{
    const hotc::TranslateResult r = check_translate("",
        "    std.string s;\n    std.vector<int> v;\n    void F() { this.s = std.to_string(3); }\n",
        {{"host.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::string s;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "mystd::vector<int32_t> v;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "mystd::to_string(3)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "\x01std::")) << r.cpp;
}

TEST(ModuleAttrsTranslation, WithoutTheListTheNamespaceIsStd)
{
    const hotc::TranslateResult r = check_translate("", "    std.string s;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::string s;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ANestedNamespaceIsValid)
{
    const hotc::TranslateResult r =
        check_translate("", "    std.string s;\n", {{"host.prelude", "[std(\"my::eastl\")];\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "my::eastl::string s;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheHandleStubUsesTheNamespace)
{
    const hotc::TranslateResult r = check_translate("", "    Mesh m;\n    void Make() { this.m = new Mesh(); }\n",
        {{"host.prelude", std::string(kMystd) + kDefaultHandle}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::shared_ptr<Mesh> m;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "mystd::make_shared<Mesh>()")) << r.cpp;
}

TEST(ModuleAttrsTranslation, PlainStdInAStubStaysLiteral)
{
    const hotc::TranslateResult r = check_translate("", "    Mesh m;\n",
        {{"host.prelude", std::string(kMystd) + "[native, default_handle(\"std::shared_ptr<$T>\")] class *;\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Mesh> m;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ALaterPreludeOverridesTheStdOfAnEarlierOne)
{
    const hotc::TranslateResult r = check_translate(
        "", "    std.string s;\n", {{"a.prelude", "[std(\"other\")];\n"}, {"b.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ASecondStdInOnePreludeIsAnError)
{
    const hotc::TranslateResult r =
        check_translate("", "    int x;\n", {{"a.prelude", std::string(kMystd) + "[std(\"mystd\")];\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'std' is given twice")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, TheListInAScriptSetsTheNamespaceOfTheScript)
{
    const hotc::TranslateResult r = check_translate(kMystd, "    std.string s;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::string s;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheListInTheScriptOverridesThePreludeDefault)
{
    const hotc::TranslateResult r = check_translate("[std(\"my::eastl\")];\n", "    std.string s;\n",
        {{"host.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "my::eastl::string s;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "mystd::")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ARepeatedAttributeInOneScriptIsAnError)
{
    const hotc::TranslateResult r = check_translate(std::string(kMystd) + kMystd, "    int x;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'std' is given twice in one file")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, ABareSemicolonDoesNothing)
{
    const hotc::TranslateResult r = check_translate(";\n", "    std.string s;\n", {{"host.prelude", ";\n" + std::string(kMystd)}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::string s;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ABadNamespaceIsAnError)
{
    const hotc::TranslateResult r =
        check_translate("", "    int x;\n", {{"host.prelude", "[std(\"not a name\")];\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'not a name' is not a C++ namespace name")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, AnOtherAttributeIsAnError)
{
    const hotc::TranslateResult r = check_translate("", "    int x;\n", {{"host.prelude", "[native];\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'native' is not allowed in a module attribute list")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, TheAttributeStdIsValidOnlyInTheList)
{
    const hotc::TranslateResult r =
        check_translate("", "    int x;\n", {{"host.prelude", "[std(\"mystd\"), native] class Foo2;\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'std' is valid only on a module attribute list")) << r.diagnostics;
}

namespace
{

// The 1-based line and column of the first match of the text in the generated C++.
SourcePos pos_of(const std::string& text, const std::string& needle)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos)
        return {0, 0};
    const int line = 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
    return {line, static_cast<int>(at - (text.rfind('\n', at) + 1)) + 1};
}

// A string literal with "$std::" is user text. It stays as it is, and the source map stays 1:1.
void expect_literal_is_kept(const std::vector<std::pair<std::string, std::string>>& preludes)
{
    const hotc::TranslateResult r = check_translate("",
        "    int n = 0;\n    void F() { std.string s = \"cost $std::x\"; this.n = 9; }\n", preludes);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "\"cost $std::x\"")) << r.cpp;
    // Script line 4: the "9" is at column 56, after the literal and the name "std.string".
    const SourcePos g = pos_of(r.cpp, "9;");
    ASSERT_GT(g.line, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(g.line, g.col), (SourcePos{4, 56})) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_generated(4, 56), g) << r.cpp;
}

} // namespace

TEST(ModuleAttrsTranslation, ALiteralWithDollarStdKeepsItsTextInTheDefaultMode)
{
    expect_literal_is_kept({});
}

TEST(ModuleAttrsTranslation, ALiteralWithDollarStdKeepsItsTextWithAnotherNamespace)
{
    expect_literal_is_kept({{"host.prelude", kMystd}});
    const hotc::TranslateResult r = check_translate(kMystd, "    void F() { std.string s = \"cost $std::x\"; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::string s = \"cost $std::x\"")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ACppScriptStringWithDollarStdKeepsItsText)
{
    TempDir dir;
    write_file(dir.path() / "w.h", std::string("#include <string>\nnamespace mystd = std;\n") + kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, "#include \"w.h\"\n"
                                                 "class A : public Widget { public: const char* s = \"cost $std::x\"; };\n");
    o.script_name = "t.cpp";
    o.preludes.emplace_back("host.prelude", kMystd);
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "\"cost $std::x\"")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheSourceMapStaysExactWithAnotherNamespace)
{
    const hotc::TranslateResult r = check_translate("",
        "    std.vector<std.string> v;\n    int n = 7;\n    void F() { std.string t = std.to_string(this.n); this.n = 9; }\n",
        {{"host.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // Script line 5: "    void F() { std.string t = std.to_string(this.n); this.n = 9; }".
    // "this.n = 9" starts at column 55. The names before it grow in the generated text.
    const SourcePos g = pos_of(r.cpp, "9;");
    ASSERT_GT(g.line, 0) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_generated(5, 63), g) << r.cpp;
    EXPECT_EQ(r.source_map.map_to_script(g.line, g.col), (SourcePos{5, 63})) << r.cpp;
}

TEST(ModuleAttrsTranslation, AnOverrideMatchesAParameterWithAnotherNamespace)
{
    const hotc::TranslateResult r = check_translate(
        "class Snk : SinkBase {\n    override void Take(std.vector<int> v) { }\n}\n", "    int x;\n",
        {{"host.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::vector<int32_t>")) << r.cpp;
}

// The byte 0x01 is the internal placeholder of the standard namespace. A script cannot hold it.
TEST(ModuleAttrsTranslation, TheByte0x01IsAnErrorInTheMainScript)
{
    const hotc::TranslateResult r = check_translate("", "    std.string s = \"a\x01 b\";\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the byte 0x01 is not allowed in a script")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, TheByte0x01IsAnErrorInAPrelude)
{
    const hotc::TranslateResult r = check_translate("", "    int x;\n", {{"host.prelude", "// a\x01 b\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the byte 0x01 is not allowed in a script")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "host.prelude")) << r.diagnostics;
}

// ---- the module attribute array: the template of the type "T[N]".

namespace
{

const char* kFixed = "[array(\"my::Fixed<$T, $N>\")];\n";

} // namespace

TEST(ModuleAttrsTranslation, TheArrayTypeIsStdArrayByDefault)
{
    const hotc::TranslateResult r = check_translate("", "    int[3] a;\n    void F() { int[2][4] b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::array<int32_t, 3> a;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::array<std::array<int32_t, 4>, 2> b")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheArrayTemplateOfThePreludeIsTheDefault)
{
    const hotc::TranslateResult r = check_translate(
        "", "    int[3] a;\n    void F() { int[2][4] b; std.vector<int[2]> v; }\n", {{"host.prelude", kFixed}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "my::Fixed<int32_t, 3> a;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "my::Fixed<my::Fixed<int32_t, 4>, 2> b")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::vector<my::Fixed<int32_t, 2>> v")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "std::array")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheArrayTemplateInTheScriptOverridesThePreludeDefault)
{
    const hotc::TranslateResult r = check_translate(
        "[array(\"other::Arr<$N, $T>\")];\n", "    int[3] a;\n", {{"host.prelude", kFixed}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "other::Arr<3, int32_t> a;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "my::Fixed")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheArrayTemplateInTheScriptWorksAfterTheFirstUse)
{
    // The attribute list comes after the class. The parser reads the file again with the template.
    const hotc::TranslateResult r =
        check_translate("", "    int[3] a;\n    void F() { int[2] b; }\n}\n[array(\"my::Fixed<$T, $N>\")];\nclass Last {\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "my::Fixed<int32_t, 3> a;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "my::Fixed<int32_t, 2> b")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ASyntaxErrorBeforeALateArrayAttributeIsReportedOnce)
{
    // The attribute list comes after the error. The parser reads the file twice, but the error appears once.
    const hotc::TranslateResult r = check_translate(
        "", "    int[3] a;\n    void F() { int y = ; }\n}\n[array(\"my::Fixed<$T, $N>\")];\nclass Last {\n");
    EXPECT_FALSE(r.ok);
    const std::string msg = "expected an expression";
    const size_t first = r.diagnostics.find(msg);
    ASSERT_NE(first, std::string::npos) << r.diagnostics;
    EXPECT_EQ(r.diagnostics.find(msg, first + 1), std::string::npos) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "t.hot:4:")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, ANameThatStartsWithDollarTIsAnError)
{
    const hotc::TranslateResult r = check_translate("[array(\"my::Fixed<$Tag, $N>\")];\n", "    int x;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "unknown '$' name")) << r.diagnostics;
    const hotc::TranslateResult n = check_translate("[array(\"my::Fixed<$T, $N1>\")];\n", "    int x;\n");
    EXPECT_FALSE(n.ok);
    EXPECT_TRUE(has(n.diagnostics, "unknown '$' name")) << n.diagnostics;
}

TEST(ModuleAttrsTranslation, DollarStdInTheArrayTemplateFollowsTheNamespace)
{
    const hotc::TranslateResult r = check_translate("[array(\"$std::array<$T, $N>\")];\n", "    int[3] a;\n",
        {{"host.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::array<int32_t, 3> a;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "\x01std::")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheDefaultArrayTemplateFollowsTheNamespace)
{
    const hotc::TranslateResult r = check_translate("", "    int[3] a;\n", {{"host.prelude", kMystd}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::array<int32_t, 3> a;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, TheArrayAndTheStdAttributeShareOneList)
{
    const hotc::TranslateResult r =
        check_translate("[std(\"mystd\"), array(\"$std::array<$T, $N>\")];\n", "    int[3] a;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::array<int32_t, 3> a;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, ARepeatedArrayAttributeIsAnError)
{
    const hotc::TranslateResult one = check_translate(std::string(kFixed) + kFixed, "    int x;\n");
    EXPECT_FALSE(one.ok);
    EXPECT_TRUE(has(one.diagnostics, "attribute 'array' is given twice in one file")) << one.diagnostics;
    const hotc::TranslateResult pre =
        check_translate("", "    int x;\n", {{"a.prelude", std::string(kFixed) + kFixed}});
    EXPECT_FALSE(pre.ok);
    EXPECT_TRUE(has(pre.diagnostics, "attribute 'array' is given twice in one file")) << pre.diagnostics;
}

// A later prelude overrides the array template of an earlier prelude.
TEST(ModuleAttrsTranslation, ALaterPreludeOverridesTheArrayOfAnEarlierOne)
{
    const hotc::TranslateResult r = check_translate("", "    int[3] a;\n",
        {{"a.prelude", kFixed}, {"b.prelude", "[array(\"$std::array<$T, $N>\")];\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::array<int32_t, 3> a;")) << r.cpp;
}

TEST(ModuleAttrsTranslation, AnArrayTemplateNeedsTheElementTypeAndTheSize)
{
    const hotc::TranslateResult no_n = check_translate("[array(\"my::Fixed<$T>\")];\n", "    int x;\n");
    EXPECT_FALSE(no_n.ok);
    EXPECT_TRUE(has(no_n.diagnostics, "the array template needs '$N' (the size)")) << no_n.diagnostics;
    const hotc::TranslateResult no_t = check_translate("[array(\"my::Fixed<int, $N>\")];\n", "    int x;\n");
    EXPECT_FALSE(no_t.ok);
    EXPECT_TRUE(has(no_t.diagnostics, "the array template needs '$T' (the element type)")) << no_t.diagnostics;
    const hotc::TranslateResult none = check_translate("[array(\"my::Fixed\")];\n", "    int x;\n");
    EXPECT_FALSE(none.ok);
    EXPECT_TRUE(has(none.diagnostics, "the array template needs '$T' (the element type) and '$N' (the size)"))
        << none.diagnostics;
    const hotc::TranslateResult bad = check_translate("[array(\"my::Fixed<$T, $N, $X>\")];\n", "    int x;\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "unknown '$' name")) << bad.diagnostics;
}

TEST(ModuleAttrsTranslation, TheAttributeArrayNeedsOneArgument)
{
    const hotc::TranslateResult r = check_translate("[array];\n", "    int x;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'array' takes 1 arguments")) << r.diagnostics;
}

TEST(ModuleAttrsTranslation, TheAttributeArrayIsValidOnlyInTheList)
{
    const hotc::TranslateResult r =
        check_translate("", "    int x;\n", {{"host.prelude", "[array(\"my::Fixed<$T, $N>\"), native] class Foo2;\n"}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "attribute 'array' is valid only on a module attribute list")) << r.diagnostics;
}

// The module attribute "lib": a standard-like library namespace next to std.
namespace
{
// A script with a second library "lib2" in its header. A namespace alias does not work here: the
// decl query of the translator finds the declarations of a real namespace only.
hotc::TranslateResult translate_lib2(const std::string& script, const std::vector<std::pair<std::string, std::string>>& preludes = {})
{
    static TempDir* dir = nullptr;
    if (!dir)
    {
        dir = new TempDir();
        write_file(dir->path() / "lib2.h",
            "#include <array>\n#include <cstddef>\n#include <vector>\n"
            "namespace lib2 { template <class T, class A = std::allocator<T>> struct vector : std::vector<T, A> {};\n"
            "template <class T, std::size_t N> struct array : std::array<T, N> {}; }\n"
            "namespace lib3 { template <class T> struct vector : std::vector<T> {}; }\n");
    }
    hotc::TranslateOptions o = temp_options(*dir, "using <vector>;\nusing \"lib2.h\";\n" + script);
    o.preludes = preludes;
    return hotc::translate(o);
}
} // namespace

TEST(ModuleAttrsTranslation, LibNamespaceGetsTheBoundsCheckOfStd)
{
    const hotc::TranslateResult r = translate_lib2("[lib(\"lib2\")];\nclass Foo {\n"
        "    std.vector<int> a;\n    lib2.vector<int> b;\n    lib2.array<int, 2> c;\n"
        "    int F() { return this.b[0] + this.a[0] + this.c[1]; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::vector<int32_t> a;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "lib2::vector<int32_t> b;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct checked_index< ::lib2::vector<A...>>")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct checked_index< ::lib2::array<T, N>>")) << r.cpp;
}

TEST(ModuleAttrsTranslation, LibWithoutTheDeclarationsWritesNoSpec)
{
    const hotc::TranslateResult r = translate_lib2("[lib(\"nolib\")];\nclass Foo {\n    int n;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "nolib")) << r.cpp;
}

TEST(ModuleAttrsTranslation, WithoutLibAnOtherVectorHasNoSpec)
{
    const hotc::TranslateResult r = translate_lib2("class Foo {\n    lib2.vector<int> b;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "checked_index< ::lib2")) << r.cpp;
}

TEST(ModuleAttrsTranslation, LibIsRepeatableInThePreludesAndTheScript)
{
    const hotc::TranslateResult r = translate_lib2("[lib(\"lib2\"), lib(\"lib3\")];\nclass Foo {\n    int n;\n}\n",
        {{"a.prelude", "[lib(\"lib3\")];\n"}, {"b.prelude", "[lib(\"lib2\")];\n"}});
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct checked_index< ::lib2::vector<A...>>")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct checked_index< ::lib3::vector<A...>>")) << r.cpp;
}

TEST(ModuleAttrsTranslation, LibSharedPtrHandleHasTheWeakPtrAndMakeShared)
{
    const hotc::TranslateResult r = check_translate(
        "[lib(\"mystd\")];\n[native, handle(\"mystd::shared_ptr<$T>\")] class Mesh;\n",
        "    [weak] Mesh w;\n    Mesh m;\n    void Make() { this.m = new Mesh(); this.w = this.m; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "mystd::weak_ptr<Mesh> w;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "mystd::make_shared<Mesh>()")) << r.cpp;
}

TEST(ModuleAttrsTranslation, WithoutLibAnOtherSharedPtrHasNoWeakHandle)
{
    const hotc::TranslateResult r = check_translate(
        "[native, handle(\"mystd::shared_ptr<$T>\")] class Mesh;\n", "    [weak] Mesh w;\n");
    EXPECT_FALSE(r.ok) << r.cpp;
}

TEST(ModuleAttrsTranslation, LibErrors)
{
    struct Case
    {
        const char* stubs;
        const char* msg;
    };
    for (const Case& c : {Case{"[lib(\"std\")];\n", "'lib(\"std\")' has no effect"},
             Case{"[lib(\"1x\")];\n", "'1x' is not a C++ namespace name"},
             Case{"[native, lib(\"mystd\")] class Mesh;\n", "attribute 'lib' is valid only on a module attribute list"}})
    {
        const hotc::TranslateResult r = check_translate(c.stubs, "    int n;\n");
        EXPECT_FALSE(r.ok) << c.stubs;
        EXPECT_TRUE(has(r.diagnostics, c.msg)) << r.diagnostics;
    }
}
