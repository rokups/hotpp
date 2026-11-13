// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

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

namespace
{

const char* kNsHeader =
    "#pragma once\n"
    "#include <string>\n"
    "#include <functional>\n"
    "namespace ns { namespace sub { struct Thing { int x = 0; }; } }\n"
    "namespace eastl { template <class T> struct vec { T v; }; }\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n";

hotc::TranslateResult translate_ns(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "n.h", kNsHeader);
    return hotc::translate(temp_options(dir, "using \"n.h\";\n" + script));
}

} // namespace

// "using ns.sub;" is "using namespace ns::sub;" at file scope, after the includes.
TEST(ImportTranslation, UsingNamespaceBecomesUsingDirective)
{
    const auto r = translate_ns("using ns.sub;\nclass Foo : Widget {\n    Thing t;\n    void f() { t.x = 3; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t inc = r.cpp.find("#include \"n.h\"\n");
    const size_t use = r.cpp.find("using namespace ns::sub;\n");
    const size_t cls = r.cpp.find("struct Foo_Shell");
    ASSERT_NE(inc, std::string::npos) << r.cpp;
    ASSERT_NE(use, std::string::npos) << r.cpp;
    EXPECT_LT(inc, use);
    EXPECT_LT(use, cls);
}

// Without the line, the output has no directive. The C++ compiler then reports the unknown name.
TEST(ImportTranslation, WithoutTheUsingLineThereIsNoDirective)
{
    const auto r = translate_ns("class Foo : Widget {\n    void f() { int a = 1; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("using namespace"), std::string::npos) << r.cpp;
}

// A namespace of a native library (the std placeholder) and a plain name.
TEST(ImportTranslation, UsingStdFollowsTheStdPlaceholder)
{
    const auto r = translate_ns("using std;\nusing std.placeholders;\nclass Foo : Widget {\n    int a;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("using namespace std;\n"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("using namespace std::placeholders;\n"), std::string::npos) << r.cpp;
}

// The three forms of "using" do not mix: a bad form is an error.
TEST(ImportTranslation, UsingWithBadArgumentIsAnError)
{
    const auto r = translate_ns("using 5;\nclass Foo : Widget {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("using needs"), std::string::npos) << r.diagnostics;
}

namespace
{

const char* kBareHeader =
    "#pragma once\n"
    "#include <memory>\n"
    "#include <exception>\n"
    "struct Glob { int g = 0; };\n"
    "namespace eng {\n"
    "struct Glob { int h = 0; };\n"
    "struct Err : std::exception {};\n"
    "enum class Mode { A, B };\n"
    "template <class T> void Make() {}\n"
    "template <class T> std::shared_ptr<T> MakeShared() { return std::make_shared<T>(); }\n"
    "class Base { public: virtual ~Base() {} virtual void Tick(float dt) {} };\n"
    "class Mid : public Base {};\n"
    "struct string { int s = 0; };\n"
    "struct uint { int u = 0; };\n"
    "struct Node : std::enable_shared_from_this<Node> { int x = 0; };\n"
    "template <class T> struct Ptr { T* p = nullptr; T* operator->() const { return p; } T* get() const { return p; } };\n"
    "template <class T, class... A> Ptr<T> MakeObj(A... a) { return Ptr<T>(); }\n"
    "struct Obj { void AddRef(); void Release(); virtual ~Obj() {} };\n"
    "struct Mat : Obj {};\n"
    "struct Two { int a = 0; };\n"
    "}\n"
    "namespace eng2 { struct Two { int b = 0; }; }\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n";

const char* kBareStubs = "[native, handle(\"std::shared_ptr<$T>\")] class eng.Node;\n"
                         "[native, handle(\"eng::Ptr<$T>\")] class eng.Obj;\n";

// The script gets "using eng;" and the stubs. "NAME" in the body stands for the type name.
hotc::TranslateResult translate_bare(const std::string& usings, const std::string& body, const std::string& name)
{
    TempDir dir;
    write_file(dir.path() / "b.h", kBareHeader);
    std::string text = body;
    for (size_t at = text.find("NAME"); at != std::string::npos; at = text.find("NAME", at))
    {
        text.replace(at, 4, name);
        at += name.size();
    }
    return hotc::translate(temp_options(dir, "using \"b.h\";\n" + usings + kBareStubs + text));
}

// The bare name and the qualified name give the same C++.
void expect_same_as_qualified(const std::string& body, const std::string& bare, const std::string& qualified,
    const char* expected_part = nullptr)
{
    const auto a = translate_bare("using eng;\n", body, bare);
    const auto b = translate_bare("using eng;\n", body, qualified);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp);
    if (expected_part)
        EXPECT_NE(a.cpp.find(expected_part), std::string::npos) << a.cpp;
}

} // namespace

// A bare handle "$Obj" has the handle template of the stub "eng.Obj".
TEST(ImportTranslation, BareHandleUsesTheNamespacedStub)
{
    expect_same_as_qualified(
        "class Foo : Widget {\n    $NAME o;\n    void f($NAME p) { o = p; }\n}\n", "Obj", "eng.Obj", "eng::Ptr<eng::Obj>");
}

// "[weak]" works with a bare name.
TEST(ImportTranslation, BareWeakHandle)
{
    expect_same_as_qualified("class Foo : Widget {\n    [weak]\n    $NAME w;\n    void f([weak] $NAME p) { w = p; }\n}\n",
        "Node", "eng.Node", "std::weak_ptr<eng::Node>");
}

// "new" with a bare name.
TEST(ImportTranslation, BareNew)
{
    expect_same_as_qualified("class Foo : Widget {\n    void f() { $NAME o = new NAME(); }\n}\n", "Obj", "eng.Obj");
}

// A bare name in a template argument and in a local.
TEST(ImportTranslation, BareNameInTemplateArgument)
{
    expect_same_as_qualified("class Foo : Widget {\n    void f() { std.vector<$NAME> v; }\n}\n", "Node", "eng.Node");
}

// Two namespaces with the same name: the script must write the prefix.
TEST(ImportTranslation, BareNameInTwoNamespacesIsAnError)
{
    const std::string body = "class Foo : Widget {\n    NAME t;\n}\n";
    const auto r = translate_bare("using eng;\nusing eng2;\n", body, "Two");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'Two'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("prefix"), std::string::npos) << r.diagnostics;
    const auto q = translate_bare("using eng;\nusing eng2;\n", body, "eng2.Two");
    EXPECT_TRUE(q.ok) << q.diagnostics;
}

// A script class wins over a native name of a namespace.
TEST(ImportTranslation, ScriptClassWinsOverBareNativeName)
{
    const auto r = translate_bare("using eng;\n", "class Node {\n    int x;\n}\nclass Foo : Widget {\n    $Node n;\n}\n", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("std::shared_ptr<eng::Node>"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("struct Node_Shell;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::shared_ptr<Node> n;"), std::string::npos) << r.cpp;
}

// The catch type, a static property and the explicit template arguments of a call.
TEST(ImportTranslation, BareCatchType)
{
    expect_same_as_qualified(
        "class Foo : Widget {\n    void f() { try { } catch (NAME e) { } }\n}\n", "Err", "eng.Err", "eng::Err");
}

TEST(ImportTranslation, BareStaticPropertyType)
{
    expect_same_as_qualified("class Foo : Widget {\n    static NAME P { get; set; }\n}\n", "Two", "eng.Two", "eng::Two");
}

TEST(ImportTranslation, BareTemplateArgumentOfACall)
{
    expect_same_as_qualified("class Foo : Widget {\n    void f() { Make<NAME>(); }\n}\n", "Node", "eng.Node",
        "Make<eng::Node>");
}

// A native enum type.
TEST(ImportTranslation, BareEnumType)
{
    expect_same_as_qualified("class Foo : Widget {\n    NAME m;\n}\n", "Mode", "eng.Mode", "eng::Mode m");
}

// A global native name wins, as in C++.
TEST(ImportTranslation, GlobalNativeNameWinsOverTheNamespace)
{
    const auto r = translate_bare("using eng;\n", "class Foo : Widget {\n    Glob g;\n}\n", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("eng::Glob"), std::string::npos) << r.cpp;
}

// A builtin script name stays, also when a namespace declares the same name.
TEST(ImportTranslation, BuiltinNameIsNotRewritten)
{
    const auto r = translate_bare("using eng;\n", "class Foo : Widget {\n    string s;\n    uint u;\n}\n", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("eng::string"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("eng::uint"), std::string::npos) << r.cpp;
}

// A local variable or a method with the name of a type is an expression name: no rewrite.
TEST(ImportTranslation, ExpressionNamesAreNotRewritten)
{
    const auto r = translate_bare("using eng;\n",
        "class Foo : Widget {\n    void Obj() { }\n    void f() { int Node = 1; Node = 2; Obj(); }\n}\n", "");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("Node = 2"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("eng::Node = 2"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("eng::Obj("), std::string::npos) << r.cpp;
}

// A name with a stub keeps its meaning.
TEST(ImportTranslation, StubNameIsNotRewritten)
{
    TempDir dir;
    write_file(dir.path() / "b.h", kBareHeader);
    const auto r = hotc::translate(temp_options(dir,
        "using \"b.h\";\nusing eng;\n[native, handle(\"std::shared_ptr<$T>\")] class Node;\n"
        "class Foo : Widget {\n    $Node n;\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("std::shared_ptr<eng::Node> n"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::shared_ptr<Node> n"), std::string::npos) << r.cpp;
}

// An imported script class keeps its meaning.
TEST(ImportTranslation, ImportedScriptClassIsNotRewritten)
{
    TempDir dir;
    write_file(dir.path() / "b.h", kBareHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"b.h\";\nusing \"lib.hot\";\nusing eng;\nclass Foo : Widget {\n    $Node n;\n}\n");
    o.modules.emplace_back("lib.hot", "using \"b.h\";\nclass Node : Widget {\n    int x;\n}\n");
    const auto r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("eng::Node> n"), std::string::npos) << r.cpp;
}

// A ".cpp" script is plain C++: the translator rewrites nothing.
TEST(ImportTranslation, CppScriptIsSkipped)
{
    TempDir dir;
    write_file(dir.path() / "b.h", kBareHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "#include \"b.h\"\nusing namespace eng;\nclass Foo : public Widget\n{\npublic:\n    Two n;\n};\n");
    o.script_name = "t.cpp";
    const auto r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("Two n;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("eng::Two n"), std::string::npos) << r.cpp;
}

// Without a "using" line a bare name stays as written.
TEST(ImportTranslation, NoUsingLineNoRewrite)
{
    const auto r = translate_bare("", "class Foo : Widget {\n    int x;\n    Two t;\n}\n", "");
    EXPECT_EQ(r.cpp.find("eng::Two"), std::string::npos) << r.cpp;
}

// A second translate with the same session needs no new parse: the questions of the bare names
// are part of the cache.
TEST(ImportTranslation, BareNamesUseTheSessionCache)
{
    TempDir dir;
    write_file(dir.path() / "b.h", kBareHeader);
    hotc::TranslatorSession session;
    const std::string text =
        "using \"b.h\";\nusing eng;\n" + std::string(kBareStubs) + "class Foo : Widget {\n    $Obj o;\n    Two t;\n}\n";
    const auto a = hotc::translate(temp_options(dir, text), session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const auto b = hotc::translate(temp_options(dir, text), session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_EQ(session.parse_count(), 1);
}

// A bare class base gets the stubs of the base chain (the prelude of "class : eng.Base;").
TEST(ImportTranslation, BareClassBaseGetsTheStubOfItsBase)
{
    expect_same_as_qualified("[native, cpp_prelude(\"int pre_text = 1;\")] class : eng.Base;\n"
                             "class Foo : NAME {\n    int x;\n}\n",
        "Mid", "eng.Mid", "int pre_text = 1;");
}

// The type of a cast.
TEST(ImportTranslation, BareCastType)
{
    expect_same_as_qualified("class Foo : Widget {\n    void f($NAME a) { var b = ($NAME)a; }\n}\n", "Node", "eng.Node");
}

// A free function of an imported script keeps the bare handle type of that script. The "using"
// line of the import counts for it.
TEST(ImportTranslation, BareTypeInAFreeFunctionOfAnImport)
{
    const auto run = [](const std::string& type)
    {
        TempDir dir;
        write_file(dir.path() / "b.h", kBareHeader);
        hotc::TranslateOptions o = temp_options(dir,
            "using \"b.h\";\nusing \"lib.hot\";\n" + std::string(kBareStubs)
                + "class Foo : Widget {\n    void g($eng.Node n) { F(n); }\n}\n");
        o.modules.emplace_back("lib.hot",
            "using \"b.h\";\nusing eng;\n" + std::string(kBareStubs) + "void F($" + type + " n) { }\n");
        return hotc::translate(o);
    };
    const auto a = run("Node");
    const auto b = run("eng.Node");
    ASSERT_TRUE(b.ok) << b.diagnostics;
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp);
}

// A static handle field with "MakeShared<T>()" as the initializer.
TEST(ImportTranslation, BareTypeInAStaticFieldWithMakeShared)
{
    expect_same_as_qualified(
        "class Foo : Widget {\n    private static $NAME m = MakeShared<NAME>();\n    void f() { var x = m; }\n}\n",
        "Node", "eng.Node");
}

// The same with a handle template that is not std::shared_ptr and a call with arguments.
TEST(ImportTranslation, BareTypeInAStaticFieldWithAHandleTemplate)
{
    expect_same_as_qualified(
        "class Foo : Widget {\n    private static $NAME m = MakeObj<NAME>(1);\n    void f() { var x = m; }\n}\n",
        "Mat", "eng.Mat");
}

// A bare base, a bare static handle field and a bare "MakeShared<T>(...)" in one class.
TEST(ImportTranslation, BareBaseAndStaticFieldTogether)
{
    expect_same_as_qualified("[native, cpp_prelude(\"int pre_text = 1;\")] class : eng.Base;\n"
                             "class Foo : Mid {\n    private static $Mat m = MakeObj<Mat>(1);\n"
                             "    private static $NAME n = MakeShared<NAME>();\n    void f() { var x = m; }\n}\n",
        "Node", "eng.Node", "int pre_text = 1;");
}

// A static handle field that a method assigns. The handle type of the assignment comes from the
// type of the field.
TEST(ImportTranslation, BareStaticHandleFieldAssignedInAMethod)
{
    expect_same_as_qualified("class Foo : Widget {\n    private static $NAME m = null;\n"
                             "    void f() { m = MakeShared<NAME>(); }\n}\n",
        "Obj", "eng.Obj", "eng::Ptr<eng::Obj>");
}

// A type alias with a bare native type, at file scope and in a class body.
TEST(ImportTranslation, BareTypeInATypeAlias)
{
    expect_same_as_qualified("using P = NAME;\nclass Foo : Widget {\n    using Q = NAME;\n    P x;\n    Q y;\n}\n",
        "Two", "eng.Two", "using P = eng::Two;");
}

// A module constant with a bare native type.
TEST(ImportTranslation, BareTypeInAModuleConstant)
{
    expect_same_as_qualified("const $NAME kC = null;\nclass Foo : Widget {\n    void f() { var a = kC; }\n}\n",
        "Node", "eng.Node");
}
