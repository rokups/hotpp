// Tests for the cache of spc::TranslatorSession.
//
// The cache keeps the virtual methods of the native base classes. A hit must give
// the same output as a miss. A change of a header must invalidate the cache.
// CMake defines SPP_SOURCE_DIR.
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "spc_lib.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream(path) << text;
}

// A new empty directory for the headers of one test.
class TempDir {
public:
    TempDir()
    {
        static int n = 0;
        path_ = fs::temp_directory_path() /
                ("spc_session_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

spc::TranslateOptions engine_options(const char* script_file, int generation)
{
    const std::string root = SPP_SOURCE_DIR;
    spc::TranslateOptions o;
    o.script_name = root + "/scripts/" + script_file;
    o.script_text = read_file(o.script_name);
    o.generation = generation;
    o.include_dirs = {root + "/engine", root + "/runtime"};
    return o;
}

spc::TranslateOptions temp_options(const TempDir& dir, const std::string& script_text)
{
    spc::TranslateOptions o;
    o.script_name = "t.script";
    o.script_text = script_text;
    o.generation = 1;
    o.include_dirs = {dir.path().string()};
    return o;
}

const char* kHeaderV1 =
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n";

std::string widget_script(const std::string& method)
{
    return "import \"w.h\";\nclass Foo : Widget {\n    override " + method + " { }\n}\n";
}

TEST(TranslatorSession, HitGivesTheSameOutputAsMiss)
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

TEST(TranslatorSession, ChangedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;

    EXPECT_TRUE(spc::translate(temp_options(dir, widget_script("Update(dt: float)")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    // The new virtual method is not in the header yet.
    spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("no virtual method Draw"), std::string::npos) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);

    // The header gets a new virtual method. The new size shows the change.
    write_file(dir.path() / "w.h",
               "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} "
               "virtual void Draw() {} };\n"
               "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.cpp.find("Draw"), std::string::npos);

    // A change with the same size and a new time also invalidates the cache.
    const std::string same_size = read_file((dir.path() / "w.h").string());
    std::string edited = same_size;
    edited.replace(edited.find("Draw"), 4, "Dram");
    write_file(dir.path() / "w.h", edited);
    fs::last_write_time(dir.path() / "w.h", fs::last_write_time(dir.path() / "w.h") + std::chrono::seconds(5));
    r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(session.parse_count(), 3);
    r = spc::translate(temp_options(dir, widget_script("Dram()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 3);
}

TEST(TranslatorSession, ChangedNestedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "inner.h", "#define EXTRA\n");
    write_file(dir.path() / "w.h",
               "#include \"inner.h\"\n"
               "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {}\n"
               "#ifdef EXTRA\n virtual void Draw() {}\n#endif\n};\n");
    spc::TranslatorSession session;
    EXPECT_TRUE(spc::translate(temp_options(dir, widget_script("Draw()")), session).ok);
    write_file(dir.path() / "inner.h", "// no macro now\n");
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(session.parse_count(), 2);
}

TEST(TranslatorSession, OtherBaseClassWorksInTheSameSession)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;

    const spc::TranslateResult a = spc::translate(temp_options(dir, widget_script("Update(dt: float)")), session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string panel_script =
        "import \"w.h\";\nclass Bar : Panel {\n    override Layout(w: int) { }\n}\n";
    const spc::TranslateResult b = spc::translate(temp_options(dir, panel_script), session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(b.cpp.find("Layout"), std::string::npos);
    // Both bases are in the cache now. The next calls do not parse.
    const int parses = session.parse_count();
    const spc::TranslateResult a2 = spc::translate(temp_options(dir, widget_script("Update(dt: float)")), session);
    const spc::TranslateResult b2 = spc::translate(temp_options(dir, panel_script), session);
    EXPECT_EQ(session.parse_count(), parses);
    EXPECT_EQ(a2.cpp, a.cpp);
    EXPECT_EQ(b2.cpp, b.cpp);
    // A method of one base is not valid for the other base.
    const spc::TranslateResult c = spc::translate(
        temp_options(dir, "import \"w.h\";\nclass Baz : Panel {\n    override Update(dt: float) { }\n}\n"), session);
    EXPECT_FALSE(c.ok);
}

TEST(TranslatorSession, UnknownBaseAndBadImportsFail)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;
    spc::TranslateResult r = spc::translate(
        temp_options(dir, "import \"w.h\";\nclass Foo : Nothing {\n    fn f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("not found"), std::string::npos) << r.diagnostics;
    r = spc::translate(
        temp_options(dir, "import \"missing.h\";\nclass Foo : Widget {\n    fn f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    // A good call still works after the failures.
    EXPECT_TRUE(spc::translate(temp_options(dir, widget_script("Update(dt: float)")), session).ok);
}

TEST(TranslatorSession, SecondTranslateIsFast)
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

// ------------------------------------------------------------ arrow lambda

std::string lambda_script(const std::string& body)
{
    return "import \"w.h\";\nclass Foo : Widget {\n    n: int = 0;\n    fn run(p: int*) {\n" + body +
           "\n    }\n}\n";
}

TEST(TranslatorLambda, ArrowLambdaBecomesAClosureThatCapturesByValue)
{
    const spc::TranslateOptions o = engine_options("timer_v1.script", 1);
    const spc::TranslateResult r = spc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("script::make_closure(GEN, \"Beeper::arm lambda\", [=]()"),
              std::string::npos)
        << r.cpp;
    // The body uses self and a new state pointer, not the pointer of the outer function.
    EXPECT_NE(r.cpp.find("auto* st = state_of(self);\n#line 15"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->fired = st->fired + 1;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("=>"), std::string::npos);
}

TEST(TranslatorLambda, ParametersGetTypesAndPointersUseArrow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(
        temp_options(dir, lambda_script("        let f = (x: float, q: int*) => { q.foo(); this.n = x; };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](float x, int* q)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("q->foo();"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->n = x;"), std::string::npos) << r.cpp;
}

TEST(TranslatorLambda, ParameterWithoutTypeIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, lambda_script("        let f = (x) => { };")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("name: type"), std::string::npos) << r.diagnostics;
}

}  // namespace
