// Tests for sppc::translate with a script constructor, a destructor and "new" of a script class.
#include <gtest/gtest.h>

#include <string>

#include "spp/sppc_lib.h"
#include "support/translator_test_support.h"

using namespace translator_test;

namespace {

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

sppc::TranslateResult translate_script(const std::string& body)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return sppc::translate(temp_options(dir, "using \"u.h\";\nusing <string>;\n" + body));
}

void expect_error(const std::string& body, const std::string& msg)
{
    const sppc::TranslateResult r = translate_script(body);
    EXPECT_FALSE(r.ok) << body;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << body << "\n" << r.diagnostics;
}

bool has(const sppc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

}  // namespace

TEST(ConstructorTranslation, ConstructorEmitsTheEntryPoints)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad(std.string label, int extra) : base(label) { this.size = this.size + extra; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* __self, std::string label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "Squad__super_args(std::string label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "static Squad_Shell* Squad__new(std::string label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "spp::construct_shell(&Squad_record")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__construct")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__new_default")) << r.cpp;
    // A constructor gets no slot: the table does not call it.
    EXPECT_EQ(r.slots_out.find("Squad"), std::string::npos) << r.slots_out;
}

TEST(ConstructorTranslation, ConstructorWithoutSuperUsesTheBaseDefaultConstructor)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad(int extra) { this.size = extra; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__new(int32_t extra)")) << r.cpp;
}

TEST(ConstructorTranslation, NewOfAScriptClass)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad(int extra) { this.size = extra; }\n"
        "}\n"
        "class Boss : Unit {\n"
        "    override void Update(float dt) { var s = new Squad(3); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__new(3)")) << r.cpp;
}

TEST(ConstructorTranslation, NewNeedsNoConstructor)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit { int size = 5; }\n"
        "class Boss : Unit {\n"
        "    override void Update(float dt) { var s = new Squad(); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "Squad__new()")) << r.cpp;
}

TEST(ConstructorTranslation, DestructorGoesToTheRecord)
{
    const sppc::TranslateResult r = translate_script(
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
    const sppc::TranslateResult r = translate_script(
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
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    Squad() { this.size = 1; }\n"
        "    Squad(int n, int& seen) { this.size = n; seen = n; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* __self);")) << r.cpp;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* __self, int32_t n, int32_t& seen);")) << r.cpp;
    EXPECT_TRUE(has(r, "using Squad_SuperArgs_1 = ")) << r.cpp;
    EXPECT_TRUE(has(r, "static spp::ShellBase* Squad__construct(const char* key, const void* args)")) << r.cpp;
    EXPECT_TRUE(has(r, "static Squad_Shell* Squad__new(int32_t n, int32_t& seen)")) << r.cpp;
    // The factory uses the overload with the fewest parameters.
    EXPECT_TRUE(has(r, "return Squad__build(false);")) << r.cpp;
}

// A "base(...)" argument that names a "T&" parameter binds to the "T&" parameter of a script
// parent (S23). The plan keeps a reference and evaluates the operand one time.
TEST(ConstructorTranslation, SuperArgumentBindsToARefParameter)
{
    const sppc::TranslateResult r = translate_script(
        "class A : Unit {\n    A(int v, int& seen) { seen = v; }\n}\n"
        "class B : A {\n    B(int w, int& seen) : base(w + 1, seen) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static auto B__plan(int32_t w, int32_t& seen)")) << r.cpp;
    EXPECT_TRUE(has(r, "std::make_tuple(w + 1, std::ref(seen))")) << r.cpp;
    EXPECT_EQ(r.cpp.find("std::ref(seen)"), r.cpp.rfind("std::ref(seen)")) << r.cpp;
}

// A "&&" constructor parameter is a const reference in the plan and moves into the constructor.
TEST(ConstructorTranslation, RvalueReferenceParameter)
{
    const sppc::TranslateResult r = translate_script(
        "class A : Unit {\n    std.string t = std.string();\n    A(std.string&& s) { this.t = s; }\n}\n"
        "class B : A {\n    B(std.string&& s) : base(std.move(s)) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void A__ctor(A_Shell* __self, std::string&& s);")) << r.cpp;
    EXPECT_TRUE(has(r, "static auto A__plan(const std::remove_reference_t<std::string&&>& s)")) << r.cpp;
    EXPECT_TRUE(has(r, "A__ctor_chain(__self, __plan.second, std::move(s));")) << r.cpp;
}

TEST(ConstructorTranslation, SuperArgumentsMayUseParametersOnly)
{
    const std::string tail = "the arguments of 'base(...)' may use the constructor parameters only";
    expect_error("class S : Unit {\n    std.string tag = \"x\";\n"
                 "    S(int a) : base(this.tag) { }\n}\n", tail);
    expect_error("class S : Unit {\n    std.string tag = \"x\";\n"
                 "    S(int a) : base(tag) { }\n}\n", tail);
}

TEST(ConstructorTranslation, ConstructorErrors)
{
    expect_error("class S : Unit {\n    S(int v) { }\n    S(int& w) { }\n}\n",
                 "class 'S' has two constructors with the same parameter types");
    expect_error("class Bar : Unit { }\nclass S : Unit {\n    S(Bar b) { }\n    S(const Bar& c) { }\n}\n",
                 "class 'S' has two constructors with the same parameter types");
    expect_error("class S : Unit {\n    ~S() { }\n    ~S() { }\n}\n", "one destructor");
    expect_error("class S : Unit {\n    S(int v, int v) { }\n}\n", "is declared twice");
    expect_error("class S : Unit {\n    S(int __a) { }\n}\n", "parameter name '__a' is reserved");
}

TEST(ConstructorTranslation, BaseWithoutDefaultConstructorNeedsSuper)
{
    expect_error("class S : Plain {\n    S(int v) { }\n}\n", "Plain");
    const sppc::TranslateResult r = translate_script(
        "class S : Plain {\n    S(int v) : base(v) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ConstructorTranslation, NewBeforeTheDefinitionIsAccepted)
{
    const sppc::TranslateResult r = translate_script(
        "class Boss : Unit {\n"
        "    override void Update(float dt) { var s = new Squad(3); }\n"
        "}\n"
        "class Squad : Unit {\n    Squad(int e) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LT(r.cpp.find("Squad__new"), r.cpp.find("Boss__Update")) << r.cpp;
}
