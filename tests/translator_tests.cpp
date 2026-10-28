// Tests for the cache of spc::TranslatorSession.
//
// The cache keeps the virtual methods of the native base classes. A hit must give
// the same output as a miss. A change of a header must invalidate the cache.
// CMake defines SPP_SOURCE_DIR.
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "diag.h"
#include "parser/parser.h"
#include "spp/spc_lib.h"

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
    o.script_name = root + "/example/scripts/" + script_file;
    o.script_text = read_file(o.script_name);
    o.generation = generation;
    o.include_dirs = {root + "/example/engine", root + "/include"};
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

std::string widget_script(const std::string& method, const std::string& mods = "")
{
    return "using \"w.h\";\nclass Foo : Widget {\n    " + mods + "void " + method + " { }\n}\n";
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

TEST(TranslatorSession, ChangedNestedHeaderInvalidatesTheCache)
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

TEST(TranslatorSession, OtherBaseClassWorksInTheSameSession)
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

TEST(TranslatorSession, UnknownBaseAndBadImportsFail)
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

// Both forms of a system import give #include <name> in the output.
TEST(TranslatorImports, SystemImportForms)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string body = "class Foo : Widget {\n    void f() { }\n}\n";
    spc::TranslatorSession session;
    for (const char* form : {"using <vector>;\nusing <sys/types.h>;\n",
                             "using \"<vector>\";\nusing \"<sys/types.h>\";\n"}) {
        spc::TranslateResult r =
            spc::translate(temp_options(dir, std::string(form) + "using \"w.h\";\n" + body), session);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        EXPECT_NE(r.cpp.find("#include <vector>\n"), std::string::npos) << r.cpp;
        EXPECT_NE(r.cpp.find("#include <sys/types.h>\n"), std::string::npos);
        EXPECT_NE(r.cpp.find("#include \"w.h\"\n"), std::string::npos);
        EXPECT_EQ(r.cpp.find("\"<vector>\""), std::string::npos);
    }
}

// A missing system header gives an error with the script name and the line.
TEST(TranslatorImports, MissingSystemHeaderNamesScriptAndLine)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;
    for (const char* form : {"using <no_such_header>;", "using \"<no_such_header>\";"}) {
        spc::TranslateOptions o =
            temp_options(dir, std::string("using \"w.h\";\n") + form +
                                  "\nclass Foo : Widget {\n    void f() { }\n}\n");
        spc::TranslateResult r = spc::translate(o, session);
        EXPECT_FALSE(r.ok);
        EXPECT_NE(r.diagnostics.find(o.script_name + ":2:"), std::string::npos) << r.diagnostics;
        EXPECT_NE(r.diagnostics.find("'no_such_header' file not found"), std::string::npos)
            << r.diagnostics;
    }
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
    return "using \"w.h\";\nclass Foo : Widget {\n    int n = 0;\n    void run(int* p) {\n" + body +
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
        temp_options(dir, lambda_script("        var f = (float x, int* q) => { q.foo(); this.n = x; };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](float x, int* q)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("script::deref(q).foo();"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->n = x;"), std::string::npos) << r.cpp;
}

TEST(TranslatorLambda, ParameterWithoutTypeIsAuto)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, lambda_script("        var f = (x, int y) => { };")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("[=](auto x, int y)"), std::string::npos) << r.cpp;
}

TEST(TranslatorLambda, OldParameterFormIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, lambda_script("        var f = (x: int) => { };")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("parameter syntax changed; write 'Type name'"),
              std::string::npos)
        << r.diagnostics;
}

// Golden-output tests. The files in tests/golden/ hold the generated C++ and the slot
// file of each example script. A later parser port must give the same bytes.
// Set SPP_UPDATE_GOLDEN to write the files and pass.
void check_golden(const std::string& name, const std::string& text)
{
    const fs::path path = fs::path(SPP_SOURCE_DIR) / "tests" / "golden" / name;
    if (std::getenv("SPP_UPDATE_GOLDEN") != nullptr) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return;
    }
    ASSERT_TRUE(fs::exists(path)) << "missing golden file: " << path;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str(), text) << "golden file differs: " << name;
}

// Translates "<base>_v1" as generation 1 and "<base>_v2" as generation 2.
// The slot input of generation 2 is the slot output of generation 1.
void golden_pair(const std::string& base)
{
    std::string slots;
    for (int gen = 1; gen <= 2; ++gen) {
        const std::string file = base + "_v" + std::to_string(gen) + ".script";
        spc::TranslateOptions o = engine_options(file.c_str(), gen);
        o.script_name = file;  // the file name only, so the output has no absolute path
        o.slots_in = slots;
        const spc::TranslateResult r = spc::translate(o);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        const std::string stem = base + "_v" + std::to_string(gen) + ".g" + std::to_string(gen);
        check_golden(stem + ".cpp", r.cpp);
        check_golden(stem + ".slots", r.slots_out);
        slots = r.slots_out;
    }
}

TEST(Golden, Player) { golden_pair("player"); }
TEST(Golden, NativeStd) { golden_pair("native_std"); }
TEST(Golden, Timer) { golden_pair("timer"); }

// Translates one script as generation 1. The script has no second generation.
void golden_one(const std::string& base)
{
    const std::string file = base + ".script";
    spc::TranslateOptions o = engine_options(file.c_str(), 1);
    o.script_name = file;
    const spc::TranslateResult r = spc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    check_golden(base + ".g1.cpp", r.cpp);
    check_golden(base + ".g1.slots", r.slots_out);
}

TEST(Golden, Props) { golden_one("props_v1"); }

}  // namespace

// ---------------------------------------------------------------- TranslatorDiag
// Count the "error:" lines in the diagnostics text.
int count_errors(const std::string& text)
{
    int n = 0;
    for (size_t p = text.find("error:"); p != std::string::npos; p = text.find("error:", p + 1)) ++n;
    return n;
}

spc::TranslateResult translate_text(const std::string& script)
{
    spc::TranslateOptions o;
    o.script_name = "t.script";
    o.script_text = script;
    o.generation = 1;
    return spc::translate(o);
}

TEST(TranslatorDiag, TwoBrokenMembersGiveTwoErrors)
{
    const spc::TranslateResult r = translate_text(
        "class A : B {\n"
        "    x : ;\n"
        "    void ok() { }\n"
        "    [alias(1)] int y;\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 2) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:2:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:4:"), std::string::npos) << r.diagnostics;
}

// The syntax errors of two bodies and an unknown annotation come in source order, each with
// a line and a column.
TEST(TranslatorDiag, BodyErrorsAndAnnotationErrorsComeInSourceOrder)
{
    const spc::TranslateResult r = translate_text(
        "class A : B {\n"
        "    void f() { var x = ; }\n"
        "    void g() { var y = 1 + ; }\n"
        "}\n"
        "class C : B {\n"
        "    [foo(\"x\")] int n;\n"
        "    void h() { var z = ; }\n"
        "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cpp.empty());
    std::vector<std::string> prefixes;
    std::istringstream in(r.diagnostics);
    for (std::string line; std::getline(in, line);) {
        const size_t e = line.find(": error:");
        ASSERT_NE(e, std::string::npos) << r.diagnostics;
        prefixes.push_back(line.substr(0, e));
    }
    EXPECT_EQ(prefixes, (std::vector<std::string>{"t.script:2:24", "t.script:3:28", "t.script:6:6",
                                                  "t.script:7:24"}))
        << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("unknown attribute 'foo'"), std::string::npos) << r.diagnostics;
}

// One fault gives one error. The end of the file after an unterminated comment or literal
// is not a new fault.
TEST(TranslatorDiag, OneFaultGivesOneError)
{
    const char* const scripts[] = {
        "class A : B {\n    void f() { var s = \"abc; }\n}\n",
        "class A : B {\n    void f() { var s = 1; /* open }\n}\n",
        "class A : B { }\n/* open\n",
        "foo bar baz\n",
        "class A : B {\n    string x = \"abc;\n}\n",
    };
    for (const char* text : scripts) {
        const spc::TranslateResult r = translate_text(text);
        EXPECT_FALSE(r.ok) << text;
        EXPECT_EQ(count_errors(r.diagnostics), 1) << text << "\n" << r.diagnostics;
    }
}

TEST(TranslatorDiag, StrayControlByteHasNoFoundText)
{
    const spc::TranslateResult r = translate_text(std::string("\0abc\n", 5));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.diagnostics, "t.script:1:1: error: expected 'using' or 'class'\n") << r.diagnostics;
}

TEST(TranslatorDiag, DeepNestingGivesOneError)
{
    const std::string deep(300, '{');
    const spc::TranslateResult r = translate_text("class A : B {\n    void f() " + deep +
                                                  std::string(300, '}') + "\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 1) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("maximum recursion depth exceeded"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorDiag, BadImportKeepsItsText)
{
    const spc::TranslateResult r = translate_text("using 5;\nclass A : B { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("using needs"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:1:7: error:"), std::string::npos) << r.diagnostics;
}

TEST(TranslatorDiag, ImportIsRemoved)
{
    const spc::TranslateResult r = translate_text("import \"x.h\";\nclass A : B { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'import' is removed; write 'using'"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorDiag, UnterminatedBraceEndsWithAnError)
{
    const spc::TranslateResult r = translate_text("class A : B {\n    void f() {\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unexpected end of file"), std::string::npos) << r.diagnostics;
}

TEST(TranslatorDiag, UnterminatedCommentEndsWithAnError)
{
    const spc::TranslateResult r = translate_text("class A : B { }\n/* open\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unterminated comment"), std::string::npos) << r.diagnostics;
    const spc::TranslateResult r2 = translate_text("class A : B { void f() { /* open }\n");
    EXPECT_FALSE(r2.ok);
    EXPECT_FALSE(r2.diagnostics.empty());
}

TEST(TranslatorDiag, AnyByteIsAccepted)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    // The byte 0xE9 is not valid UTF-8 here. It is in a comment, a string and a body comment.
    const std::string script =
        "using \"w.h\";\n// caf\xE9\nclass Foo : Widget {\n    int n = 3;  // caf\xE9\n"
        "    override void Update(float dt) { /* \xE9 */ const char* t = \"na\xE9\"; t; }\n}\n";
    const spc::TranslateResult r = spc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("\"na\xE9\""), std::string::npos) << r.cpp;
}

TEST(TranslatorDiag, DoubleColonIsNotAField)
{
    const spc::TranslateResult r = translate_text("class A : B {\n    x::y : int;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("t.script:2:6: error: use '.' instead of '::'"),
              std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorDiag, BacktickInATypeIsAnError)
{
    const spc::TranslateResult r = translate_text("class A : B {\n    Foo`a` x;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("string interpolation is not supported"), std::string::npos)
        << r.diagnostics;
}

// A script cut at any byte must end with a result and not hang. A failure has a text.
TEST(TranslatorDiag, TruncatedInputsTerminate)
{
    // The parser runs on every prefix (cut at each byte). A prefix that parses runs the whole
    // translator and libclang (about 15 ms), so the full translation uses each 16th cut only.
    const auto start = Clock::now();
    size_t first = 0;
    for (const char* base : {"player", "native_std", "timer"}) {
        for (int gen = 1; gen <= 2; ++gen, ++first) {
            const std::string file = std::string(base) + "_v" + std::to_string(gen) + ".script";
            const spc::TranslateOptions full = engine_options(file.c_str(), gen);
            spc::TranslatorSession session;  // the header parse is cached
            for (size_t cut = 0; cut <= full.script_text.size(); ++cut) {
                const std::string text = full.script_text.substr(0, cut);
                const spc::LineMap map(text);
                spc::Diagnostics diag;
                (void)spc::parse_script(text, map, diag);
                if (cut % 16 != first * 3 % 16) continue;
                spc::TranslateOptions o = full;
                o.script_text = text;
                const spc::TranslateResult r = spc::translate(o, session);
                if (!r.ok) {
                    ASSERT_FALSE(r.diagnostics.empty()) << file << " cut at " << cut;
                }
            }
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    EXPECT_LT(ms.count(), 5000);
}

// ---------------------------------------------------------------- Diag
TEST(Diag, LineMapFirstChar)
{
    spc::LineMap m("ab\ncd");
    EXPECT_EQ(m.line(0), 1);
    EXPECT_EQ(m.col(0), 1);
}

TEST(Diag, LineMapCharAfterNewline)
{
    spc::LineMap m("ab\ncd");
    EXPECT_EQ(m.line(2), 1);  // the newline char
    EXPECT_EQ(m.col(2), 3);
    EXPECT_EQ(m.line(3), 2);
    EXPECT_EQ(m.col(3), 1);
}

TEST(Diag, LineMapLastCharWithAndWithoutTrailingNewline)
{
    spc::LineMap a("ab\ncd");
    EXPECT_EQ(a.line(4), 2);
    EXPECT_EQ(a.col(4), 2);
    spc::LineMap b("ab\ncd\n");
    EXPECT_EQ(b.line(4), 2);
    EXPECT_EQ(b.col(4), 2);
    EXPECT_EQ(b.line(5), 2);  // the trailing newline
    EXPECT_EQ(b.col(5), 3);
    EXPECT_EQ(b.line(6), 3);  // the end of the text
    EXPECT_EQ(b.col(6), 1);
}

TEST(Diag, LineMapEmptyText)
{
    spc::LineMap m("");
    EXPECT_EQ(m.line(0), 1);
    EXPECT_EQ(m.col(0), 1);
}

TEST(Diag, FormatWithColumn)
{
    spc::LineMap m("ab\ncd");
    spc::Diagnostics d;
    d.add(4, m, "bad token");
    EXPECT_EQ(d.format("a.spp"), "a.spp:2:2: error: bad token\n");
}

TEST(Diag, FormatWithoutColumn)
{
    spc::Diagnostics d;
    d.add_line(7, "bad line");
    d.add_line(9, "second");
    EXPECT_EQ(d.format("a.spp"), "a.spp:7: error: bad line\na.spp:9: error: second\n");
}

TEST(Diag, FormatWithLineZero)
{
    spc::Diagnostics d;
    d.add_line(0, "no line");
    EXPECT_EQ(d.format("a.spp"), "spc: error: no line\n");
}

// ------------------------------------------------------------------ function and [alias]
// The error text for a script text that needs no header.
std::string diag_of(const std::string& script)
{
    return translate_text(script).diagnostics;
}

TEST(TranslatorSyntax, OverrideBeforeFunctionGivesOneError)
{
    EXPECT_EQ(diag_of("class A : B {\n    override function f() { }\n    void g() { }\n}\n"),
              "t.script:2:14: error: 'function' is removed; write 'Ret name(...)'\n");
}

TEST(TranslatorSyntax, JunkInsteadOfAMemberIsAnError)
{
    EXPECT_NE(diag_of("class A : B {\n    123 f() { }\n}\n")
                  .find("t.script:2:5: error: expected a member: 'Type name;' or 'Ret name(...)', "
                        "found '123'"),
              std::string::npos);
}

TEST(TranslatorSyntax, FunctionKeywordIsRemoved)
{
    EXPECT_EQ(diag_of("class A : B {\n    function f() { }\n}\n"),
              "t.script:2:5: error: 'function' is removed; write 'Ret name(...)'\n");
}

TEST(TranslatorSyntax, OldFieldFormIsAnError)
{
    EXPECT_EQ(diag_of("class A : B {\n    n: int;\n    int m;\n}\n"),
              "t.script:2:5: error: field syntax changed; write 'Type name;', found 'n'\n");
}

TEST(TranslatorSyntax, ArrowReturnTypeIsRemoved)
{
    EXPECT_EQ(diag_of("class A : B {\n    int f() -> int { return 1; }\n}\n"),
              "t.script:2:13: error: '->' return type is removed; write the type before the "
              "name\n");
}

TEST(TranslatorSyntax, MethodNeedsAReturnType)
{
    EXPECT_NE(diag_of("class A : B {\n    f() { }\n}\n")
                  .find("t.script:2:5: error: a method needs a return type; write 'Ret name(...)'"),
              std::string::npos);
}

TEST(TranslatorSyntax, UnknownAttribute)
{
    EXPECT_EQ(diag_of("class A : B {\n    [foo(\"x\")] int n;\n}\n"),
              "t.script:2:6: error: unknown attribute 'foo'\n");
}

TEST(TranslatorSyntax, AliasNeedsAString)
{
    const std::string d = diag_of("class A : B {\n    [alias(x)] int n;\n}\n");
    EXPECT_EQ(d.rfind("t.script:2:12: error: expected a string in [alias(...)]", 0), 0u) << d;
}

TEST(TranslatorSyntax, AliasOnAMethod)
{
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"x\")] void f() { }\n}\n"),
              "t.script:2:5: error: [alias] applies to fields only\n");
}

TEST(TranslatorSyntax, OldWasSyntaxHasAHint)
{
    EXPECT_EQ(diag_of("class A : B {\n    @was(\"x\") int n;\n}\n"),
              "t.script:2:5: error: use [alias(...)] instead of @was(...)\n");
}

TEST(TranslatorSyntax, TwoAttributesInAList)
{
    // The second attribute is checked like the first one.
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), foo(\"b\")] int n;\n}\n"),
              "t.script:2:18: error: unknown attribute 'foo'\n");
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), alias(\"b\")] int n;\n}\n"),
              "t.script:2:18: error: duplicate attribute 'alias'\n");
}

TEST(TranslatorSyntax, AliasGoesToTheReflectionRow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(
        temp_options(dir, "using \"w.h\";\nclass Foo : Widget {\n"
                          "    [alias(\"old\")] int n = 0;\n    int m = 0;\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("{\"n\", \"old\","), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("{\"m\", nullptr,"), std::string::npos) << r.cpp;
}

TEST(TranslatorOverride, ExplicitOverrideUsesTheVirtualSlot)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // An override gets no entry in the slot file. A plain function gets one.
    EXPECT_EQ(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;
    EXPECT_NE(r.cpp.find("Foo__Update(Widget* a"), std::string::npos) << r.cpp;
}

TEST(TranslatorOverride, MissingOverrideIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Update(float dt)")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("method 'Update' overrides a virtual method of Widget; write 'override'"),
              std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorOverride, OverrideWithoutBaseMethodIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Nothing()", "override ")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'override' has no virtual method to override"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorOverride, NewOnAVirtualMethodIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Update(float dt)", "new ")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'new' cannot hide a virtual method; write 'override'"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorOverride, ScriptOnlyMethodNeedsNoModifier)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Extra()")));
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(TranslatorOverride, SameNameWithOtherParametersHidesTheVirtualMethod)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, widget_script("Update(int n)")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("function 'Update' hides the virtual method of Widget. "
                                 "Candidates: "),
              std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorOverride, FinalMethodStaysAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h",
               "class Widget { public: virtual ~Widget() {} virtual void Lock() final {} };\n");
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Lock()", "override ")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("is final and cannot be overridden"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorOverride, FunctionChecksUseTheWordFunction)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string head = "using \"w.h\";\nclass Foo : Widget {\n";
    spc::TranslateResult r = spc::translate(
        temp_options(dir, head + "    void f() { }\n    void f() { }\n}\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("function 'f' is declared twice (overloads are not supported)"),
              std::string::npos)
        << r.diagnostics;
    r = spc::translate(temp_options(dir, head + "    int f;\n    void f() { }\n}\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'f' is a field and a function"), std::string::npos)
        << r.diagnostics;
}

// ------------------------------------------------------------ scope in script text
// The script writes "." for a scope. The generated C++ uses "::".

const char* kScopeHeader =
    "#include <string>\n#include <vector>\n"
    "namespace Engine { class Actor { public: virtual ~Actor() {} virtual void Tick(float dt) {} }; }\n";

TEST(TranslatorScope, FieldTypesAndBaseClassUseDoubleColons)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kScopeHeader);
    const std::string script =
        "using \"w.h\";\nclass P : Engine.Actor {\n    std.string title;\n"
        "    std.vector<int> history;\n    override void Tick(float dt) { }\n}\n";
    const spc::TranslateResult r = spc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("std::string title"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("std::vector<int> history"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Engine::Actor"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Engine.Actor"), std::string::npos) << r.cpp;
}

TEST(TranslatorScope, MemberAccessUsesDeref)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kScopeHeader);
    const std::string script =
        "using \"w.h\";\nclass P : Engine.Actor {\n    std.string title;\n"
        "    override void Tick(float dt) { var n = this.title.size(); }\n}\n";
    const spc::TranslateResult r = spc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("script::deref(st->title).size()"), std::string::npos) << r.cpp;
}

TEST(TranslatorScope, DoubleColonInABaseClassIsAnError)
{
    const spc::TranslateResult r = translate_text("class P : Engine::Actor {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("use '.' instead of '::'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:1:"), std::string::npos) << r.diagnostics;
}

TEST(TranslatorScope, DoubleColonInAFieldTypeIsAnError)
{
    const spc::TranslateResult r = translate_text("class P : B {\n    std::string title;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("use '.' instead of '::'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:2:"), std::string::npos) << r.diagnostics;
}

TEST(TranslatorScope, ArrowInABodyIsAnError)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, lambda_script("        p->foo();")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("use '.' instead of '->'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:5:"), std::string::npos) << r.diagnostics;
}

// ---------------------------------------------------------------- TranslatorProps
namespace {

std::string prop_script(const std::string& members)
{
    return "using \"w.h\";\nclass Foo : Widget {\n" + members + "\n}\n";
}

spc::TranslateResult translate_props(const std::string& members)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    return spc::translate(temp_options(dir, prop_script(members)));
}

void expect_prop_error(const std::string& members, const std::string& msg)
{
    const spc::TranslateResult r = translate_props(members);
    EXPECT_FALSE(r.ok) << members;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << members << "\n" << r.diagnostics;
}

}  // namespace

TEST(TranslatorProps, AutoPropertyLowersToAccessorCalls)
{
    const spc::TranslateResult r = translate_props(
        "    int N { get; set; } = 3;\n"
        "    void run() { N = 1; this.N += 2; N++; int k = this.N + N; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("    int __p_N;\n"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("new (f) T(3);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("static int Foo__get_N(Foo_Shell* self)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("static void Foo__set_N(Foo_Shell* self, int value)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return st->__p_N;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->__p_N = value;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_N(self, 1);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_N(self, Foo__get_N(self) + (2));"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_N(self, Foo__get_N(self) + 1);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("int k = Foo__get_N(self) + Foo__get_N(self);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.slots_out.find("Foo::get_N 1"), std::string::npos) << r.slots_out;
    EXPECT_NE(r.slots_out.find("Foo::set_N 1"), std::string::npos) << r.slots_out;
}

TEST(TranslatorProps, CustomAccessorsBecomeFunctions)
{
    const spc::TranslateResult r = translate_props(
        "    int raw = 0;\n"
        "    int V { get { return this.raw * 2; } set { this.raw = value / 2; } }\n"
        "    int run(int value) { V = value; return V; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("__p_V"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return st->raw * 2;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("st->raw = value / 2;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("Foo__set_V(self, value);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return Foo__get_V(self);"), std::string::npos) << r.cpp;
}

TEST(TranslatorProps, LocalHidesProperty)
{
    const spc::TranslateResult r =
        translate_props("    int N { get; set; }\n    void run() { int N = 2; N = 3; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("N = 3;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Foo__set_N(self, 3)"), std::string::npos) << r.cpp;
}

// "w.N" on another object of the class calls the accessors through script::shell_of.
TEST(TranslatorProps, OtherObjectUsesAccessors)
{
    const spc::TranslateResult r = translate_props(
        "    int N { get; set; }\n"
        "    int hp = 0;\n"
        "    int run(Widget& w, Widget* q) {\n"
        "        w.N = 1;\n"
        "        q.N += 2;\n"
        "        w.N++;\n"
        "        int a = this.N = q.N = 4;\n"
        "        int b = (w.N = 5);\n"
        "        return w.N + q.N;\n"
        "    }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const std::string w = "script::shell_of<Foo_Shell>(w, \"Foo\")";
    const std::string q = "script::shell_of<Foo_Shell>(q, \"Foo\")";
    EXPECT_NE(r.cpp.find("Foo__set_N(" + w + ", 1);"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("script::prop_update<script::op_add>(Foo__get_N, Foo__set_N, " + q + ", 2);"),
              std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("script::prop_update<script::op_add>(Foo__get_N, Foo__set_N, " + w + ", 1);"),
              std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("int a = script::prop_set(Foo__set_N, self, script::prop_set(Foo__set_N, " + q +
                         ", 4));"),
              std::string::npos)
        << r.cpp;
    EXPECT_NE(r.cpp.find("int b = (script::prop_set(Foo__set_N, " + w + ", 5));"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("return Foo__get_N(" + w + ") + Foo__get_N(" + q + ");"), std::string::npos) << r.cpp;
}

// A member that is not a property keeps the native member access.
TEST(TranslatorProps, OtherObjectPlainMemberIsNotRewritten)
{
    const spc::TranslateResult r = translate_props(
        "    int N { get; set; }\n"
        "    void run(Foo2& p) { p.hp = 1; p.hp += p.n; }");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("script::deref(p).hp = 1;"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("script::deref(p).hp += script::deref(p).n;"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("Foo__set_hp"), std::string::npos) << r.cpp;
}

TEST(TranslatorProps, Errors)
{
    expect_prop_error("    int N { get; }\n    void run(Widget& w) { w.N = 1; }",
                      "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { get; }\n    void run(Widget* w) { w.N++; }",
                      "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { set; }\n    int run(Widget& w) { return w.N; }",
                      "property 'N' has no 'get' accessor");
    expect_prop_error("    int N { get; }\n    void run() { N = 1; }",
                      "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { get; }\n    void run() { this.N += 1; }",
                      "property 'N' has no 'set' accessor");
    expect_prop_error("    int N { set; }\n    int run() { return N; }",
                      "property 'N' has no 'get' accessor");
    expect_prop_error("    int N { }", "a property needs a 'get' or a 'set' accessor");
    expect_prop_error("    int N { get; get; }", "duplicate 'get' accessor");
    expect_prop_error("    int N { get; set { } }", "a property cannot mix auto and custom accessors");
    expect_prop_error("    int N { get { return 1; } } = 2;",
                      "only an auto property can have an initializer");
    expect_prop_error("    int N { get; fetch; }", "expected 'get' or 'set'");
    expect_prop_error("    int N;\n    int N { get; }", "'N' is a field and a property");
    expect_prop_error("    int N { get; }\n    void N() { }", "'N' is a method and a property");
    expect_prop_error("    int N { get; }\n    int N { set; }", "property 'N' is declared twice");
    expect_prop_error("    int N { get; }\n    int get_N() { return 1; }",
                      "method 'get_N' collides with the 'get' accessor of property 'N'");
    expect_prop_error("    int N { get; set; }\n    int m = N;",
                      "property 'N' is not allowed in a field initializer");
    expect_prop_error("    override int N { get; }", "'override' is only valid on a method");
    expect_prop_error("    int get;", "'get' is a reserved word");
    expect_prop_error("    void set() { }", "'set' is a reserved word");
    expect_prop_error("    void f(int get) { }", "'get' is a reserved word");
    expect_prop_error("    int get { get; }", "'get' is a reserved word");
}
