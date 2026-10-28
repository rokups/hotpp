// Tests for the cache of sppc::TranslatorSession.
//
// The cache keeps the virtual methods of the native base classes. A hit must give
// the same output as a miss. A change of a header must invalidate the cache.
// CMake defines SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>

#include "spp/sppc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

using namespace translator_test;
using namespace stub_translate_test;

TEST(SessionCache, HitGivesTheSameOutputAsMiss)
{
    sppc::TranslateOptions o = engine_options("player_v1.spp", 1);
    const sppc::TranslateResult alone = sppc::translate(o);  // a temporary session
    ASSERT_TRUE(alone.ok) << alone.diagnostics;

    sppc::TranslatorSession session;
    const sppc::TranslateResult miss = sppc::translate(o, session);
    const sppc::TranslateResult hit = sppc::translate(o, session);
    ASSERT_TRUE(miss.ok) << miss.diagnostics;
    ASSERT_TRUE(hit.ok) << hit.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(miss.cpp, alone.cpp);
    EXPECT_EQ(hit.cpp, alone.cpp);
    EXPECT_EQ(hit.slots_out, alone.slots_out);

    // The second generation also hits and agrees with a call without a session.
    sppc::TranslateOptions o2 = engine_options("player_v2.spp", 2);
    o2.slots_in = alone.slots_out;
    const sppc::TranslateResult alone2 = sppc::translate(o2);
    const sppc::TranslateResult hit2 = sppc::translate(o2, session);
    ASSERT_TRUE(hit2.ok) << hit2.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(hit2.cpp, alone2.cpp);
    EXPECT_EQ(hit2.slots_out, alone2.slots_out);
}

TEST(SessionCache, ChangedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    sppc::TranslatorSession session;

    EXPECT_TRUE(sppc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    // The new virtual method is not in the header yet. Draw is a plain script function.
    sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;
    EXPECT_EQ(session.parse_count(), 1);

    // The header gets a new virtual method. The new size shows the change.
    write_file(dir.path() / "w.h",
               "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} "
               "virtual void Draw() {} };\n"
               "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    r = sppc::translate(temp_options(dir, widget_script("Draw()", "override ")), session);
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
    r = sppc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;  // not an override
    EXPECT_EQ(session.parse_count(), 3);
    r = sppc::translate(temp_options(dir, widget_script("Dram()", "override ")), session);
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
    sppc::TranslatorSession session;
    const sppc::TranslateResult first = sppc::translate(temp_options(dir, widget_script("Draw()", "override ")), session);
    EXPECT_TRUE(first.ok) << first.diagnostics;
    EXPECT_EQ(first.slots_out.find("Foo::Draw"), std::string::npos) << first.slots_out;  // an override
    write_file(dir.path() / "inner.h", "// no macro now\n");
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;  // not an override
    EXPECT_EQ(session.parse_count(), 2);
}

TEST(SessionCache, OtherBaseClassWorksInTheSameSession)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    sppc::TranslatorSession session;

    const sppc::TranslateResult a = sppc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string panel_script =
        "using \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n";
    const sppc::TranslateResult b = sppc::translate(temp_options(dir, panel_script), session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(b.cpp.find("Layout"), std::string::npos);
    // Both bases are in the cache now. The next calls do not parse.
    const int parses = session.parse_count();
    const sppc::TranslateResult a2 = sppc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session);
    const sppc::TranslateResult b2 = sppc::translate(temp_options(dir, panel_script), session);
    EXPECT_EQ(session.parse_count(), parses);
    EXPECT_EQ(a2.cpp, a.cpp);
    EXPECT_EQ(b2.cpp, b.cpp);
    // A method of one base is a plain script function for the other base.
    const sppc::TranslateResult c = sppc::translate(
        temp_options(dir, "using \"w.h\";\nclass Baz : Panel {\n    void Update(float dt) { }\n}\n"), session);
    EXPECT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(c.slots_out.find("Baz::Update"), std::string::npos) << c.slots_out;
}

TEST(SessionCache, UnknownBaseAndBadImportsFail)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    sppc::TranslatorSession session;
    sppc::TranslateResult r = sppc::translate(
        temp_options(dir, "using \"w.h\";\nclass Foo : Nothing {\n    void f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("not found"), std::string::npos) << r.diagnostics;
    r = sppc::translate(
        temp_options(dir, "using \"missing.h\";\nclass Foo : Widget {\n    void f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    // A good call still works after the failures.
    EXPECT_TRUE(sppc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
}

TEST(SessionCache, SecondTranslateIsFast)
{
    const sppc::TranslateOptions o = engine_options("player_v1.spp", 1);
    sppc::TranslatorSession session;
    Clock::time_point t = Clock::now();
    ASSERT_TRUE(sppc::translate(o, session).ok);
    const double first = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    t = Clock::now();
    ASSERT_TRUE(sppc::translate(o, session).ok);
    const double second = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    std::cout << "translate first=" << first << " ms, second=" << second << " ms\n";
    RecordProperty("first_ms", static_cast<int>(first));
    RecordProperty("second_ms", static_cast<int>(second));
    EXPECT_LT(second, 150.0);  // the target is 50 ms; the bound is wide to avoid a flaky test
}

TEST(SessionCache, SessionKeepsTheHierarchy)
{
    sppc::TranslatorSession session;
    const sppc::TranslateResult a =
        hier_translate(kIntrusiveStub, "    Item item;\n", false, &session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    const sppc::TranslateResult b =
        hier_translate(kIntrusiveStub, "    Item item;\n", false, &session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_TRUE(has(b.cpp, "    " + kIntr + " item;\n")) << b.cpp;
}

// An edit that keeps the size and the time of the header also invalidates the cache.
TEST(SessionCache, EditWithEqualSizeAndTimeInvalidatesTheCache)
{
    TempDir dir;
    const fs::path header = dir.path() / "w.h";
    write_file(header, kHeaderV1);
    sppc::TranslatorSession session;
    ASSERT_TRUE(sppc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);

    const auto old_time = fs::last_write_time(header);
    std::string edited = read_file(header.string());
    edited.replace(edited.find("Update"), 6, "Updatf");
    write_file(header, edited);
    fs::last_write_time(header, old_time);  // the same time and the same size as before

    // Updatf is a new virtual method. Update is a plain script function now.
    const sppc::TranslateResult r = sppc::translate(temp_options(dir, widget_script("Update(float dt)")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;  // not an override
}

// A new header earlier in the include path hides the header that the cache used.
TEST(SessionCache, NewHeaderEarlierInTheIncludePathInvalidatesTheCache)
{
    TempDir first, second;
    write_file(second.path() / "w.h", kHeaderV1);
    sppc::TranslatorSession session;
    auto options = [&](const std::string& script) {
        sppc::TranslateOptions o = temp_options(first, script);
        o.include_dirs = {first.path().string(), second.path().string()};
        return o;
    };
    ASSERT_TRUE(sppc::translate(options(widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_TRUE(sppc::translate(options(widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);

    // The new header has no Update. The method Update is not an override any more.
    write_file(first.path() / "w.h",
               "class Widget { public: virtual ~Widget() {} virtual void Draw() {} };\n"
               "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    const sppc::TranslateResult r =
        sppc::translate(options(widget_script("Update(float dt)")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;
}
