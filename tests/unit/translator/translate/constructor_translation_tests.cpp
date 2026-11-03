// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with a script constructor, a destructor and "new" of a script class.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kHeader =
    "#include <string>\n"
    "class Unit { public:\n"
    "    Unit() {}\n"
    "    explicit Unit(std::string n) : name(n) {}\n"
    "    virtual ~Unit() {}\n"
    "    virtual void Update(float dt) {}\n"
    "    std::string name;\n"
    "};\n"
    "class Plain { public: Plain(int v) {} virtual ~Plain() {} virtual void Update(float dt) {} };\n";

hotc::TranslateResult translate_script(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"u.h\";\nusing <string>;\n" + body));
}

void expect_error(const std::string& body, const std::string& msg)
{
    const hotc::TranslateResult r = translate_script(body);
    EXPECT_FALSE(r.ok) << body;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << body << "\n" << r.diagnostics;
}

bool has(const hotc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

} // namespace

TEST(ConstructorTranslation, ConstructorEmitsTheEntryPoints)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad(std.string label, int extra) : base(label) { this.size = this.size + extra; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* __self, hot::In<std::string> label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "Squad__super_args(hot::In<std::string> label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "static Squad_Shell* Squad__new(hot::In<std::string> label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::construct_shell(&Squad_record")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__construct")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__new_default")) << r.cpp;
    // A constructor gets no slot: the table does not call it.
    EXPECT_EQ(r.slots_out.find("Squad"), std::string::npos) << r.slots_out;
}

TEST(ConstructorTranslation, ConstructorWithoutSuperUsesTheBaseDefaultConstructor)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad(int extra) { this.size = extra; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__new(int32_t extra)")) << r.cpp;
}

TEST(ConstructorTranslation, NewOfAScriptClass)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad(int extra) { this.size = extra; }\n"
        "}\n"
        "class Boss : Unit {\n"
        "    override void Update(float dt) { var s = new $Squad(3); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__new_shared(3)")) << r.cpp;
}

TEST(ConstructorTranslation, NewNeedsNoConstructor)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit { int size = 5; }\n"
        "class Boss : Unit {\n"
        "    override void Update(float dt) { var s = new $Squad(); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__new_shared()")) << r.cpp;
}

TEST(ConstructorTranslation, DestructorGoesToTheRecord)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    ~Squad() { this.size = 0; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Squad__dtor(Squad_Shell* __self)")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__dtor_slot")) << r.cpp;
    EXPECT_EQ(r.slots_out.find("dtor"), std::string::npos) << r.slots_out;
}

// A class without a constructor gets a construct entry, so a later generation can use "new".
TEST(ConstructorTranslation, AClassWithoutConstructorGetsAConstructEntry)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    override void Update(float dt) { this.size = this.size + 1; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r, "__ctor")) << r.cpp;
    EXPECT_FALSE(has(r, "__dtor")) << r.cpp;
    EXPECT_FALSE(has(r, "Squad__new_default")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__construct, typeid(Squad_SuperArgs).name(), nullptr,")) << r.cpp;
    EXPECT_TRUE(has(r, "static Squad_Shell* Squad__new()")) << r.cpp;
}

// Overloads are C++ overloads of the same functions. A "T&" parameter is a reference.
TEST(ConstructorTranslation, OverloadsAndRefParameters)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad() { this.size = 1; }\n"
        "    Squad(int n, ref int seen) { this.size = n; seen = n; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* __self);")) << r.cpp;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* __self, int32_t n, int32_t& seen);")) << r.cpp;
    EXPECT_TRUE(has(r, "using Squad_SuperArgs_1 = ")) << r.cpp;
    EXPECT_TRUE(has(r, "static hot::ShellBase* Squad__construct(void* mem, const char* key, const void* args)")) << r.cpp;
    EXPECT_TRUE(has(r, "static Squad_Shell* Squad__new(int32_t n, int32_t& seen)")) << r.cpp;
    // The factory uses the overload with the fewest parameters.
    EXPECT_TRUE(has(r, "return Squad__build(false);")) << r.cpp;
}

// A "base(...)" argument that names a "T&" parameter binds to the "T&" parameter of a script
// parent (S23). The plan keeps a reference and evaluates the operand one time.
TEST(ConstructorTranslation, SuperArgumentBindsToARefParameter)
{
    const hotc::TranslateResult r = translate_script(
        "class A : Unit {\n    A(int v, ref int seen) { seen = v; }\n}\n"
        "class B : A {\n    B(int w, ref int seen) : base(w + 1, seen) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static auto B__plan(int32_t w, int32_t& seen)")) << r.cpp;
    EXPECT_TRUE(has(r, "std::make_tuple(w + 1, std::ref(seen))")) << r.cpp;
    EXPECT_EQ(r.cpp.find("std::ref(seen)"), r.cpp.rfind("std::ref(seen)")) << r.cpp;
}

// A "&&" constructor parameter is removed: "&" is not a type suffix.
TEST(ConstructorTranslation, RvalueReferenceParameterIsAnError)
{
    expect_error("class A : Unit {\n    std.string t = std.string();\n    A(std.string&& s) { this.t = s; }\n}\n",
        "\"&\" is not a type suffix; write ref T x");
}

TEST(ConstructorTranslation, SuperArgumentsMayUseParametersOnly)
{
    const std::string tail = "the arguments of 'base(...)' may use the constructor parameters only";
    expect_error(
        "class S : Unit {\n    std.string tag = \"x\";\n"
        "    S(int a) : base(this.tag) { }\n}\n",
        tail);
    expect_error(
        "class S : Unit {\n    std.string tag = \"x\";\n"
        "    S(int a) : base(tag) { }\n}\n",
        tail);
}

TEST(ConstructorTranslation, ConstructorErrors)
{
    expect_error("class S : Unit {\n    S(int v) { }\n    S(ref int w) { }\n}\n",
        "class 'S' has two constructors with the same parameter types");
    expect_error("class Bar : Unit { }\nclass S : Unit {\n    S($Bar b) { }\n    S(in $Bar c) { }\n}\n",
        "class 'S' has two constructors with the same parameter types");
    expect_error("class S : Unit {\n    ~S() { }\n    ~S() { }\n}\n", "one destructor");
    expect_error("class S : Unit {\n    S(int v, int v) { }\n}\n", "is declared twice");
    expect_error("class S : Unit {\n    S(int __a) { }\n}\n", "parameter name '__a' is reserved");
}

TEST(ConstructorTranslation, BaseWithoutDefaultConstructorNeedsSuper)
{
    expect_error("class S : Plain {\n    S(int v) { }\n}\n", "Plain");
    const hotc::TranslateResult r = translate_script("class S : Plain {\n    S(int v) : base(v) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ConstructorTranslation, NewBeforeTheDefinitionIsAccepted)
{
    const hotc::TranslateResult r = translate_script(
        "class Boss : Unit {\n"
        "    override void Update(float dt) { var s = new $Squad(3); }\n"
        "}\n"
        "class Squad : Unit {\n    Squad(int e) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("Squad__new"), r.cpp.find("Boss__Update")) << r.cpp;
}

// "null" for a "[weak]" parameter of a constructor is an empty weak_ptr. A nullptr does not
// convert to std::weak_ptr, so C__new_shared(nullptr) did not compile.
TEST(ConstructorTranslation, NullForAWeakParameterIsTheEmptyWeakHandle)
{
    const hotc::TranslateResult r = translate_script(
        "class N {\n    int v = 1;\n}\n"
        "class C {\n    [weak]\n    $N p = null;\n    C([weak] $N q) { p = q; }\n}\n"
        "class K : Unit {\n    override void Update(float dt) { $C c = new $C(null); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "C__new_shared(std::weak_ptr<N>())")) << r.cpp;
    EXPECT_FALSE(has(r, "C__new_shared(nullptr)")) << r.cpp;
}

// A field with the name of a method is an error. This includes a method that overrides a native
// virtual, which has no other check of the name.
TEST(ConstructorTranslation, FieldWithTheNameOfAnOverrideIsAnError)
{
    expect_error("class S : Unit {\n    int Update = 3;\n    override void Update(float dt) { }\n}\n",
        "the field 'Update' has the name of a method of the class");
}

// A shell with no native base gets a clone constructor and a clone branch in "construct". The
// field rows have the copy function. A child builds on the shell of the parent.
TEST(ConstructorTranslation, ShellWithoutNativeBaseHasACloneConstructor)
{
    const hotc::TranslateResult r = translate_script(
        "class Base {\n"
        "    int n = 1;\n"
        "    std.string label = \"b\";\n"
        "}\n"
        "class Child : Base {\n"
        "    int extra = 2;\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "explicit Base_Shell(hot::CloneFrom, const hot::ShellBase* src)")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::clone_state(this, &Base_record, src);")) << r.cpp;
    EXPECT_TRUE(has(r, "explicit Child_Shell(hot::CloneFrom, const hot::ShellBase* src) : Base_Shell(hot::ConstructChild{})"))
        << r.cpp;
    EXPECT_TRUE(has(r, "hot::clone_state(this, &Child_record, src);")) << r.cpp;
    EXPECT_TRUE(has(r, "key == hot::kCloneKey")) << r.cpp;
    EXPECT_TRUE(has(r, "Base_Shell(hot::CloneFrom{}, src)")) << r.cpp;
    EXPECT_TRUE(has(r, "Child__construct_keys[] = {")) << r.cpp;
    EXPECT_TRUE(has(r, ", hot::kCloneKey, nullptr};")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::field_copy<int32_t>()")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::field_copy<std::string>()")) << r.cpp;
}

// A shell with a native base has no clone constructor.
TEST(ConstructorTranslation, ShellWithNativeBaseHasNoCloneConstructor)
{
    const hotc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r, "hot::CloneFrom")) << r.cpp;
    EXPECT_FALSE(has(r, "kCloneKey")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::field_copy<int32_t>()")) << r.cpp;
}
