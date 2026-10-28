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
        "    Squad(std.string label, int extra) : super(label) { this.size = this.size + extra; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "static void Squad__ctor(Squad_Shell* self, std::string label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "Squad__super_args(std::string label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "static Squad_Shell* Squad__new(std::string label, int32_t extra)")) << r.cpp;
    EXPECT_TRUE(has(r, "spp::construct_shell(&Squad_record")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__construct")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__ctor_default")) << r.cpp;
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
    EXPECT_TRUE(has(r, "static void Squad__dtor(Squad_Shell* self)")) << r.cpp;
    EXPECT_TRUE(has(r, "&Squad__dtor_slot")) << r.cpp;
    EXPECT_EQ(r.slots_out.find("dtor"), std::string::npos) << r.slots_out;
}

TEST(ConstructorTranslation, AClassWithoutConstructorKeepsTheOldOutput)
{
    const sppc::TranslateResult r = translate_script(
        "class Squad : Unit {\n"
        "    int size = 5;\n"
        "    override void Update(float dt) { this.size = this.size + 1; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r, "__ctor")) << r.cpp;
    EXPECT_FALSE(has(r, "__new")) << r.cpp;
    EXPECT_FALSE(has(r, "__dtor")) << r.cpp;
    EXPECT_FALSE(has(r, "construct_shell")) << r.cpp;
}

TEST(ConstructorTranslation, SuperArgumentsMayUseParametersOnly)
{
    const std::string tail = "the arguments of 'super(...)' may use the constructor parameters only";
    expect_error("class S : Unit {\n    std.string tag = \"x\";\n"
                 "    S(int a) : super(this.tag) { }\n}\n", tail);
    expect_error("class S : Unit {\n    std.string tag = \"x\";\n"
                 "    S(int a) : super(tag) { }\n}\n", tail);
}

TEST(ConstructorTranslation, ConstructorErrors)
{
    expect_error("class S : Unit {\n    S() { }\n    S(int a) { }\n}\n", "one constructor");
    expect_error("class S : Unit {\n    ~S() { }\n    ~S() { }\n}\n", "one destructor");
    expect_error("class S : Unit {\n    S(ref int a) { }\n}\n", "constructor");
    expect_error("class S : Unit {\n    S(int v, int v) { }\n}\n", "is declared twice");
    expect_error("class S : Unit {\n    S(int a) { }\n}\n", "parameter name 'a' is reserved");
}

TEST(ConstructorTranslation, BaseWithoutDefaultConstructorNeedsSuper)
{
    expect_error("class S : Plain {\n    S(int v) { }\n}\n", "Plain");
    const sppc::TranslateResult r = translate_script(
        "class S : Plain {\n    S(int v) : super(v) { }\n}\n");
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
