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
