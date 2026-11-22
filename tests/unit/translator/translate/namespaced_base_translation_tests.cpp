// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with a native base class in a namespace. The base spells the types
// of its virtual methods without the namespace. A derived native class overrides a virtual method
// with a qualified spelling of the same type (as the URHO3D_OBJECT macro of rbfx does).
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kNamespacedHeader =
    "namespace eng {\n"
    "struct Key { int v = 0; };\n"
    "class Base { public: virtual ~Base() {} virtual bool Is(Key k) const = 0;\n"
    "    virtual Key Get() { return Key(); } virtual void Tick(float dt) {} };\n"
    "class Mid : public Base { public: using Base::Is; bool Is(eng::Key k) const override { return k.v == 1; } };\n"
    "}\n";

hotc::TranslateResult run(const std::string& classes)
{
    TempDir dir;
    write_file(dir.path() / "eng.h", kNamespacedHeader);
    return hotc::translate(temp_options(dir, "using \"eng.h\";\n" + classes));
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

// The override "Is(eng::Key)" of Mid replaces the pure virtual "Is(Key)" of Base. The spellings
// differ, but the canonical types are the same.
TEST(NamespacedBaseTranslation, OverrideWithAnotherSpellingIsNotPure)
{
    const hotc::TranslateResult r = run("class Foo : eng.Mid {\n    override void Tick(float dt) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "does not override the pure virtual method")) << r.diagnostics;
}

// The generated code is outside the namespace, so it spells the types of the base fully qualified.
TEST(NamespacedBaseTranslation, GeneratedCodeQualifiesTheTypesOfTheBase)
{
    const hotc::TranslateResult r = run("class Foo : eng.Mid {\n    override eng.Key Get() { return eng.Key(); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "bool Is(eng::Key a0) const override")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "eng::Key Get() override")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "(Key ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, " Key Get(")) << r.cpp;
}

// A bare base name under "using eng;" is the base "eng::Mid".
TEST(NamespacedBaseTranslation, BareBaseNameUnderUsing)
{
    const hotc::TranslateResult a = run("using eng;\nclass Foo : Mid {\n    override void Tick(float dt) { }\n}\n");
    const hotc::TranslateResult b = run("using eng;\nclass Foo : eng.Mid {\n    override void Tick(float dt) { }\n}\n");
    ASSERT_TRUE(b.ok) << b.diagnostics;
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp);
}

// A bare parameter type of an override is the type of the native virtual method.
TEST(NamespacedBaseTranslation, BareOverrideParameterUnderUsing)
{
    const hotc::TranslateResult a = run("using eng;\nclass Foo : Mid {\n    override bool Is(Key k) { return true; }\n}\n");
    const hotc::TranslateResult b = run("using eng;\nclass Foo : eng.Mid {\n    override bool Is(eng.Key k) { return true; }\n}\n");
    ASSERT_TRUE(b.ok) << b.diagnostics;
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_FALSE(has(a.diagnostics, "hides the virtual method")) << a.diagnostics;
}

// HOT-80: a recursive class template base has no end. The translator reports an error and does not crash.
TEST(NamespacedBaseTranslation, RecursiveTemplateBaseIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "chain.h",
        "template <int N> class Chain : public Chain<N - 1> { public: virtual ~Chain() {} };\n"
        "template <> class Chain<0> { public: virtual ~Chain() {} };\n"
        "struct Base : Chain<2> { };\n");
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, "using \"chain.h\";\nclass S : Base { }\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "levels of bases")) << r.diagnostics;
}

// HOT-90: a class template that is declared with "struct" keeps its virtual methods.
TEST(NamespacedBaseTranslation, StructTemplateBaseKeepsVirtuals)
{
    TempDir dir;
    write_file(dir.path() / "st.h",
        "template <int N> struct T1 { virtual void f() {} virtual ~T1() {} };\n"
        "struct Base : T1<2> { };\n");
    const hotc::TranslateResult r =
        hotc::translate(temp_options(dir, "using \"st.h\";\nclass S : Base {\n    override void f() { }\n}\n"));
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.diagnostics, "no virtual destructor")) << r.diagnostics;
}

// HOT-90: two namespaces have a class "Node". The base "b.Node" is the class of namespace b.
TEST(NamespacedBaseTranslation, BaseNameUsesTheNamespace)
{
    TempDir dir;
    write_file(dir.path() / "nn.h",
        "namespace a { class Node { public: virtual ~Node() {} }; }\n"
        "namespace b { class Node { public: virtual ~Node() {} virtual void tick() {} }; }\n");
    const hotc::TranslateResult r =
        hotc::translate(temp_options(dir, "using \"nn.h\";\nclass S : b.Node {\n    override void tick() { }\n}\n"));
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

namespace
{

// The class "S" derives from "base" of a header and overrides the virtual method "tick".
hotc::TranslateResult derive_with_tick(const std::string& header, const std::string& base)
{
    TempDir dir;
    write_file(dir.path() / "nb.h", header);
    return hotc::translate(
        temp_options(dir, "using \"nb.h\";\nclass S : " + base + " {\n    override void tick() { }\n}\n"));
}

} // namespace

// HOT-90: the base name is a namespace alias.
TEST(NamespacedBaseTranslation, BaseNameThroughNamespaceAlias)
{
    const hotc::TranslateResult r = derive_with_tick(
        "namespace ns { class Base { public: virtual ~Base() {} virtual void tick() {} }; }\n"
        "namespace U = ns;\n",
        "U.Base");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// HOT-90: the base name of a .cpp script has a leading "::".
TEST(NamespacedBaseTranslation, BaseNameWithLeadingScope)
{
    TempDir dir;
    write_file(dir.path() / "nb.h",
        "namespace ns { class Base { public: virtual ~Base() {} virtual void tick() {} }; }\n");
    hotc::TranslateOptions o = temp_options(dir,
        "#include \"nb.h\"\nclass S : public ::ns::Base\n{\npublic:\n    void tick() override { }\n};\n");
    o.script_name = "t.cpp";
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(r.base_virtuals.empty()) << r.diagnostics;
}

// HOT-90: the header declares the class in an inline namespace. The script does not name it.
TEST(NamespacedBaseTranslation, BaseNameSkipsInlineNamespace)
{
    const hotc::TranslateResult r = derive_with_tick(
        "namespace ns { inline namespace v1 { class Base { public: virtual ~Base() {} virtual void tick() {} }; } }\n",
        "ns.Base");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
