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

std::string widget_script(const std::string& method)
{
    return "import \"w.h\";\nclass Foo : Widget {\n    function " + method + " { }\n}\n";
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
    r = spc::translate(temp_options(dir, widget_script("Draw()")), session);
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
    r = spc::translate(temp_options(dir, widget_script("Dram()")), session);
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
    const spc::TranslateResult first = spc::translate(temp_options(dir, widget_script("Draw()")), session);
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

    const spc::TranslateResult a = spc::translate(temp_options(dir, widget_script("Update(dt: float)")), session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string panel_script =
        "import \"w.h\";\nclass Bar : Panel {\n    function Layout(w: int) { }\n}\n";
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
    // A method of one base is a plain script function for the other base.
    const spc::TranslateResult c = spc::translate(
        temp_options(dir, "import \"w.h\";\nclass Baz : Panel {\n    function Update(dt: float) { }\n}\n"), session);
    EXPECT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(c.slots_out.find("Baz::Update"), std::string::npos) << c.slots_out;
}

TEST(TranslatorSession, UnknownBaseAndBadImportsFail)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    spc::TranslatorSession session;
    spc::TranslateResult r = spc::translate(
        temp_options(dir, "import \"w.h\";\nclass Foo : Nothing {\n    function f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("not found"), std::string::npos) << r.diagnostics;
    r = spc::translate(
        temp_options(dir, "import \"missing.h\";\nclass Foo : Widget {\n    function f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    // A good call still works after the failures.
    EXPECT_TRUE(spc::translate(temp_options(dir, widget_script("Update(dt: float)")), session).ok);
}

// Both forms of a system import give #include <name> in the output.
TEST(TranslatorImports, SystemImportForms)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string body = "class Foo : Widget {\n    function f() { }\n}\n";
    spc::TranslatorSession session;
    for (const char* form : {"import <vector>;\nimport <sys/types.h>;\n",
                             "import \"<vector>\";\nimport \"<sys/types.h>\";\n"}) {
        spc::TranslateResult r =
            spc::translate(temp_options(dir, std::string(form) + "import \"w.h\";\n" + body), session);
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
    for (const char* form : {"import <no_such_header>;", "import \"<no_such_header>\";"}) {
        spc::TranslateOptions o =
            temp_options(dir, std::string("import \"w.h\";\n") + form +
                                  "\nclass Foo : Widget {\n    function f() { }\n}\n");
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
    return "import \"w.h\";\nclass Foo : Widget {\n    n: int = 0;\n    function run(p: int*) {\n" + body +
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
    EXPECT_NE(r.cpp.find("script::deref(q).foo();"), std::string::npos) << r.cpp;
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
        "    function ok() { }\n"
        "    [alias(1)] y : int;\n"
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
        "    function f() { let x = ; }\n"
        "    function g() { let y = 1 + ; }\n"
        "}\n"
        "class C : B {\n"
        "    [foo(\"x\")] n : int;\n"
        "    function h() { let z = ; }\n"
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
    EXPECT_EQ(prefixes, (std::vector<std::string>{"t.script:2:28", "t.script:3:32", "t.script:6:6",
                                                  "t.script:7:28"}))
        << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("unknown attribute 'foo'"), std::string::npos) << r.diagnostics;
}

// One fault gives one error. The end of the file after an unterminated comment or literal
// is not a new fault.
TEST(TranslatorDiag, OneFaultGivesOneError)
{
    const char* const scripts[] = {
        "class A : B {\n    function f() { let s = \"abc; }\n}\n",
        "class A : B {\n    function f() { let s = 1; /* open }\n}\n",
        "class A : B { }\n/* open\n",
        "foo bar baz\n",
        "class A : B {\n    x : string = \"abc;\n}\n",
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
    EXPECT_EQ(r.diagnostics, "t.script:1:1: error: expected 'import' or 'class'\n") << r.diagnostics;
}

TEST(TranslatorDiag, DeepNestingGivesOneError)
{
    const std::string deep(300, '{');
    const spc::TranslateResult r = translate_text("class A : B {\n    function f() " + deep +
                                                  std::string(300, '}') + "\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(count_errors(r.diagnostics), 1) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("maximum recursion depth exceeded"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorDiag, BadImportKeepsItsText)
{
    const spc::TranslateResult r = translate_text("import 5;\nclass A : B { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("import needs"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("t.script:1:8: error:"), std::string::npos) << r.diagnostics;
}

TEST(TranslatorDiag, UnterminatedBraceEndsWithAnError)
{
    const spc::TranslateResult r = translate_text("class A : B {\n    function f() {\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unexpected end of file"), std::string::npos) << r.diagnostics;
}

TEST(TranslatorDiag, UnterminatedCommentEndsWithAnError)
{
    const spc::TranslateResult r = translate_text("class A : B { }\n/* open\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("unterminated comment"), std::string::npos) << r.diagnostics;
    const spc::TranslateResult r2 = translate_text("class A : B { function f() { /* open }\n");
    EXPECT_FALSE(r2.ok);
    EXPECT_FALSE(r2.diagnostics.empty());
}

TEST(TranslatorDiag, AnyByteIsAccepted)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    // The byte 0xE9 is not valid UTF-8 here. It is in a comment, a string and a body comment.
    const std::string script =
        "import \"w.h\";\n// caf\xE9\nclass Foo : Widget {\n    n : int = 3;  // caf\xE9\n"
        "    function Update(dt: float) { /* \xE9 */ const t: char* = \"na\xE9\"; t; }\n}\n";
    const spc::TranslateResult r = spc::translate(temp_options(dir, script));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("\"na\xE9\""), std::string::npos) << r.cpp;
}

TEST(TranslatorDiag, DoubleColonIsNotAField)
{
    const spc::TranslateResult r = translate_text("class A : B {\n    x::y : int;\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("expected a field, '[' or 'function', found 'x'"),
              std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorDiag, BacktickInATypeIsAnError)
{
    const spc::TranslateResult r = translate_text("class A : B {\n    x : Foo`a`;\n}\n");
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

TEST(TranslatorSyntax, OverrideKeywordIsRemoved)
{
    EXPECT_EQ(diag_of("class A : B {\n    override f() { }\n}\n"),
              "t.script:2:5: error: 'override' is removed; a function overrides a virtual "
              "method implicitly\n");
}

TEST(TranslatorSyntax, OverrideBeforeFunctionGivesOneError)
{
    EXPECT_EQ(diag_of("class A : B {\n    override function f() { }\n    function g() { }\n}\n"),
              "t.script:2:5: error: 'override' is removed; a function overrides a virtual "
              "method implicitly\n");
}

TEST(TranslatorSyntax, FnIsAnUnknownToken)
{
    EXPECT_NE(diag_of("class A : B {\n    fn f() { }\n}\n")
                  .find("t.script:2:5: error: expected a field, '[' or 'function', found 'fn'"),
              std::string::npos);
}

TEST(TranslatorSyntax, UnknownAttribute)
{
    EXPECT_EQ(diag_of("class A : B {\n    [foo(\"x\")] n : int;\n}\n"),
              "t.script:2:6: error: unknown attribute 'foo'\n");
}

TEST(TranslatorSyntax, AliasNeedsAString)
{
    const std::string d = diag_of("class A : B {\n    [alias(x)] n : int;\n}\n");
    EXPECT_EQ(d.rfind("t.script:2:12: error: expected a string in [alias(...)]", 0), 0u) << d;
}

TEST(TranslatorSyntax, AliasOnAMethod)
{
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"x\")] function f() { }\n}\n"),
              "t.script:2:5: error: [alias] applies to fields only\n");
}

TEST(TranslatorSyntax, OldWasSyntaxHasAHint)
{
    EXPECT_EQ(diag_of("class A : B {\n    @was(\"x\") n : int;\n}\n"),
              "t.script:2:5: error: use [alias(...)] instead of @was(...)\n");
}

TEST(TranslatorSyntax, TwoAttributesInAList)
{
    // The second attribute is checked like the first one.
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), foo(\"b\")] n : int;\n}\n"),
              "t.script:2:18: error: unknown attribute 'foo'\n");
    EXPECT_EQ(diag_of("class A : B {\n    [alias(\"a\"), alias(\"b\")] n : int;\n}\n"),
              "t.script:2:18: error: duplicate attribute 'alias'\n");
}

TEST(TranslatorSyntax, AliasGoesToTheReflectionRow)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r = spc::translate(
        temp_options(dir, "import \"w.h\";\nclass Foo : Widget {\n"
                          "    [alias(\"old\")] n : int = 0;\n    m : int = 0;\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("{\"n\", \"old\","), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("{\"m\", nullptr,"), std::string::npos) << r.cpp;
}

TEST(TranslatorOverride, ImplicitOverrideUsesTheVirtualSlot)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, widget_script("Update(dt: float)")));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // An override gets no entry in the slot file. A plain function gets one.
    EXPECT_EQ(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;
    EXPECT_NE(r.cpp.find("Foo__Update(Widget* a"), std::string::npos) << r.cpp;
}

TEST(TranslatorOverride, SameNameWithOtherParametersHidesTheVirtualMethod)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const spc::TranslateResult r =
        spc::translate(temp_options(dir, widget_script("Update(n: int)")));
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
    const spc::TranslateResult r = spc::translate(temp_options(dir, widget_script("Lock()")));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("is final and cannot be overridden"), std::string::npos)
        << r.diagnostics;
}

TEST(TranslatorOverride, FunctionChecksUseTheWordFunction)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const std::string head = "import \"w.h\";\nclass Foo : Widget {\n";
    spc::TranslateResult r = spc::translate(
        temp_options(dir, head + "    function f() { }\n    function f() { }\n}\n"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("function 'f' is declared twice (overloads are not supported)"),
              std::string::npos)
        << r.diagnostics;
    r = spc::translate(temp_options(dir, head + "    f: int;\n    function f() { }\n}\n"));
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
        "import \"w.h\";\nclass P : Engine.Actor {\n    title: std.string;\n"
        "    history: std.vector<int>;\n    function Tick(dt: float) { }\n}\n";
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
        "import \"w.h\";\nclass P : Engine.Actor {\n    title: std.string;\n"
        "    function Tick(dt: float) { let n = this.title.size(); }\n}\n";
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
    const spc::TranslateResult r = translate_text("class P : B {\n    title: std::string;\n}\n");
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
