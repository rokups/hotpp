// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the operators of a script struct. The tests check the text of
// the generated members.
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

hotc::TranslateResult translate_structs(const std::string& body)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    return hotc::translate(temp_options(*dir, body));
}

const char* kVec =
    "struct Vec2 {\n"
    "    float x = 0.0;\n"
    "    float y = 0.0;\n"
    "    float[2] data;\n"
    "    Vec2 operator+(in Vec2 o) { var r = Vec2(); r.x = this.x + o.x; return r; }\n"
    "    Vec2 operator*(float f) { var r = Vec2(); r.x = this.x * f; return r; }\n"
    "    Vec2 operator-() { var r = Vec2(); r.x = -this.x; return r; }\n"
    "    bool operator<(in Vec2 o) { return this.x < o.x; }\n"
    "    bool operator>(in Vec2 o) { return this.x > o.x; }\n"
    "    ref float operator[](int i) { return this.data[i]; }\n"
    "}\n";

} // namespace

TEST(OperatorStructTranslation, MemberUsesTheCxxOperatorName)
{
    const hotc::TranslateResult r = translate_structs(kVec);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Vec2 operator+(const Vec2& o) const { return Vec2__op_add(const_cast<Vec2*>(this), o); }"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Vec2 operator-() const { return Vec2__op_neg(const_cast<Vec2*>(this)); }")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, " op_add(")) << r.cpp; // the free function keeps the identifier
    EXPECT_TRUE(has(r.cpp, "static Vec2 Vec2__op_add(Vec2* __self, const Vec2& o);")) << r.cpp;
}

TEST(OperatorStructTranslation, CompareMembersAreConst)
{
    const hotc::TranslateResult r = translate_structs(kVec);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "bool operator<(const Vec2& o) const {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool operator>(const Vec2& o) const {")) << r.cpp;
}

TEST(OperatorStructTranslation, RefIndexGivesAConstAndANonConstMember)
{
    const hotc::TranslateResult r = translate_structs(kVec);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "float& operator[](int32_t i) { return Vec2__op_index(this, i); }")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const float& operator[](int32_t i) const { return Vec2__op_index(const_cast<Vec2*>(this), i); }"))
        << r.cpp;
}

TEST(OperatorStructTranslation, ValueIndexIsConst)
{
    const hotc::TranslateResult r = translate_structs(
        "struct Row {\n    int[2] d;\n    int operator[](int i) { return this.d[i]; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t operator[](int32_t i) const {")) << r.cpp;
}

TEST(OperatorStructTranslation, GeneratesEqualAndNotEqualFromTheFields)
{
    const hotc::TranslateResult r = translate_structs(kVec);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp,
        "template <class S_ = Vec2> bool operator==(const Vec2& o) const { const auto& __o = static_cast<const S_&>(o); "
        "return this->x == __o.x && this->y == __o.y && this->data == __o.data; }"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp,
        "template <class S_ = Vec2> bool operator!=(const Vec2& o) const { return !(*this == static_cast<const S_&>(o)); }"))
        << r.cpp;
}

TEST(OperatorStructTranslation, StructWithoutFieldsComparesEqual)
{
    const hotc::TranslateResult r = translate_structs("struct Empty {\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "bool operator==(const Empty&) const { return true; }")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool operator!=(const Empty& o) const {")) << r.cpp;
}

TEST(OperatorStructTranslation, NestedStructFieldUsesItsEqual)
{
    const hotc::TranslateResult r = translate_structs(std::string(kVec) + "struct Pair {\n    Vec2 v;\n    int n;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return this->v == __o.v && this->n == __o.n; }")) << r.cpp;
}

TEST(OperatorStructTranslation, WrittenEqualIsNotGenerated)
{
    const hotc::TranslateResult r = translate_structs(
        "struct Id {\n    int n;\n    bool operator==(in Id o) { return this.n == o.n; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "bool operator==(const Id& o) const { return Id__op_eq(")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "__o.n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool operator!=(const Id& o) const { return !(*this == o); }")) << r.cpp;
}

TEST(OperatorStructTranslation, GeneratesCompoundOperatorsForTheWrittenOnes)
{
    const hotc::TranslateResult r = translate_structs(kVec);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Vec2& operator+=(const Vec2& o) { *this = *this + o; return *this; }")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Vec2& operator*=(float f) { *this = *this * f; return *this; }")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "operator-=")) << r.cpp; // only the unary minus is written
    EXPECT_FALSE(has(r.cpp, "operator/=")) << r.cpp;
}

TEST(OperatorStructTranslation, GeneratedEqualIsATemplate)
{
    // The generated "==" is a member template: C++ checks the body only when the code compares.
    const hotc::TranslateResult r = translate_structs("struct Id {\n    int n;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "template <class S_ = Id> bool operator==(")) << r.cpp;
}

TEST(OperatorStructTranslation, NoCompoundOperatorWhenTheResultIsNotTheStruct)
{
    const hotc::TranslateResult r = translate_structs(
        "struct Len {\n    float x;\n    float operator*(in Len o) { return this.x * o.x; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "operator*=")) << r.cpp;
}

// A plain method that does not change the object is a const member. A call on an "in" value compiles.
TEST(OperatorStructTranslation, ReadOnlyMethodIsConst)
{
    const hotc::TranslateResult r = translate_structs("struct Team {\n"
                                                      "    int slot;\n"
                                                      "    int[4] cells;\n"
                                                      "    bool IsT() { return slot % 2 == 0; }\n"
                                                      "    bool IsCt() { return !IsT(); }\n"
                                                      "    bool Same(in Team o) { return this.IsT() == o.IsT(); }\n"
                                                      "    int Get() { int s = slot; return s; }\n"
                                                      "    Team Copy() { Team t; t.slot = this.slot; return t; }\n"
                                                      "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "bool IsT() const { return Team__IsT(const_cast<Team*>(this)); }")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "bool IsCt() const { return Team__IsCt(const_cast<Team*>(this)); }")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "o) const { return Team__Same(const_cast<Team*>(this), o); }")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "int32_t Get() const {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Team Copy() const {")) << r.cpp;
}

// A method that writes the object, or that is not surely a read, stays a non-const member.
TEST(OperatorStructTranslation, WritingMethodIsNotConst)
{
    const hotc::TranslateResult r = translate_structs("struct Box {\n"
                                                      "    int n;\n"
                                                      "    int[4] cells;\n"
                                                      "    std.vector<int> v;\n"
                                                      "    void Set(int x) { n = x; }\n"
                                                      "    void Bump() { this.n++; }\n"
                                                      "    void Cell(int i) { this.cells[i] += 1; }\n"
                                                      "    void Push() { v.push_back(1); }\n"
                                                      "    void Twice() { Set(1); }\n"
                                                      "    void Twice2() { this.Bump(); }\n"
                                                      "    void Inc(ref int x) { x += 1; }\n"
                                                      "    void Pass() { Inc(n); }\n"
                                                      "    void Alias() { ref int a = n; a = 2; }\n"
                                                      "    ref int Slot() { return n; }\n"
                                                      "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    for (const char* m : {"void Set(int32_t x) {", "void Bump() {", "void Cell(int32_t i) {", "void Push() {",
             "void Twice() {", "void Twice2() {", "void Pass() {", "void Alias() {", "int32_t& Slot() {"})
        EXPECT_TRUE(has(r.cpp, m)) << m << "\n" << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Box__Bump(const_cast<Box*>(this))")) << r.cpp;
}
