// Tests for the cache of spc::TranslatorSession.
//
// The cache keeps the virtual methods of the native base classes. A hit must give
// the same output as a miss. A change of a header must invalidate the cache.
// CMake defines SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>

#include "spp/spc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

using namespace translator_test;
using namespace stub_translate_test;

TEST(SessionCache, HitGivesTheSameOutputAsMiss)
{
    spc::TranslateOptions o = engine_options("player_v1.script", 1);
    const spc::TranslateResult alone = spc::translate(o);  // a temporary session
    ASSERT_TRUE(alone.ok) << alone.diagnostics;

    spc::TranslatorSession session;
    const spc::TranslateResult miss = spc::translate(o, session);
    const spc::TranslateResult hit = spc::translate(o, session);
    ASSERT_TRUE(miss.ok) << miss.diagnostics;
    ASSERT_TRUE(hit.ok) << hit.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(miss.cpp, alone.cpp);
    EXPECT_EQ(hit.cpp, alone.cpp);
    EXPECT_EQ(hit.slots_out, alone.slots_out);

    // The second generation also hits and agrees with a call without a session.
    spc::TranslateOptions o2 = engine_options("player_v2.script", 2);
    o2.slots_in = alone.slots_out;
    const spc::TranslateResult alone2 = spc::translate(o2);
    const spc::TranslateResult hit2 = spc::translate(o2, session);
    ASSERT_TRUE(hit2.ok) << hit2.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(hit2.cpp, alone2.cpp);
    EXPECT_EQ(hit2.slots_out, alone2.slots_out);
}

TEST(SessionCache, ChangedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;

    EXPECT_TRUE(spc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    // The new virtual method is not in the header yet. Draw is a plain script function.
    spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;
    EXPECT_EQ(session.parse_count(), 1);

    // The header gets a new virtual method. The new size shows the change.
    write_file(dir.path() / "w.h",
               "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} "
               "virtual void Draw() {} };\n"
               "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    r = spc::translate(temp_options(dir, widget_script("Draw()", "override ")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.cpp.find("Draw"), std::string::npos);
    EXPECT_EQ(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;  // an override

    // A change with the same size and a new time also invalidates the cache.
    const std::string same_size = read_file((dir.path() / "w.h").string());
    std::string edited = same_size;
    edited.replace(edited.find("Draw"), 4, "Dram");
    write_file(dir.path() / "w.h", edited);
    fs::last_write_time(dir.path() / "w.h", fs::last_write_time(dir.path() / "w.h") + std::chrono::seconds(5));
    r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;  // not an override
    EXPECT_EQ(session.parse_count(), 3);
    r = spc::translate(temp_options(dir, widget_script("Dram()", "override ")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.slots_out.find("Foo::Dram"), std::string::npos) << r.slots_out;  // an override
    EXPECT_EQ(session.parse_count(), 3);
}

TEST(SessionCache, ChangedNestedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "inner.h", "#define EXTRA\n");
    write_file(dir.path() / "w.h",
               "#include \"inner.h\"\n"
               "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {}\n"
               "#ifdef EXTRA\n virtual void Draw() {}\n#endif\n};\n");
    spc::TranslatorSession session;
    const spc::TranslateResult first = spc::translate(temp_options(dir, widget_script("Draw()", "override ")), session);
    EXPECT_TRUE(first.ok) << first.diagnostics;
    EXPECT_EQ(first.slots_out.find("Foo::Draw"), std::string::npos) << first.slots_out;  // an override
    write_file(dir.path() / "inner.h", "// no macro now\n");
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;  // not an override
    EXPECT_EQ(session.parse_count(), 2);
}

TEST(SessionCache, OtherBaseClassWorksInTheSameSession)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;

    const spc::TranslateResult a = spc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string panel_script =
        "using \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n";
    const spc::TranslateResult b = spc::translate(temp_options(dir, panel_script), session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(b.cpp.find("Layout"), std::string::npos);
    // Both bases are in the cache now. The next calls do not parse.
    const int parses = session.parse_count();
    const spc::TranslateResult a2 = spc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session);
    const spc::TranslateResult b2 = spc::translate(temp_options(dir, panel_script), session);
    EXPECT_EQ(session.parse_count(), parses);
    EXPECT_EQ(a2.cpp, a.cpp);
    EXPECT_EQ(b2.cpp, b.cpp);
    // A method of one base is a plain script function for the other base.
    const spc::TranslateResult c = spc::translate(
        temp_options(dir, "using \"w.h\";\nclass Baz : Panel {\n    void Update(float dt) { }\n}\n"), session);
    EXPECT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(c.slots_out.find("Baz::Update"), std::string::npos) << c.slots_out;
}

TEST(SessionCache, UnknownBaseAndBadImportsFail)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;
    spc::TranslateResult r = spc::translate(
        temp_options(dir, "using \"w.h\";\nclass Foo : Nothing {\n    void f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("not found"), std::string::npos) << r.diagnostics;
    r = spc::translate(
        temp_options(dir, "using \"missing.h\";\nclass Foo : Widget {\n    void f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    // A good call still works after the failures.
    EXPECT_TRUE(spc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
}

TEST(SessionCache, SecondTranslateIsFast)
{
    const spc::TranslateOptions o = engine_options("player_v1.script", 1);
    spc::TranslatorSession session;
    Clock::time_point t = Clock::now();
    ASSERT_TRUE(spc::translate(o, session).ok);
    const double first = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    t = Clock::now();
    ASSERT_TRUE(spc::translate(o, session).ok);
    const double second = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    std::cout << "translate first=" << first << " ms, second=" << second << " ms\n";
    RecordProperty("first_ms", static_cast<int>(first));
    RecordProperty("second_ms", static_cast<int>(second));
    EXPECT_LT(second, 150.0);  // the target is 50 ms; the bound is wide to avoid a flaky test
}

TEST(SessionCache, SessionKeepsTheHierarchy)
{
    spc::TranslatorSession session;
    const spc::TranslateResult a =
        hier_translate(kIntrusiveStub, "    Item item;\n", false, &session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    const spc::TranslateResult b =
        hier_translate(kIntrusiveStub, "    Item item;\n", false, &session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_TRUE(has(b.cpp, "    " + kIntr + " item;\n")) << b.cpp;
}
