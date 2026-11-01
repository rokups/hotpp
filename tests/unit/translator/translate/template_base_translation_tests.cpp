// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with native bases that come from a class template (design risk 8,
// experiment F1). The script names a plain class. The class can derive from a template instance.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kTemplateHeader =
    "template <class T> class Tmpl { public: virtual ~Tmpl() {} virtual void Step(T v) {} };\n"
    "template <> class Tmpl<float> { public: virtual ~Tmpl() {} virtual void Step(float v) {} };\n"
    "class FloatBase : public Tmpl<float> {};\n"
    "class IntBase : public Tmpl<int> {};\n"
    "template <class T> class Mid : public Tmpl<T> { public: virtual void Extra(T v) {} };\n"
    "class MidBase : public Mid<int> {};\n"
    "template <class T> class Part { public: virtual ~Part() {} virtual void Plain(T v) {} };\n"
    "template <class U> class Part<U*> { public: virtual ~Part() {} virtual void Ptr(U* v) {} };\n"
    "class PartBase : public Part<int*> {};\n"
    "class PlainPartBase : public Part<int> {};\n"
    "template <class T> class PartMid : public Part<T*> {};\n"
    "class PartMidBase : public PartMid<float> {};\n"
    "template <> class Part<char> { public: virtual ~Part() {} virtual void Chr(char v) {} };\n"
    "template <class T> class PartWrap : public Part<T> {};\n"
    "class CharWrap : public PartWrap<char> {};\n";

hotc::TranslateResult run(const std::string& classes)
{
    TempDir dir;
    write_file(dir.path() / "t.h", kTemplateHeader);
    return hotc::translate(temp_options(dir, "using \"t.h\";\n" + classes));
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

// An explicit specialization is a normal class for libclang. Its virtuals get slots.
TEST(TemplateBaseTranslation, ExplicitSpecializationBaseGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : FloatBase {\n    override void Step(float v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Step = 0,")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Foo_Shell final : FloatBase, hot::ShellBase {")) << r.cpp;
}

// A base that derives from an implicit instance of a class template gives its virtuals. The
// translator walks the type declaration of the base specifier (design risk 8).
TEST(TemplateBaseTranslation, ImplicitInstanceBaseGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : IntBase {\n    override void Step(int v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Step = 0,")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Foo_Shell final : IntBase, hot::ShellBase {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "void Step(int a0) override")) << r.cpp;
}

// A wrong signature still gives an error: the parameter type is int, not float.
TEST(TemplateBaseTranslation, ImplicitInstanceBaseChecksSignature)
{
    const hotc::TranslateResult r = run("class Foo : IntBase {\n    override void Step(float v) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(has(r.cpp, "SLOT_Step")) << r.cpp;
}

// A script class names a template instance as its direct native base.
TEST(TemplateBaseTranslation, DirectInstanceBaseGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : Tmpl<int> {\n    override void Step(int v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Step = 0,")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "struct Foo_Shell final : Tmpl<int32_t>, hot::ShellBase {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "void Step(int a0) override")) << r.cpp;
}

// A template that derives from another template: the argument passes through both.
TEST(TemplateBaseTranslation, TemplateOverTemplateGivesSlots)
{
    const hotc::TranslateResult r =
        run("class Foo : MidBase {\n    override void Step(int v) { }\n    override void Extra(int v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Step = 0,")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SLOT_Extra = 1,")) << r.cpp;
}

TEST(TemplateBaseTranslation, TemplateOverTemplateChecksSignature)
{
    const hotc::TranslateResult r = run("class Foo : MidBase {\n    override void Step(float v) { }\n}\n");
    EXPECT_FALSE(r.ok);
}

// The direct script base is a template that derives from a template.
TEST(TemplateBaseTranslation, DirectTemplateOverTemplateGivesSlots)
{
    const hotc::TranslateResult r =
        run("class Foo : Mid<int> {\n    override void Step(int v) { }\n    override void Extra(int v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Extra = 1,")) << r.cpp;
}

// A partial specialization is the pattern when the argument matches it.
TEST(TemplateBaseTranslation, PartialSpecializationBaseGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : PartBase {\n    override void Ptr(int* v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Ptr = 0,")) << r.cpp;
}

// The primary template stays the pattern when no partial specialization matches.
TEST(TemplateBaseTranslation, PrimaryTemplateStillMatchesBesidePartial)
{
    const hotc::TranslateResult r = run("class Foo : PlainPartBase {\n    override void Plain(int v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Plain = 0,")) << r.cpp;
}

TEST(TemplateBaseTranslation, DirectPartialSpecializationGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : Part<int*> {\n    override void Ptr(int* v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Ptr = 0,")) << r.cpp;
}

// A template that derives from a partial specialization.
TEST(TemplateBaseTranslation, TemplateOverPartialSpecializationGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : PartMidBase {\n    override void Ptr(float* v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Ptr = 0,")) << r.cpp;
}

// A template that derives from an explicit specialization.
TEST(TemplateBaseTranslation, TemplateOverExplicitSpecializationGivesSlots)
{
    const hotc::TranslateResult r = run("class Foo : CharWrap {\n    override void Chr(char v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SLOT_Chr = 0,")) << r.cpp;
}
