// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of class-scoped declarations that a game port needs: a class constant
// ("static const T Name = value;" in a class or a struct), a script class as the argument of a
// native template, and a module variable that holds a script class handle.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace translator_test;

const char* kHeader =
    "#include <limits>\n"
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "class RefCounted { public: virtual ~RefCounted() {} void AddRef() {} void ReleaseRef() {} };\n"
    "template <class T> class SharedPtr { public: SharedPtr() {} explicit SharedPtr(T* p) : p_(p) {}\n"
    "    template <class U> SharedPtr(const SharedPtr<U>& o) : p_(o.Get()) {}\n"
    "    T* Get() const { return p_; } T* operator->() const { return p_; } T& operator*() const { return *p_; }\n"
    "    explicit operator bool() const { return p_ != nullptr; } T* p_ = nullptr; };\n"
    "template <class T, class... A> SharedPtr<T> MakeShared(A&&... a) { return SharedPtr<T>(new T(a...)); }\n"
    "class Component : public RefCounted { public: virtual void Start() {} };\n"
    "class Node : public RefCounted { public:\n"
    "    template <class T> T* GetComponent() const { return nullptr; }\n"
    "    template <class T> T* CreateComponent() { return nullptr; }\n"
    "    int GetId() const { return 0; } };\n"
    "template <class T> class Signal;\n"
    "template <class... A> class Signal<void(A...)> { public: void operator()(A... a) const {} };\n";

const char* kStubs = "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"ReleaseRef\")] class Component;\n"
                     "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"ReleaseRef\")] class Node;\n";

hotc::TranslateOptions opts(const TempDir& dir, const std::string& script)
{
    write_file(dir.path() / "w.h", kHeader);
    return temp_options(dir, "using \"w.h\";\n" + script);
}

hotc::TranslateResult translate_script(const TempDir& dir, const std::string& script)
{
    return hotc::translate(opts(dir, script));
}

bool has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

// G01: a class constant and a struct constant. The output has a "static constexpr" at file scope.
// "Class.Name" and a bare name in the class give the constant. A case label and a default
// argument can use it.
TEST(ClassScopeTranslation, ClassConstantInClassAndStruct)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir,
        "enum Mode { Idle, Run }\n"
        "struct P { static const int N = 2; int v = N; }\n"
        "class Foo : Widget {\n"
        "    public static const int Max = 3;\n"
        "    static const float Scale = 0.5f;\n"
        "    static const bool On = true;\n"
        "    static const Mode Def = Mode.Run;\n"
        "    static const char* Label = \"foo\";\n"
        "    static const float Inf = std.numeric_limits<float>.infinity();\n"
        "    private static const int Twice = Max * 2;\n"
        "    int Pick(int v = Max) {\n"
        "        switch (v) { case Max: return Twice; case Foo.Max + 1: return P.N; default: return 0; }\n"
        "    }\n"
        "    float S() => On ? Scale * Foo.Max : Inf;\n"
        "}\n"
        "int Total() => Foo.Max + P.N;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static constexpr int32_t Foo__Max = 3;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr float Foo__Scale = 0.5f;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr bool Foo__On = true;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr Mode Foo__Def = Mode::Run;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr const char* Foo__Label = \"foo\";")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Inf = std::numeric_limits<float>::infinity();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr int32_t Foo__Twice = Foo__Max * 2;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr int32_t P__N = 2;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "case Foo__Max:")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "case Foo__Max + 1:")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return Foo__Twice;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "= Foo__Max")) << "the default argument" << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Max + P__N")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo.Max")) << r.cpp;
}

// G01: a local or a parameter with the name of a class constant hides the constant.
TEST(ClassScopeTranslation, LocalHidesClassConstant)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir,
        "class Foo : Widget {\n"
        "    static const int Max = 3;\n"
        "    int A(int Max) => Max + 1;\n"
        "    int B() { int Max = 7; return Max + Foo.Max; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Max + 1")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo__Max + 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Max + Foo__Max")) << r.cpp;
}

// G01: the errors of a class constant.
TEST(ClassScopeTranslation, ClassConstantErrors)
{
    TempDir dir;
    auto diag = [&](const std::string& script)
    {
        const hotc::TranslateResult r = translate_script(dir, script);
        EXPECT_FALSE(r.ok) << script;
        return r.diagnostics;
    };
    EXPECT_TRUE(has(diag("class Foo : Widget { const int Max = 1; }\n"),
        "a class member cannot be 'const'; for a class constant, write 'static const' before the type of 'Max'"));
    EXPECT_TRUE(has(diag("class Foo : Widget { static const int Max; }\n"), "the constant 'Max' needs a value"));
    EXPECT_TRUE(has(diag("class Foo : Widget { static int count = 0; }\n"), "a static field is not supported"));
    EXPECT_TRUE(has(diag("class Foo : Widget { static const int Max = 1; int Max = 2; }\n"),
        "'Max' is declared twice in 'Foo'"));
    EXPECT_TRUE(has(diag("class Foo : Widget { private static const int Max = 1; }\n"
                         "class Bar : Widget { int M() => Foo.Max; }\n"),
        "'Foo.Max' is private"));
    EXPECT_TRUE(has(diag("class Foo : Widget { protected static const int Max = 1; }\n"
                         "int F() => Foo.Max;\n"),
        "'Foo.Max' is protected"));
}

// G01: a class of an imported script. The importer reads the constant by the interface. A child
// class reads a public or protected constant of the parent by its bare name. A private constant is
// not in the interface. A new value changes the interface hash.
TEST(ClassScopeTranslation, ImportedClassConstant)
{
    TempDir dir;
    const std::string parent = "using \"w.h\";\nclass Base : Widget {\n"
                               "    static const int Cap = 4;\n"
                               "    protected static const int Step = Cap + 1;\n"
                               "    private static const int Secret = 9;\n"
                               "    virtual int Power() => Secret;\n"
                               "}\n";
    hotc::TranslateOptions o = opts(dir,
        "using \"base.hot\";\nclass Kid : Base {\n"
        "    override int Power() { switch (Step) { case Base.Cap: return Cap; default: return Step; } }\n"
        "}\nint F() => Base.Cap;\n");
    o.modules.emplace_back("base.hot", parent);
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static constexpr int32_t Base__Cap = 4;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static constexpr int32_t Base__Step = Base__Cap + 1;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Base__Secret")) << "a private constant stays in its module" << r.cpp;
    EXPECT_TRUE(has(r.cpp, "case Base__Cap:")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return Base__Step;")) << r.cpp;

    const hotc::TranslateResult a = translate_script(dir, parent.substr(std::string("using \"w.h\";\n").size()));
    std::string changed = parent;
    changed.replace(changed.find("Cap = 4"), 7, "Cap = 5");
    const hotc::TranslateResult b = translate_script(dir, changed.substr(std::string("using \"w.h\";\n").size()));
    ASSERT_TRUE(a.ok && b.ok) << a.diagnostics << b.diagnostics;
    EXPECT_NE(a.iface_hash, b.iface_hash);
    std::string secret = parent;
    secret.replace(secret.find("Secret = 9"), 10, "Secret = 8");
    const hotc::TranslateResult c = translate_script(dir, secret.substr(std::string("using \"w.h\";\n").size()));
    ASSERT_TRUE(c.ok) << c.diagnostics;
    EXPECT_EQ(a.iface_hash, c.iface_hash) << "a private constant is not in the interface";
}

// G09: a script class as the template argument of a native template. "GetComponent<C>()" gives a
// raw "C*". The raw pointer becomes the handle "$C" (the intrusive handle takes a reference).
// "MakeShared<C>()" gives the handle type of C. "$C" as the argument is the same as "C".
TEST(ClassScopeTranslation, ScriptClassAsNativeTemplateArgument)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir, std::string(kStubs) +
        "class PlayerStance : Component { int hp = 1; }\n"
        "class User : Component {\n"
        "    $PlayerStance kept;\n"
        "    int Run(Node* n) {\n"
        "        if (n == null) return 0;\n"
        "        $PlayerStance a = n.GetComponent<PlayerStance>();\n"
        "        $PlayerStance b = n.CreateComponent<PlayerStance>();\n"
        "        $PlayerStance c = MakeShared<PlayerStance>();\n"
        "        $PlayerStance d = n.GetComponent<$PlayerStance>();\n"
        "        this.kept = n.GetComponent<PlayerStance>();\n"
        "        var e = n.GetComponent<PlayerStance>();\n"
        "        if (a == null || b == null || c == null || d == null) return 0;\n"
        "        return a.hp + b.hp + c.hp + d.hp + n.GetId();\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string h = "hot::Intrusive<PlayerStance, &Component::AddRef, &Component::ReleaseRef>";
    EXPECT_TRUE(has(r.cpp, (h + " a = hot::handle_from_raw<" + h + ">(hot::deref(n).GetComponent<PlayerStance>());").c_str()))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, (h + " b = hot::handle_from_raw<" + h + ">(hot::deref(n).CreateComponent<PlayerStance>());").c_str()))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, (h + " c = hot::handle_from_raw<" + h + ">(MakeShared<PlayerStance>());").c_str())) << r.cpp;
    EXPECT_TRUE(has(r.cpp, (h + " d = hot::handle_from_raw<" + h + ">(hot::deref(n).GetComponent<PlayerStance>());").c_str()))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, ("__st->kept = hot::handle_from_raw<" + h + ">(").c_str())) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const auto e = hot::deref(n).GetComponent<PlayerStance>();")) << "a raw pointer local" << r.cpp;
    EXPECT_FALSE(has(r.cpp, "$PlayerStance")) << r.cpp;
}

// G09: a native template call that gives another script class cannot become the handle.
TEST(ClassScopeTranslation, ScriptClassTemplateArgumentOfAnotherClassIsAnError)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir, std::string(kStubs) +
        "class PlayerStance : Component { int hp = 1; }\n"
        "class Fast : PlayerStance { }\n"
        "class User : Component {\n"
        "    void Run(Node* n) {\n"
        "        if (n == null) return;\n"
        "        $PlayerStance ok = n.GetComponent<Fast>();\n"
        "        $PlayerStance bad = n.GetComponent<User>();\n"
        "    }\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics,
        "'GetComponent<User>' gives a 'User', and 'User' is not 'PlayerStance' or a script child of it: the value "
        "cannot become '$PlayerStance'"))
        << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "Fast")) << r.diagnostics;
}
