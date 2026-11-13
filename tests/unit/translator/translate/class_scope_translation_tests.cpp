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

// G23: a static field is a module variable of the owner with the name "Class__Name". "Class.Name"
// and a bare name in the class (also in a static method and a lambda) give it. A struct can have one.
TEST(ClassScopeTranslation, StaticFieldInClassAndStruct)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir,
        "struct P { static int made = 0; static int Next() { made++; return made; } }\n"
        "class Foo : Widget {\n"
        "    static int count_;\n"
        "    public static float scale = 0.5f;\n"
        "    private static int hidden_ = 3;\n"
        "    override void Update(float dt) {\n"
        "        count_++;\n"
        "        Foo.count_ += 2;\n"
        "        hidden_ = count_ + P.made;\n"
        "        std.function<void()> f = () => { count_ = 9; };\n"
        "        f();\n"
        "    }\n"
        "    static int Get() => count_ + hidden_;\n"
        "    int Hide(int count_) => count_ + 1;\n"
        "}\n"
        "int Total() => Foo.count_ + P.Next();\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t Foo__count_;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "float Foo__scale;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t P__made;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot_mod()->Foo__count_++")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot_mod()->Foo__count_ += 2")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot_mod()->Foo__count_ = 9")) << "a lambda body" << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot_mod()->Foo__hidden_ = hot_mod()->Foo__count_ + hot_mod()->P__made")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot_mod()->Foo__count_ + hot_mod()->Foo__hidden_;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return count_ + 1;")) << "a parameter hides the static field" << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo.count_")) << r.cpp;
}

// G23: the errors of a static field. The interface hash changes when a public static field is
// added, removed or retyped. A private static field is not in the interface.
TEST(ClassScopeTranslation, StaticFieldErrorsAndInterface)
{
    TempDir dir;
    auto diag = [&](const std::string& script)
    {
        const hotc::TranslateResult r = translate_script(dir, script);
        EXPECT_FALSE(r.ok) << script;
        return r.diagnostics;
    };
    EXPECT_TRUE(has(diag("class Foo : Widget { static int n = 0; int n = 1; }\n"), "'n' is declared twice in 'Foo'"));
    EXPECT_TRUE(has(diag("class Foo : Widget { private static int n = 0; }\n"
                         "class Bar : Widget { int M() => Foo.n; }\n"),
        "'Foo.n' is private"));
    EXPECT_TRUE(has(diag("class Foo : Widget { protected static int n = 0; }\n"
                         "int F() => Foo.n;\n"),
        "'Foo.n' is protected"));
    auto hash = [&](const std::string& script)
    {
        const hotc::TranslateResult r = translate_script(dir, script);
        EXPECT_TRUE(r.ok) << r.diagnostics;
        return r.iface_hash;
    };
    const auto base = hash("class Foo : Widget { int k = 0; }\n");
    const auto added = hash("class Foo : Widget { int k = 0; static int n = 0; }\n");
    const auto retyped = hash("class Foo : Widget { int k = 0; static float n = 0.0f; }\n");
    const auto priv = hash("class Foo : Widget { int k = 0; private static int n = 0; }\n");
    const auto prop = hash("class Foo : Widget { int k = 0; static int N { get; set; } }\n");
    EXPECT_NE(base, added);
    EXPECT_NE(added, retyped);
    EXPECT_NE(base, prop);
    EXPECT_EQ(base, priv) << "a private static field is not in the interface";
}

// G23: "static" at file level. A module variable and a free function are the static forms there.
TEST(ClassScopeTranslation, StaticAtFileLevelGivesTheForm)
{
    TempDir dir;
    auto diag = [&](const std::string& script)
    {
        const hotc::TranslateResult r = translate_script(dir, script);
        EXPECT_FALSE(r.ok) << script;
        return r.diagnostics;
    };
    EXPECT_TRUE(has(diag("static int total = 0;\n"), "a module variable is static"));
    EXPECT_TRUE(has(diag("static int F() => 1;\n"), "a free function cannot be 'static'"));
}

// G23: a static property. The parser makes static methods "get_Name" and "set_Name". A read is a
// call of the getter. A write statement is a call of the setter. "get; set;" has a hidden static field.
TEST(ClassScopeTranslation, StaticProperty)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir,
        "class Foo : Widget {\n"
        "    static int count_ = 0;\n"
        "    static int Count { get { return count_; } set { count_ = value; } }\n"
        "    static int Auto { get; private set; } = 4;\n"
        "    static int Twice => count_ * 2;\n"
        "    override void Update(float dt) {\n"
        "        Count = 3;\n"
        "        Count += 2;\n"
        "        Foo.Count++;\n"
        "        int a = Count + Foo.Twice;\n"
        "        Auto = a;\n"
        "        std.function<void()> f = () => { Foo.Count = Auto; };\n"
        "        f();\n"
        "    }\n"
        "}\n"
        "int F() { Foo.Count = 7; return Foo.Count + Foo.Auto; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Foo__s_set_Count(3)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__s_set_Count(Foo__s_get_Count() + (2))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__s_set_Count(Foo__s_get_Count() + 1)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__s_get_Count() + Foo__s_get_Twice()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__s_set_Auto(a)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__s_set_Count(Foo__s_get_Auto())")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo____p_Auto")) << "the hidden field of 'get; set;'" << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Foo.Count")) << r.cpp;
}

// G23: the errors of a static property.
TEST(ClassScopeTranslation, StaticPropertyErrors)
{
    TempDir dir;
    auto diag = [&](const std::string& script)
    {
        const hotc::TranslateResult r = translate_script(dir, script);
        EXPECT_FALSE(r.ok) << script;
        return r.diagnostics;
    };
    const std::string foo = "class Foo : Widget { static int n = 0; static int N { get { return n; } private set { n = value; } }"
                            " static int R => n; }\n";
    EXPECT_TRUE(has(diag(foo + "int F() { Foo.R = 1; return 0; }\n"), "'Foo.R' has no 'set'"));
    EXPECT_TRUE(has(diag(foo + "int F() { int a = (Foo.N = 1); return a; }\n"),
        "a write of the static property 'Foo.N' must be a statement"));
    EXPECT_TRUE(has(diag(foo + "int F() { Foo.N = 1; return 0; }\n"), "private"));
    EXPECT_TRUE(has(diag("class Foo : Widget { static int N { get; set; } int N = 1; }\n"),
        "'N' is declared twice in 'Foo'"));
}

// G23: a static field, a static property and a static method of an imported class. The owner
// module holds the storage: the importer uses hot::mod_var and hot::call_fn.
TEST(ClassScopeTranslation, ImportedStaticMembers)
{
    TempDir dir;
    const std::string lib = "class Counter {\n"
                            "    static int count_ = 0;\n"
                            "    private static int secret_ = 1;\n"
                            "    static int Count { get { return count_ + secret_; } set { count_ = value; } }\n"
                            "    static int Bump(int by) { count_ += by; return count_; }\n"
                            "}\n";
    hotc::TranslateOptions o = opts(dir,
        "using \"lib.hot\";\nclass User : Widget {\n"
        "    override void Update(float dt) { Counter.count_ += 1; Counter.Count = Counter.Count + Counter.Bump(2); }\n"
        "}\n");
    o.modules.emplace_back("lib.hot", lib);
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot_mv_Counter__count_() += 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::mod_var<int32_t>(hot::unit_ref(\"lib\"), __site, \"Counter__count_\")")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Counter__s_set_Count(Counter__s_get_Count() + Counter__s_Bump(2))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "\"Counter::get_Count\"")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "secret_")) << "a private static field stays in its module" << r.cpp;
}

// G23: a static field of an imported struct. The owner module holds it, as for a class.
TEST(ClassScopeTranslation, ImportedStructStaticField)
{
    TempDir dir;
    const std::string lib = "struct Tally {\n"
                            "    static int n = 0;\n"
                            "    private static int step_ = 1;\n"
                            "    static int Next() { n += step_; return n; }\n"
                            "}\n";
    hotc::TranslateOptions o = opts(dir,
        "using \"lib.hot\";\nclass User : Widget {\n"
        "    override void Update(float dt) { Tally.n += Tally.Next(); }\n"
        "}\n");
    o.modules.emplace_back("lib.hot", lib);
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot_mv_Tally__n() += Tally__s_Next()")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Tally__step_")) << r.cpp;
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

// G11: a module variable can hold handles of script classes: "$C", "std.vector<$C>" and a native
// signal with a "$C" parameter. "$C" in the native template is the handle type of C.
TEST(ClassScopeTranslation, ModuleVariableHoldsScriptClassHandles)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir,
        "using <vector>;\n"
        "class PlayerScore { int points = 0; }\n"
        "std.vector<$PlayerScore> scores;\n"
        "$PlayerScore best;\n"
        "Signal<void($PlayerScore)> changed;\n"
        "Signal<void(PlayerScore*)> seen;\n"
        "class Board : Widget {\n"
        "    void Add() { var s = new $PlayerScore(); scores.push_back(s); best = s; changed(s); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::vector<std::shared_ptr<PlayerScore>> scores")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Signal<void(std::shared_ptr<PlayerScore>)> changed")) << r.cpp;
}

// G11: the errors. A bare script class is a value box, and a raw pointer has no owner: a module
// variable cannot hold them. A module variable initializer cannot create a script object.
TEST(ClassScopeTranslation, ModuleVariableScriptClassErrors)
{
    TempDir dir;
    auto diag = [&](const std::string& script)
    {
        const hotc::TranslateResult r = translate_script(dir, "class PlayerScore { int points = 0; }\n" + script);
        EXPECT_FALSE(r.ok) << script;
        return r.diagnostics;
    };
    EXPECT_TRUE(has(diag("PlayerScore one;\n"), "a module variable cannot hold a value of a script class"));
    EXPECT_TRUE(has(diag("using <vector>;\nstd.vector<PlayerScore*> raw;\n"),
        "a module variable can hold a script class only as a handle: write '$PlayerScore'"));
    EXPECT_TRUE(has(diag("$PlayerScore best = new $PlayerScore();\n"),
        "a module variable initializer cannot create the script object 'PlayerScore'"));
}

} // namespace
