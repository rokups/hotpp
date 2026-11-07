// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of native interop that a game port needs: a logger call, a native signal,
// a native constexpr call in a constant, a protected native virtual, and a null test of a function.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace translator_test;

const char* kNativeHeader =
    "#include <functional>\n"
    "#include <limits>\n"
    "#include <string>\n"
    "#include <utility>\n"
    "namespace Urho3D {\n"
    "class Logger {\n"
    "public:\n"
    "    template <class... Args> void Error(const char* format, Args&&... args) {}\n"
    "    template <class... Args> void Info(const char* format, Args&&... args) {}\n"
    "};\n"
    "class Log { public: static Logger& GetLogger() { static Logger l; return l; } };\n"
    "template <class T> class Signal;\n"
    "template <class... Args> class Signal<void(Args...)> {\n"
    "public:\n"
    "    template <class Owner, class F> void Subscribe(Owner* owner, F&& f) {}\n"
    "    template <class Owner> void Unsubscribe(Owner* owner) {}\n"
    "    void operator()(Args... args) const {}\n"
    "};\n"
    "}\n"
    "class Widget {\n"
    "public:\n"
    "    virtual ~Widget() {}\n"
    "    virtual void Update(float dt) {}\n"
    "protected:\n"
    "    virtual void OnSceneSet(int scene) {}\n"
    "};\n";

hotc::TranslateResult translate_native(const TempDir& dir, const std::string& script)
{
    write_file(dir.path() / "w.h", kNativeHeader);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + script));
}

bool has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

// G03: a logger field and a variadic native template call with format arguments.
TEST(NativeGapTranslation, LoggerCallWithFormatArguments)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_native(dir,
        "class Foo : Widget {\n"
        "    protected var logger = Urho3D.Log.GetLogger();\n"
        "    std.string name = \"a\";\n"
        "    void M() { logger.Error(\"x {}\", name); logger.Info(\"y {} {}\", 1, 2.5); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::remove_cv_t<std::remove_reference_t<decltype(Urho3D::Log::GetLogger())>> logger;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Urho3D::Log::GetLogger()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Error(\"x {}\"")) << r.cpp;
}

// G04: a native signal field, Subscribe with a lambda, Unsubscribe and an invoke.
TEST(NativeGapTranslation, NativeSignalSubscribeUnsubscribeAndInvoke)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_native(dir,
        "class Foo : Widget {\n"
        "    Urho3D.Signal<void(int)> OnChanged;\n"
        "    int last = 0;\n"
        "    void Start() { OnChanged.Subscribe(this, (int id) => { this.last = id; }); }\n"
        "    void Stop() { OnChanged.Unsubscribe(this); }\n"
        "    void Fire() { OnChanged(3); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Subscribe(")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Unsubscribe(")) << r.cpp;
}

// G07: a native constexpr call in a module constant.
TEST(NativeGapTranslation, NativeConstexprCallInModuleConstant)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_native(dir,
        "const float Inf = std.numeric_limits<float>.infinity();\n"
        "const int Big = std.numeric_limits<int>.max();\n"
        "const std.string Empty = std.string();\n"
        "class Foo : Widget {\n"
        "    float M() { return Inf; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::numeric_limits<float>::infinity()")) << r.cpp;
}

// G12: a protected override of a native virtual that calls the base.
TEST(NativeGapTranslation, ProtectedOverrideOfANativeVirtual)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_native(dir,
        "class Foo : Widget {\n"
        "    int n = 0;\n"
        "    protected override void OnSceneSet(int scene) { base.OnSceneSet(scene); this.n = scene; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "__self->__hot_base_OnSceneSet(scene);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__hot_base_OnSceneSet(int a0)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return Widget::OnSceneSet(a0);")) << r.cpp;
}

// G13: a null test of a std.function and an assignment of null.
TEST(NativeGapTranslation, NullTestOfAFunction)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_native(dir,
        "class Foo : Widget {\n"
        "    std.function<int()> f;\n"
        "    int M() {\n"
        "        std.function<int()> g = null;\n"
        "        if (f == null) return 0;\n"
        "        if (f != null && g == null) return f();\n"
        "        f = null;\n"
        "        return 1;\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "if (!f) return 0;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static_cast<bool>(f) && !g")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "f = nullptr;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "== nullptr")) << r.cpp;
}

// G07: the constants of an imported module can have a native constexpr call. The importer uses them.
TEST(NativeGapTranslation, ImportedConstantWithANativeConstexprCall)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kNativeHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"w.h\";\nusing \"lib.hot\";\nclass Foo : Widget {\n    float M() { return Inf + Big; }\n}\n");
    o.modules.emplace_back("lib.hot",
        "using \"w.h\";\nconst float Inf = std.numeric_limits<float>.infinity();\n"
        "const int Big = std.numeric_limits<int>.max();\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::numeric_limits<float>::infinity()")) << r.cpp;
}

// G14: two imported modules import each other. The classes use each other by a handle field and by a call.
const char* kCycleA =
    "using \"w.h\";\nusing \"b.hot\";\n"
    "class Ca : Widget {\n    int hp = 1;\n    $Cb other;\n"
    "    int Power() { var t = other; if (t != null) return t.Twice(hp); return 0; }\n"
    "    int Plain() { return hp; }\n}\n";

const char* kCycleB =
    "using \"w.h\";\nusing \"a.hot\";\n"
    "class Cb : Widget {\n    $Ca back;\n    int Twice(int x) { return x * 2; }\n"
    "    int Peek() { var t = back; if (t != null) return t.Plain(); return 0; }\n}\n";

TEST(NativeGapTranslation, ImportCycleOfTwoImportedModules)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kNativeHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"w.h\";\nusing \"a.hot\";\nclass Main : Widget {\n    $Ca a;\n"
        "    int Run() { var t = a; if (t != null) return t.Power(); return 0; }\n}\n");
    o.modules.emplace_back("a.hot", kCycleA);
    o.modules.emplace_back("b.hot", kCycleB);
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.imports.size(), 2u);
    EXPECT_TRUE(has(r.cpp, "struct Ca_Shell")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Cb_Shell")) << r.cpp;
}

// Each module of the cycle translates as the main script too.
TEST(NativeGapTranslation, ImportCycleTranslatesFromEachModule)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kNativeHeader);
    hotc::TranslateOptions oa = temp_options(dir, kCycleA);
    oa.script_name = "a.hot";
    oa.modules.emplace_back("b.hot", kCycleB);
    const hotc::TranslateResult ra = hotc::translate(oa);
    ASSERT_TRUE(ra.ok) << ra.diagnostics;
    hotc::TranslateOptions ob = temp_options(dir, kCycleB);
    ob.script_name = "b.hot";
    ob.modules.emplace_back("a.hot", kCycleA);
    const hotc::TranslateResult rb = hotc::translate(ob);
    ASSERT_TRUE(rb.ok) << rb.diagnostics;
}

// A struct that holds a struct of the cycle by value stays an error.
TEST(NativeGapTranslation, ImportCycleOfStructsByValueIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kNativeHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"w.h\";\nusing \"sa.hot\";\nclass M : Widget { SA s; }\n");
    o.modules.emplace_back("sa.hot", "using \"w.h\";\nusing \"sb.hot\";\nstruct SA { SB b; int x = 0; }\n");
    o.modules.emplace_back("sb.hot", "using \"w.h\";\nusing \"sa.hot\";\nstruct SB { SA a; int y = 0; }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the structs use each other in a cycle")) << r.diagnostics;
}

} // namespace
