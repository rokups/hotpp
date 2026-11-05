// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with plain parameters. A plain parameter is a read only input. One rule
// gives the C++ type (design: "Parameter defaults"). It does not depend on the body.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

// The struct "Vec3" is small. The struct "Big" has 5 floats. "Box" is a script class. The class "Foo" has the members.
hotc::TranslateResult script(const std::string& members)
{
    return translate_text(
        "struct Vec3 { float x; float y; float z; }\n"
        "struct Big { float a; float b; float c; float d; float e; }\n"
        "class Box { int n; }\n"
        "class Foo {\n"
        + members + "}\n");
}

// The slot signature of the method "F" in the text of the translation.
std::string slot_of(const hotc::TranslateResult& r)
{
    const size_t at = r.cpp.find("Foo__F(");
    return at == std::string::npos ? std::string() : r.cpp.substr(at, r.cpp.find(')', at) - at + 1);
}

} // namespace

TEST(ParamFormTranslation, StructValueIsTheInTrait)
{
    const hotc::TranslateResult r = script("    void F(Vec3 v) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::In<Vec3> v")) << r.cpp;
}

TEST(ParamFormTranslation, BigStructIsTheInTrait)
{
    const hotc::TranslateResult r = script("    void F(Big b) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::In<Big> b")) << r.cpp;
}

TEST(ParamFormTranslation, HandleIsConstSharedPtrReference)
{
    const hotc::TranslateResult r = script("    void F($Box h) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const std::shared_ptr<Box>& h")) << r.cpp;
}

TEST(ParamFormTranslation, BoxIsTheInTraitOfTheValue)
{
    const hotc::TranslateResult r = script("    void F(Box c) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::In<hot::Value<Box>> c")) << r.cpp;
}

TEST(ParamFormTranslation, PrimitiveIsThePlainType)
{
    const hotc::TranslateResult r = script("    void F(int x) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(slot_of(r), "int32_t x")) << r.cpp;
    EXPECT_FALSE(has(slot_of(r), "hot::In")) << r.cpp;
}

TEST(ParamFormTranslation, OptionalIsTheInTrait)
{
    const hotc::TranslateResult r = script("    void F(int? x) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::In<std::optional<int32_t>> x")) << r.cpp;
}

TEST(ParamFormTranslation, ArrayIsSpanByValue)
{
    hotc::TranslateOptions o; // the default view type is hot::span
    o.script_name = "t.hot";
    o.script_text = "class Foo {\n    void F(int[] xs) { }\n}\n";
    o.generation = 1;
    o.std_version = "c++20";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::span<int32_t> xs")) << r.cpp;
}

TEST(ParamFormTranslation, PointerIsPointer)
{
    const hotc::TranslateResult r = script("    void F(int* p) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t* p")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::In<int32_t*>")) << r.cpp;
}

TEST(ParamFormTranslation, InRefAndByValKeepTheirForm)
{
    const hotc::TranslateResult r = script("    void F(in Vec3 a, ref Vec3 b, [byval] Big c) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const Vec3& a")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Vec3& b")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Big c")) << r.cpp;
    EXPECT_FALSE(has(slot_of(r), "hot::In")) << r.cpp;
}

TEST(ParamFormTranslation, BodyThatAssignsTheParameterWorksOnACopy)
{
    const hotc::TranslateResult r = script("    void F(Big b) { b.a = 1.0; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::In<Big> __b")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Big b = ")) << r.cpp;
}

TEST(ParamFormTranslation, BodyThatOnlyReadsKeepsTheRule)
{
    const hotc::TranslateResult r = script("    float F(Big b) { return b.a; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::In<Big> b")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::In<Big> __b")) << r.cpp;
}

namespace
{

const char* kVirtualHeader =
    "struct Vec3 { float x, y, z; };\n"
    "class Base {\n"
    "public:\n"
    "    virtual ~Base() {}\n"
    "    virtual void ByConstRef(const Vec3& v) {}\n"
    "    virtual void ByValue(Vec3 v) {}\n"
    "    virtual void ByRef(Vec3& v) {}\n"
    "};\n";

hotc::TranslateResult override_translate(const std::string& members)
{
    TempDir dir;
    write_file(dir.path() / "v.h", kVirtualHeader);
    return hotc::translate(temp_options(dir, "using \"v.h\";\nclass Foo : Base {\n" + members + "}\n"));
}

} // namespace

TEST(ParamFormTranslation, OverrideWithPlainParameterCopiesTheBaseByValue)
{
    const hotc::TranslateResult r = override_translate("    override void ByValue(Vec3 v) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "::ByValue(Vec3 v")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::In<Vec3>")) << r.cpp;
}

TEST(ParamFormTranslation, OverrideWithInParameterCopiesTheBaseConstReference)
{
    const hotc::TranslateResult r = override_translate("    override void ByConstRef(in Vec3 v) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "::ByConstRef(const Vec3& v")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::In<Vec3>")) << r.cpp;
}

TEST(ParamFormTranslation, OverrideWithRefParameterCopiesTheBaseReference)
{
    const hotc::TranslateResult r = override_translate("    override void ByRef(ref Vec3 v) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "::ByRef(Vec3& v")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::In<Vec3>")) << r.cpp;
}

TEST(ParamFormTranslation, OverrideThatDoesNotMatchTheBaseFormIsAnError)
{
    // "ByValue" takes "Vec3". An "in" parameter is "const Vec3&": no native virtual has it.
    EXPECT_FALSE(override_translate("    override void ByValue(in Vec3 v) { }\n").ok);
    EXPECT_FALSE(override_translate("    override void ByRef(Vec3 v) { }\n").ok);
    EXPECT_FALSE(override_translate("    override void ByConstRef(ref Vec3 v) { }\n").ok);
}

namespace
{

// A script class with the members of "members". The standard vector and string are in the scope.
hotc::TranslateResult std_script(const std::string& members)
{
    return translate_text("using <vector>;\nusing <string>;\nclass Foo {\n" + members + "}\n");
}

} // namespace

TEST(ParamFormTranslation, MemberCallOnInVectorParameter)
{
    const hotc::TranslateResult r = std_script("    int F(in std.vector<int> xs) { return xs.size(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const std::vector<int32_t>& xs")) << r.cpp;
}

TEST(ParamFormTranslation, MemberCallOnInStringParameter)
{
    const hotc::TranslateResult r = std_script("    int F(in std.string s) { return s.size(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "const std::string& s")) << r.cpp;
}

TEST(ParamFormTranslation, NestedMemberCallOnInParameter)
{
    const hotc::TranslateResult r = std_script(
        "    int F(in std.vector<int> xs) { return xs.size() + 1; }\n"
        "    int G(in std.string s) { return s.substr(1).substr(1).size() + s.length(); }\n"
        "    int H(in std.string s) { var n = s.substr(1).size(); return n; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(ParamFormTranslation, MemberCallOnRefParameter)
{
    const hotc::TranslateResult r = std_script(
        "    int F(ref std.vector<int> xs) { return xs.size() + 1; }\n"
        "    int G(ref std.string s) { return s.size() + 1; }\n"
        "    void H(ref std.vector<int> xs) { xs.push_back(1); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::vector<int32_t>& xs")) << r.cpp;
}

TEST(ParamFormTranslation, MemberCallOnOutParameter)
{
    const hotc::TranslateResult r = std_script(
        "    void F(out std.vector<int> xs) { xs = std.vector<int>(); xs.push_back(xs.size() + 1); }\n"
        "    void G(out std.string s) { s = std.string(); s.push_back('a'); int n = s.size(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::vector<int32_t>& xs")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::string& s")) << r.cpp;
}
