// The fine interface data of a translation (hot::IfaceInfo, hot::UseInfo): the shape, the member
// entries, the names and the strong imports. See doc/module-level.md.

#include "hot/hotc_lib.h"
#include "hot/iface_compat.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

using namespace translator_test;

hotc::TranslateResult run(const TempDir& dir, const std::string& script)
{
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateResult r = hotc::translate(temp_options(dir, script));
    EXPECT_TRUE(r.ok) << r.diagnostics;
    return r;
}

hot::IfaceInfo info(const TempDir& dir, const std::string& script)
{
    const hotc::TranslateResult r = run(dir, script);
    EXPECT_TRUE(r.iface_info.known);
    EXPECT_EQ(r.iface_info.hash, r.iface_hash);
    return r.iface_info;
}

bool has_name(const std::vector<uint64_t>& names, const char* name)
{
    return std::binary_search(names.begin(), names.end(), hot::iface_name_hash(name));
}

std::vector<uint64_t> changed(const hot::IfaceInfo& a, const hot::IfaceInfo& b)
{
    return hot::iface_changed_names(a, b);
}

const char* kClassV1 =
    "using \"w.h\";\n"
    "class Clock : Widget {\n"
    "    int ticks = 0;\n"
    "    int Get() { return ticks; }\n"
    "}\n";

} // namespace

TEST(IfaceInfoTranslation, NewMethodChangesOneMemberEntryAndKeepsTheShape)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, kClassV1);
    const hot::IfaceInfo b = info(dir,
        "using \"w.h\";\n"
        "class Clock : Widget {\n"
        "    int ticks = 0;\n"
        "    int Get() { return ticks; }\n"
        "    void Reset() { ticks = 0; }\n"
        "}\n");
    EXPECT_NE(a.hash, b.hash) << "the old hash stays as it was";
    EXPECT_EQ(a.shape, b.shape);
    const std::vector<uint64_t> d = changed(a, b);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0], hot::iface_name_hash("Reset"));
    EXPECT_TRUE(has_name(b.names, "Reset"));
    EXPECT_FALSE(has_name(a.names, "Reset"));
}

TEST(IfaceInfoTranslation, ChangedMethodSignatureChangesItsEntryOnly)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, kClassV1);
    const hot::IfaceInfo b = info(dir,
        "using \"w.h\";\n"
        "class Clock : Widget {\n"
        "    int ticks = 0;\n"
        "    int Get(int k) { return ticks + k; }\n"
        "}\n");
    EXPECT_EQ(a.shape, b.shape);
    const std::vector<uint64_t> d = changed(a, b);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0], hot::iface_name_hash("Get"));
}

TEST(IfaceInfoTranslation, NewClassFieldChangesOneMemberEntry)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, kClassV1);
    const hot::IfaceInfo b = info(dir,
        "using \"w.h\";\n"
        "class Clock : Widget {\n"
        "    int ticks = 0;\n"
        "    float rate = 1.0f;\n"
        "    int Get() { return ticks; }\n"
        "}\n");
    EXPECT_EQ(a.shape, b.shape) << "a class field is late bound by name";
    const std::vector<uint64_t> d = changed(a, b);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0], hot::iface_name_hash("rate"));
}

TEST(IfaceInfoTranslation, NewStructFieldChangesTheShape)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, "struct Pt {\n    int x;\n}\n");
    const hot::IfaceInfo b = info(dir, "struct Pt {\n    int x;\n    int y;\n}\n");
    EXPECT_NE(a.shape, b.shape);
    EXPECT_TRUE(changed(a, b).empty()) << "a struct has no member entries";
}

TEST(IfaceInfoTranslation, ClassBodyEditChangesNothing)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, kClassV1);
    const hot::IfaceInfo b = info(dir,
        "using \"w.h\";\n"
        "class Clock : Widget {\n"
        "    int ticks = 0;\n"
        "    int Get() { return ticks * 2 + 1; }\n"
        "}\n");
    EXPECT_TRUE(a == b);
}

TEST(IfaceInfoTranslation, VirtualAndOverrideMembersAreInTheShape)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, kClassV1);
    const hot::IfaceInfo virt = info(dir,
        "using \"w.h\";\n"
        "class Clock : Widget {\n"
        "    int ticks = 0;\n"
        "    int Get() { return ticks; }\n"
        "    virtual int Rate() { return 1; }\n"
        "}\n");
    const hot::IfaceInfo over = info(dir,
        "using \"w.h\";\n"
        "class Clock : Widget {\n"
        "    int ticks = 0;\n"
        "    int Get() { return ticks; }\n"
        "    override void Update(float dt) { }\n"
        "}\n");
    EXPECT_NE(a.shape, virt.shape) << "a script virtual sets the slot family";
    EXPECT_NE(a.shape, over.shape) << "a native override sets the shell";
}

TEST(IfaceInfoTranslation, InterfaceDeclarationsAreInTheShape)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, "interface IShape {\n    int Area();\n}\n");
    const hot::IfaceInfo b = info(dir, "interface IShape {\n    int Area();\n    void Scale(int k);\n}\n");
    EXPECT_NE(a.shape, b.shape);
    EXPECT_TRUE(changed(a, b).empty());
}

TEST(IfaceInfoTranslation, FreeFunctionsAndVariablesAreMembers)
{
    TempDir dir;
    const hot::IfaceInfo a = info(dir, "int Twice(int v) { return v * 2; }\nint counter = 0;\n");
    const hot::IfaceInfo b = info(dir, "int Twice(int v) { return v * 2; }\nint counter = 0;\nint Half(int v) { return v / 2; }\n");
    const hot::IfaceInfo c = info(dir, "int Twice(int v) { return v * 2; }\nfloat counter = 0.0f;\n");
    EXPECT_EQ(a.shape, b.shape);
    EXPECT_EQ(changed(a, b), std::vector<uint64_t>{hot::iface_name_hash("Half")});
    EXPECT_EQ(a.shape, c.shape);
    EXPECT_EQ(changed(a, c), std::vector<uint64_t>{hot::iface_name_hash("counter")});
}

TEST(IfaceInfoTranslation, UseNamesHaveEachIdentifierOfTheScript)
{
    TempDir dir;
    const hotc::TranslateResult r = run(dir,
        "using \"w.h\";\n"
        "// Reset in a comment\n"
        "class Clock : Widget {\n"
        "    string Note() { return \"Stop\"; }\n"
        "}\n");
    EXPECT_TRUE(r.use_info.known);
    EXPECT_TRUE(has_name(r.use_info.names, "Reset"));
    EXPECT_TRUE(has_name(r.use_info.names, "Stop"));
    EXPECT_TRUE(has_name(r.use_info.names, "Widget"));
    EXPECT_FALSE(has_name(r.use_info.names, "Start"));
    EXPECT_TRUE(r.use_info.strong.empty());
}

// A class with a parent in an import needs the import in full. The walk goes through the parent
// of a middle import to the unit of the root.
TEST(IfaceInfoTranslation, ParentChainUnitsAreStrong)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"mid.hot\";\n"
        "class Kid : Mid {\n"
        "    override int Power() { return 3; }\n"
        "}\n");
    o.modules.emplace_back("base.hot",
        "using \"w.h\";\nclass Base : Widget {\n    virtual int Power() { return 1; }\n}\n");
    o.modules.emplace_back("mid.hot", "using \"base.hot\";\nclass Mid : Base { }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.use_info.strong, (std::vector<std::string>{"base", "mid"}));
}

// Open question 1 (doc/module-level.md): a value of a script class is a hot::Value<T>, a box with
// a pointer to the shell. The importer does not embed the fields: the import is not strong.
TEST(IfaceInfoTranslation, ValueUseOfAForeignClassIsNotStrong)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"w.h\";\n"
        "using \"plain.hot\";\n"
        "class User : Widget {\n"
        "    Plain p;\n"
        "    int Read() { return p.Get(); }\n"
        "}\n");
    o.modules.emplace_back("plain.hot", "class Plain {\n    int v = 1;\n    int Get() { return v; }\n}\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.use_info.strong.empty());
    EXPECT_NE(r.cpp.find("hot::Value<Plain>"), std::string::npos) << "the field is a box, not the state";
    EXPECT_TRUE(has_name(r.use_info.names, "Get"));
}
