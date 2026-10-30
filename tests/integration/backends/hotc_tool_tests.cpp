// Tests of the tool hotc as a child process.
//
// CMake defines these macros: HOT_SOURCE_DIR, HOT_SCRATCH_DIR, HOTC_PATH and HOT_CXX.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/config.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using test_support::expect_errors_at_script;
using test_support::line_with;
using test_support::read_file;
using test_support::TestScratch;

// hotc on a bad script: exit code 1, an error at the script line, no library.
TEST(HotcTool, HotcReportsErrorAtTheScriptLine)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_diag");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/bad.hot";
    const std::string cmd = std::string("'") + HOTC_PATH + "' '" + script + "' --gen 1 --out-dir '" + out_dir + "' -I '"
        + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' --cxx '" + HOT_CXX + "' 2>&1";
    std::string text;
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        text.append(buf, n);
    const int status = pclose(p);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1) << text;
    expect_errors_at_script(text, line_with(read_file(script), "\"fast\""));
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/bad.g1.so"));
}

// The error text speaks script names. --raw-diagnostics gives the text of the compiler.
TEST(HotcTool, RawDiagnosticsKeepsTheCompilerText)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_raw_diag");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/bad.hot";
    const std::string base = std::string("'") + HOTC_PATH + "' '" + script + "' --gen 1 --out-dir '" + out_dir
        + "' -I '" + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' --cxx '" + HOT_CXX + "' ";
    std::string script_names, raw;
    FILE* p = popen((base + "2>&1").c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        script_names.append(buf, n);
    pclose(p);
    p = popen((base + "--raw-diagnostics 2>&1").c_str(), "r");
    ASSERT_NE(p, nullptr);
    while (size_t n = fread(buf, 1, sizeof buf, p))
        raw.append(buf, n);
    pclose(p);
    EXPECT_NE(script_names.find("cannot assign a value of type 'string' to 'float'"), std::string::npos)
        << script_names;
    EXPECT_EQ(script_names.find("st->"), std::string::npos) << script_names;
    EXPECT_NE(raw.find("assigning to 'float' from incompatible type 'const char[5]'"), std::string::npos) << raw;
    EXPECT_NE(raw.find("st->speed"), std::string::npos) << raw;
}

} // namespace

namespace
{

// Runs a shell command. Gives the exit code and the output.
int run_command(const std::string& cmd, std::string& text)
{
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr)
        return -1;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        text.append(buf, n);
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string player_args()
{
    return std::string("'") + HOT_SOURCE_DIR + "/tests/scripts/player_v1.hot' --gen 1 -I '" + HOT_SOURCE_DIR
        + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include'";
}

// hotc --emit-cpp prints the C++ to the standard output, or writes it with -o.
TEST(HotcTool, EmitCppModeTranslatesWithoutCompiling)
{
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp " + player_args() + " 2>&1", text), 0) << text;
    EXPECT_NE(text.find("hot::deref"), std::string::npos);

    TestScratch scratch;
    const std::string out = scratch.file("hotc_emit.cpp");
    std::string text2;
    EXPECT_EQ(
        run_command(std::string("'") + HOTC_PATH + "' --emit-cpp " + player_args() + " -o '" + out + "' 2>&1", text2),
        0)
        << text2;
    EXPECT_NE(read_file(out).find("hot::deref"), std::string::npos); // the #line names differ
}

// A translator error (here a parse error) has the final form of a rewritten compiler error:
// script path, line, real column, the script line, and the count line. --raw-diagnostics keeps
// the plain translator text.
TEST(HotcTool, TranslatorErrorHasTheFinalForm)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_final_form.hot");
    {
        std::ofstream f(script);
        f << "class Foo {\n    void Run() { int x = ; }\n}\n";
    }
    std::string text, raw;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "' --gen 1 2>&1", text), 1);
    EXPECT_EQ(
        run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "' --gen 1 --raw-diagnostics 2>&1", raw),
        1);
    const size_t at = text.find(":2:");
    ASSERT_NE(at, std::string::npos) << text;
    EXPECT_EQ(text.compare(0, script.size(), script), 0) << text;
    EXPECT_EQ(text.compare(at, 3, ":2:"), 0);
    EXPECT_NE(text.find("\n    2 |     void Run() { int x = ; }\n"), std::string::npos) << text;
    EXPECT_NE(text.find("1 error generated."), std::string::npos) << text;
    // The column is the real one, not 1.
    EXPECT_EQ(text.find(":2:1:"), std::string::npos) << text;
    EXPECT_NE(raw.find(":2:"), std::string::npos) << raw;
    EXPECT_EQ(raw.find("|"), std::string::npos) << raw;
    EXPECT_EQ(raw.find("generated."), std::string::npos) << raw;
}

// hotc -o x.so builds one library. A mode flag of the other mode gives a usage error.
TEST(HotcTool, OutputOptionBuildsOneLibrary)
{
    TestScratch scratch;
    const std::string dir = scratch.file("hotc_one_so");
    std::filesystem::create_directories(dir);
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' " + player_args() + " -o '" + dir + "/x.so' --cxx '"
                      + HOT_CXX + "' 2>&1",
                  text),
        0)
        << text;
    EXPECT_TRUE(std::filesystem::exists(dir + "/x.so"));

    std::string text2;
    EXPECT_EQ(
        run_command(
            std::string("'") + HOTC_PATH + "' --emit-cpp " + player_args() + " --out-dir '" + dir + "' 2>&1", text2),
        2);
}

// A field with a borrowed pointer is an error by default. --no-strict-borrowed makes it a warning.
TEST(HotcTool, StrictBorrowedIsTheDefault)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_strict.hot");
    {
        std::FILE* f = std::fopen(script.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("using \"Actor.h\";\nclass P : Actor {\n    Actor* target = null;\n}\n", f);
        std::fclose(f);
    }
    const std::string args =
        std::string("' --gen 1 -I '") + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include'";
    std::string strict, relaxed;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + args + " 2>&1", strict), 1)
        << strict;
    EXPECT_NE(strict.find("a field cannot hold a borrowed pointer"), std::string::npos) << strict;
    EXPECT_EQ(
        run_command(
            std::string("'") + HOTC_PATH + "' --emit-cpp --no-strict-borrowed '" + script + args + " 2>&1", relaxed),
        0)
        << relaxed;
    EXPECT_NE(relaxed.find("warning"), std::string::npos) << relaxed;
}

// String interpolation (design 7.3, DECISIONS item 10).
const char* kInterpScript =
    "using <string>;\n"
    "using \"Actor.h\";\n"
    "class Hero : Actor {\n"
    "    int hp = 7;\n"
    "    float ratio = 0.5;\n"
    "    std.string Label() { return \"L${this.hp}\"; }\n"
    "    override void Tick(float dt) {\n"
    "        this.Name = \"hp = ${this.hp}, name ${this.Label()}, r ${this.ratio:.2f} {x} \\${y} "
    "${this.hp > 3 ? \"hi\" : \"lo\"}\";\n"
    "    }\n"
    "}\n";

std::string write_interp_script(const TestScratch& scratch, const std::string& name)
{
    const std::string script = scratch.file(name);
    std::FILE* f = std::fopen(script.c_str(), "w");
    if (f)
    {
        std::fputs(kInterpScript, f);
        std::fclose(f);
    }
    return script;
}

std::string engine_args()
{
    return std::string(" --gen 1 -I '") + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include'";
}

// Generated code and the runtime headers need C++17 only (DECISIONS). The flags reject each
// C++20 feature and each extension of the compiler.
const char* const kCxx17Flags = " -std=c++17 -fsyntax-only -pedantic -Wc++20-extensions -Werror";

// Translates the script and compiles the C++ with -std=c++17. A script with "${...}" uses
// --format=fmt, because std::format is C++20. Without the fmt headers, such a script is not checked.
void expect_compiles_as_cxx17(const std::string& script)
{
    std::string format, defines;
    if (read_file(script).find("${") != std::string::npos)
    {
#if __has_include(<fmt/format.h>)
        format = " --format=fmt";
        defines = " -DFMT_HEADER_ONLY";
#else
        return;
#endif
    }
    const std::string out = script + ".cxx17.cpp";
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp" + format + " -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    std::string cc;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "'" + kCxx17Flags + defines + " -I '" + HOT_SOURCE_DIR
                      + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << "C++17: " << script << "\n"
        << cc;
}

// A "T&" parameter and a "T&" local of a script class: the generated C++ must compile.
TEST(HotcTool, ScriptClassReferenceCompiles)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_class_ref.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using \"Actor.h\";\nusing <vector>;\n\n"
        "class Weapon : Actor {\n    int dmg = 1;\n}\n\n"
        "class Hero : Actor {\n    Weapon wp;\n    int n = 0;\n\n"
        "    void Make(Weapon& w) { w = new Weapon(); }\n"
        "    int Peek(const Weapon& w) { return w.dmg; }\n"
        "    void Run() {\n"
        "        Make(this.wp);\n"
        "        Weapon& r = this.wp;\n"
        "        var& q = this.n;\n"
        "        q = Peek(r);\n"
        "        foreach (var& x in std.vector<int>()) { x = 1; }\n"
        "        foreach (const var& x in std.vector<int>()) { this.n = x; }\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string out = scratch.file("hotc_class_ref.cpp");
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    std::string cc;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "' -std=c++20 -fsyntax-only -I '" + HOT_SOURCE_DIR
                      + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << cc;
}

// Translates the script text and compiles the C++ with -fsyntax-only.
void expect_script_compiles(TestScratch& scratch, const char* name, const char* text)
{
    const std::string script = scratch.file(name);
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(text, f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string out = scratch.file(std::string(name) + ".cpp");
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    std::string cc;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "' -std=c++20 -fsyntax-only -I '" + HOT_SOURCE_DIR
                      + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << cc;
}

// "var& r = p;" and "foreach (var& e in p)" write through a value parameter: the generated
// function must not take it as "const T&".
TEST(HotcTool, VarReferenceToAValueParameterCompiles)
{
    TestScratch scratch;
    expect_script_compiles(scratch, "hotc_param_ref.hot",
        "using \"Actor.h\";\nusing <vector>;\n\n"
        "class Hero : Actor {\n"
        "    void A(std.vector<int> p) { var& r = p; r.clear(); }\n"
        "    void B(std.vector<int> p) { foreach (var& e in p) { e = 0; } }\n"
        "    int C(std.vector<int> p) { int s = 0; foreach (const var& e in p) { s += e; } return s; }\n"
        "}\n");
}

// A constructor parameter may have the name of a generated local of the construction code.
// Expression bodies, default values and "??" give C++ that compiles. The calls omit the defaults.
TEST(HotcTool, ConveniencesCompile)
{
    TestScratch scratch;
    expect_script_compiles(scratch, "conveniences.hot",
        "struct Vec {\n"
        "    int x;\n"
        "    Vec(int q, int a = 5) { this.x = q + a; }\n"
        "    int Get() => this.x;\n"
        "    void Add(int d = 2) { this.x += d; }\n"
        "}\n"
        "class Bar {\n"
        "    void Run(Foo g) {\n"
        "        g.Heal(); g.Heal(2.0); Twice(); this.Twice(4);\n"
        "        var f = new Foo(); var f2 = new Foo(7); var h = f ?? f2;\n"
        "        var v = new Vec(1); v.Add(); int p = g.Power + v.Get();\n"
        "    }\n"
        "    void Twice(int a = 10) { }\n"
        "}\n"
        "class Foo {\n"
        "    int count;\n"
        "    Foo(int start = 3) { count = start; }\n"
        "    int Get() => count;\n"
        "    int Power => count * 2;\n"
        "    void Heal(float amount = 1.0, int times = 2 * 3) { count += 1; }\n"
        "    void Bump(Foo other = null) => Heal();\n"
        "    Foo Pick(Foo a, Foo b) => a ?? b;\n"
        "}\n");
}

// "x is T name" in an if or a while gives C++ that compiles and runs.
TEST(HotcTool, IsBindingCompilesAndRuns)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_is_bind.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <string>;\nusing \"Actor.h\";\n"
        "class Grunt : Actor {\n    int rage = 2;\n    void Rage() { rage += 1; }\n}\n"
        "class Hero : Actor {\n"
        "    override void Tick(float dt) {\n"
        "        Hero me = new Hero(); Grunt a = new Grunt(); int n = 0; int hits = 0;\n"
        "        if (a is Grunt g && g.rage > 1) { g.Rage(); hits += g.rage; }\n"
        "        if (me is Grunt g2) { hits += 100; } else if (a is Grunt g3) { hits += 10; }\n"
        "        while (a is Grunt g4 && g4.rage < 6) { g4.Rage(); n += 1; if (n == 2) continue; }\n"
        "        this.Name = \"hits ${hits} n ${n}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_is_bind.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "hits 13 n 3");
}

// Private and protected members of a script class: hotc enforces them, the generated C++ keeps
// them public. A script that uses them in the allowed ways builds and runs.
TEST(HotcTool, AccessWordsCompileAndRun)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_access.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <string>;\nusing \"Actor.h\";\n"
        "class Core : Actor {\n"
        "    private int secret = 2;\n"
        "    protected int shared = 3;\n"
        "    private int Twice() { return secret * 2; }\n"
        "    protected int Thrice() { return shared * 3; }\n"
        "    int Level { get; private set; } = 5;\n"
        "    Core() {}\n"
        "    private Core(int n) { secret = n; }\n"
        "    int Total(Core other) { return other.secret + other.Twice() + Twice(); }\n"
        "    int Make() { Core c = new Core(10); return c.secret; }\n"
        "    int Via() { var f = () => { Level = 7; return secret + this.Twice(); }; return f(); }\n"
        "}\n"
        "class Hero : Core {\n"
        "    override void Tick(float dt) {\n"
        "        Core other = new Core();\n"
        "        int via = Via();\n"
        "        this.Name = \"total ${Total(other)} sub ${shared + Thrice()} make ${Make()} via ${via} level "
        "${Level}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_access.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "total 10 sub 12 make 10 via 6 level 7");
}

// A bare member name is "this.name" everywhere: before ".", in a struct method and for a
// member of the native base. The script builds and runs.
TEST(HotcTool, BareMemberNamesCompileAndRun)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_bare_members.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <string>;\nusing <vector>;\nusing \"Actor.h\";\n"
        "struct Pt { int x; int y; }\n"
        "struct Acc {\n"
        "    int hp; Pt p; std.vector<int> history;\n"
        "    int Effective() { return hp * 2 + Extra(); }\n"
        "    int Extra() { return p.x + p.y; }\n"
        "    void Add(int v) { hp += v; history.push_back(v); p.x = v; }\n"
        "}\n"
        "class Unit { int hp = 1; }\n"
        "class Mid : Actor { std.vector<int> seen; Pt where; }\n"
        "class Hero : Mid {\n"
        "    Unit u = new Unit();\n"
        "    Acc acc;\n"
        "    void Fill(int Name) { u.hp = 3; seen.push_back(Name); where.y = u.hp; }\n"
        "    override void Tick(float dt) {\n"
        "        Fill(4);\n"
        "        acc.Add(5);\n"
        "        Move(Vec3(1, 2, 3));\n"
        "        var p = Position;\n"
        "        Position.x = p.x + 1;\n"
        "        Name = \"hp ${u.hp} seen ${seen.size()} eff ${acc.Effective()} hist ${acc.history.size()} "
        "y ${where.y} px ${Position.x} p ${p.x}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_bare_members.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "hp 3 seen 1 eff 15 hist 1 y 3 px 2 p 1");
}

// A structured binding "var (a, b) = e" and "foreach (var (k, v) in c)" give C++ that compiles and
// runs. They work for std.pair, std.tuple, a struct and a std.map. "enumerate" and
// "range" are the helpers of the foreach with an index.
TEST(HotcTool, StructuredBindingsCompileAndRun)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_bindings.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <map>;\nusing <string>;\nusing <tuple>;\nusing <utility>;\nusing <vector>;\nusing \"Actor.h\";\n"
        "struct P { int x; int y; }\n"
        "class Hero : Actor {\n"
        "    std.pair<int, int> pr;\n"
        "    std.pair<int, int> Mk() { return std.make_pair(3, 4); }\n"
        "    override void Tick(float dt) {\n"
        "        var (a, b) = Mk();\n"
        "        var t = std.make_tuple(1, 2, 3);\n"
        "        var (t1, t2, t3) = t;\n"
        "        var p = new P(); p.x = 7; p.y = 8;\n"
        "        var (px, py) = p;\n"
        "        this.pr = std.make_pair(10, 20);\n"
        "        var& (c, d) = this.pr; c += 1; d += 2;\n"
        "        const var (e, g) = Mk();\n"
        "        std.vector<int> xs; xs.push_back(5); xs.push_back(6); xs.push_back(7);\n"
        "        std.map<int, int> m; m[1] = 2; m[3] = 4;\n"
        "        int sum = 0;\n"
        "        foreach (var (k, v) in m) sum += k * v;\n"
        "        foreach (var& (k, v) in m) v += 1;\n"
        "        foreach (const var& (k, v) in m) sum += v;\n"
        "        foreach (var (i, x) in enumerate(xs)) sum += (int)i * x;\n"
        "        foreach (var (i, x) in enumerate(xs)) x += 1;\n"
        "        foreach (int i in range(3)) sum += i;\n"
        "        foreach (int i in range(2, 4)) sum += i;\n"
        "        foreach (int i in range(0)) sum += 1000;\n"
        "        int last = 0;\n"
        "        foreach (var x in enumerate(xs)) last = x.second;\n"
        "        var fn = () => a + b;\n"
        "        last += fn();\n"
        "        this.Name = \"s ${sum} ${a}${b} ${t1}${t2}${t3} ${px}${py} ${this.pr.first} ${this.pr.second} "
        "${e}${g} ${xs[0]}${xs[2]} ${last}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_bindings.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    // sum: 1*2+3*4=14; +3+5=22; enumerate 0*5+1*6+2*7=20 -> 42; range(3)=3 -> 45; range(2,4)=5 -> 50
    EXPECT_EQ(s.actor->Name, "s 50 34 123 78 11 22 34 68 15");
}

TEST(HotcTool, ConstructorParametersMayUseOldGeneratedNames)
{
    TestScratch scratch;
    expect_script_compiles(scratch, "hotc_ctor_names.hot",
        "using \"Actor.h\";\n\n"
        "class Base : Actor {\n    int v = 0;\n    Base(int chain, int up) { this.v = chain + up; }\n}\n\n"
        "class Kid : Base {\n    Kid(int chain, int up, int pp, int plan, int note) : base(chain, up) { this.v += pp + "
        "plan + note; }\n}\n");
}

// "&*h" gives a "T*" and "*h" gives a "T&" for a std::shared_ptr handle and for an intrusive
// handle. "this" is a handle argument for a script handle parameter. "*this" is a "T&" argument for
// a native "Actor&" parameter. The generated C++ must compile.
TEST(HotcTool, HandleAddressOfDereferenceAndThisCompile)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_handle_deref.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <memory>;\nusing \"Actor.h\";\nusing \"Weapon.h\";\nusing \"Object.h\";\n"
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n"
        "[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Object;\n\n"
        "class Hero : Actor {\n"
        "    void TakeW(Weapon* w) { }\n    void RefW(Weapon& w) { }\n"
        "    void TakeI(Item* i) { }\n    void RefI(Item& i) { }\n"
        "    void Hurt(Hero h, float d) { }\n"
        "    void Join(Hero h) { }\n    void Nat(Actor* a) { }\n    Hero buddy;\n    Hero Me() { return this; }\n"
        "    void Run(Weapon w, Item i, Hero other, Actor a) {\n"
        "        TakeW(&*w); RefW(*w); TakeI(&*i); RefI(*i);\n"
        "        Join(this); Nat(this); this.buddy = this; var me = this; Join(me);\n"
        "        other.Hurt(this, 2.0);\n"
        "        a.TakeDamage(*this, 2.0);\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string out = scratch.file("hotc_handle_deref.cpp");
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    std::string cc;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "' -std=c++20 -fsyntax-only -I '" + HOT_SOURCE_DIR
                      + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << cc;
}

// A std.unique_ptr field is an error: a reload copies each field.
TEST(HotcTool, MoveOnlyFieldIsAnError)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_unique_field.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <memory>;\nusing \"Actor.h\";\nusing \"Weapon.h\";\n"
        "class Holder : Actor {\n    std.unique_ptr<Weapon> w;\n}\n",
        f);
    std::fclose(f);
    const std::string out = scratch.file("hotc_unique_field.cpp");
    std::string log;
    EXPECT_NE(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0);
    EXPECT_NE(log.find("field of move-only type 'std::unique_ptr<Weapon>' is not supported"), std::string::npos) << log;
}

// --format=fmt only changes the text. The default is std::format.
TEST(HotcTool, FormatOptionSelectsTheFormatFunction)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_text.hot");
    std::string std_text, fmt_text, bad;
    EXPECT_EQ(
        run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "'" + engine_args() + " 2>&1", std_text),
        0)
        << std_text;
    EXPECT_NE(std_text.find("#include <format>"), std::string::npos) << std_text;
    EXPECT_NE(std_text.find("std::format(\"hp = {}, name {}, r {:.2f} {{x}} ${{y}} {}\""), std::string::npos)
        << std_text;
    EXPECT_EQ(run_command(
                  std::string("'") + HOTC_PATH + "' --emit-cpp --format=fmt '" + script + "'" + engine_args() + " 2>&1",
                  fmt_text),
        0)
        << fmt_text;
    EXPECT_NE(fmt_text.find("#include <fmt/format.h>"), std::string::npos) << fmt_text;
    EXPECT_NE(fmt_text.find("fmt::format(\"hp = {}"), std::string::npos) << fmt_text;
    EXPECT_EQ(fmt_text.find("std::format"), std::string::npos) << fmt_text;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp --format=printf '" + script + "'" + engine_args()
                      + " 2>&1",
                  bad),
        2);
#if __has_include(<fmt/format.h>)
    // fmt is installed: the generated text compiles.
    const std::string out = scratch.file("hotc_interp_fmt.cpp");
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp --format=fmt -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    std::string cc;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "' -std=c++20 -fsyntax-only -DFMT_HEADER_ONLY -I '"
                      + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << cc;
#endif
}

#if __has_include(<fmt/format.h>)
// The text of --format=fmt compiles as C++17. This is the format of a C++17 build.
TEST(HotcTool, FormatFmtOutputCompilesAsCxx17)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_fmt17.hot");
    const std::string out = scratch.file("hotc_interp_fmt17.cpp");
    std::string log, cc;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp --format=fmt -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "'" + kCxx17Flags + " -DFMT_HEADER_ONLY -I '" + HOT_SOURCE_DIR
                      + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << cc;
}
#endif

#if __has_include(<format>)
// The text of --format=std compiles as C++20. This is the format of a C++20 build.
TEST(HotcTool, FormatStdOutputCompilesAsCxx20)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_std20.hot");
    const std::string out = scratch.file("hotc_interp_std20.cpp");
    std::string log, cc;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp --format=std -o '" + out + "' '" + script + "'"
                      + engine_args() + " 2>&1",
                  log),
        0)
        << log;
    EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "' -std=c++20 -fsyntax-only -I '" + HOT_SOURCE_DIR
                      + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' '" + out + "' 2>&1",
                  cc),
        0)
        << cc;
}
#endif

// The build of hotc fixes the format library (include/hot/config.h). In build mode, a
// different --format is an error.
TEST(HotcTool, BuildModeRejectsAFormatThatTheBuildDoesNotUse)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_mismatch.hot");
    const std::string other = HOT_USE_FMT ? "std" : "fmt";
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '"
                      + scratch.file("hotc_interp_mismatch.so") + "' --format=" + other + " 2>&1",
                  log),
        2)
        << log;
    EXPECT_NE(log.find("this hotc is built for --format="), std::string::npos) << log;
}

// "x ?? y" runs y only when x is false.
TEST(HotcTool, CoalesceRunsTheRightSideOnlyWhenNeeded)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_coalesce_run.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <string>;\nusing \"Actor.h\";\n"
        "class Hero : Actor {\n"
        "    int runs = 0;\n"
        "    Hero Make() { runs += 1; return new Hero(); }\n"
        "    override void Tick(float dt) {\n"
        "        Hero a = new Hero(); Hero n = null;\n"
        "        Hero r1 = a ?? Make();\n"
        "        int after_first = runs;\n"
        "        Hero r2 = n ?? Make();\n"
        "        Hero r3 = n ?? null;\n"
        "        this.Name = \"first ${after_first} second ${runs} same ${r1 == a} made ${r2 != null} null ${r3 == "
        "null}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_coalesce_run.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "first 0 second 1 same true made true null true");
}

// "var p = a ?? b;" has the type of the operands: "p.F()" works on a script handle, on a native
// handle and on a native pointer.
TEST(HotcTool, CoalesceInVarKeepsTheOperandType)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_coalesce_var.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <string>;\nusing \"Actor.h\";\nusing \"Weapon.h\";\n"
        "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n"
        "class Pick {\n"
        "    int v = 1;\n"
        "    int F() { return v; }\n"
        "}\n"
        "class Sub : Pick { }\n"
        "class Hero : Actor {\n"
        "    override void Tick(float dt) {\n"
        "        Pick a = new Pick(); Pick n = null; Sub s = new Sub();\n"
        "        var p = a ?? n;\n"
        "        int r1 = p.F();\n"
        "        var q = n ?? a;\n"
        "        int r2 = 0;\n"
        "        q?.F();\n"
        "        var m = s ?? a;\n"
        "        int r3 = m.F();\n"
        "        Weapon w1 = null; Weapon w2 = new Weapon(5, \"axe\");\n"
        "        var w = w1 ?? w2;\n"
        "        int r4 = w.dmg;\n"
        "        Actor* x = this; Actor* y = null;\n"
        "        var z = y ?? x;\n"
        "        z.Name = \"set ${r1} ${r3} ${r4}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_coalesce_var.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "set 1 1 5");
}

#if __has_include(<format>)
// The script is compiled with std::format and run.
TEST(HotcTool, InterpolationRunsWithStdFormat)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_run.hot");
    expect_compiles_as_cxx17(script);
    const std::string so = scratch.file("hotc_interp_run.so");
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so
                      + "' --cxx '" + HOT_CXX + "' 2>&1",
                  log),
        0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "hp = 7, name L7, r 0.50 {x} ${y} hi");
}
#endif

// Statements and expressions that lower to more complex C++ compile with -std=c++17: a string
// switch, collection literals, "?.", "??", "is" with a binding, default arguments, structured
// bindings in lambdas, in "??" and in "finally", enumerate and range.
TEST(HotcTool, LoweredStatementsCompileAsCxx17)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_cxx17.hot");
    std::FILE* f = std::fopen(script.c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs(
        "using <map>;\nusing <string>;\nusing <utility>;\nusing <vector>;\nusing \"Actor.h\";\n"
        "class Unit { int hp = 1; void Hit(int d = 1) { hp -= d; } }\n"
        "class Hero : Actor {\n"
        "    std.vector<int> ids = [1, 2, 3];\n"
        "    std.map<std.string, int> named = [\"a\": 1, \"b\": 2];\n"
        "    Unit Find() { return null; }\n"
        "    int Mode(std.string s) {\n"
        "        switch (s) {\n"
        "            case \"idle\": return 0;\n"
        "            case \"walk\":\n"
        "            case \"run\": return 1;\n"
        "            default: return 2;\n"
        "        }\n"
        "    }\n"
        "    override void Tick(float dt) {\n"
        "        Unit u = Find();\n"
        "        u?.Hit();\n"
        "        u?.hp = 3;\n"
        "        Unit w = u ?? new Unit();\n"
        "        w.Hit(2);\n"
        "        std.vector<int> none = [];\n"
        "        var (a, b) = std.make_pair(1, 2);\n"
        "        var (u1, u2) = std.make_pair(Find(), Find());\n"
        "        Unit picked = u1 ?? u2;\n"
        "        var fn = () => a + b;\n"
        "        int n = fn() + Mode(\"run\");\n"
        "        foreach (var (k, v) in named) { var g = (int z) => z + v + a; n += g(1); }\n"
        "        foreach (var (i, x) in enumerate(ids)) n += (int)i * x;\n"
        "        foreach (int i in range(1, 3)) n += i;\n"
        "        try { n += a; } finally { n += b; }\n"
        "        if (picked is Unit p && p.hp > 0) n += p.hp;\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
}

// Each public header compiles alone with -std=c++17. A script or an embedder can include it.
TEST(HotcTool, PublicHeadersCompileAloneAsCxx17)
{
    TestScratch scratch;
    std::vector<std::string> headers;
    for (const char* dir : {"/include/hot", "/include/hot/runtime"})
        for (const auto& entry : std::filesystem::directory_iterator(std::string(HOT_SOURCE_DIR) + dir))
            if (entry.is_regular_file() && entry.path().extension() == ".h")
                headers.push_back(entry.path().string());
    ASSERT_FALSE(headers.empty());
    for (const std::string& h : headers)
    {
        const std::string tu = scratch.file("hdr_" + std::filesystem::path(h).stem().string() + ".cpp");
        std::FILE* f = std::fopen(tu.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(("#include \"" + h + "\"\n").c_str(), f);
        std::fclose(f);
        std::string cc;
        EXPECT_EQ(run_command(std::string("'") + HOT_CXX + "'" + kCxx17Flags + " -I '" + HOT_SOURCE_DIR + "/include' '"
                          + tu + "' 2>&1",
                      cc),
            0)
            << h << "\n"
            << cc;
    }
}

// --aot writes the C++ of each script and the registration file. It does not compile.
TEST(HotcTool, AotModeWritesTheCppAndTheRegistrationFile)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_aot");
    const std::string scripts = std::string("'") + HOT_SOURCE_DIR + "/tests/scripts/aot_demo.hot' '" + HOT_SOURCE_DIR
        + "/tests/scripts/props_v1.hot'";
    const std::string cmd = std::string("'") + HOTC_PATH + "' --aot " + scripts + " --out-dir '" + out_dir + "' -I '"
        + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    std::string text;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        text.append(buf, n);
    const int status = pclose(p);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << text;
    const std::string demo = read_file(out_dir + "/aot_demo.aot.cpp");
    EXPECT_NE(demo.find("#define script_register hot_script_register_aot_demo"), std::string::npos);
    EXPECT_NE(demo.find("extern \"C\" void script_register("), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/aot_demo.g1.so"));
    const std::string reg = read_file(out_dir + "/hot_aot_registry.cpp");
    EXPECT_NE(reg.find("{\"aot_demo\", &hot_script_register_aot_demo}"), std::string::npos) << reg;
    EXPECT_NE(reg.find("{\"props_v1\", &hot_script_register_props_v1}"), std::string::npos) << reg;
    EXPECT_NE(reg.find("aot_table = {kAotScripts, 2}"), std::string::npos) << reg;
}

// --aot needs an output directory and does not take the options of the other modes.
TEST(HotcTool, AotModeRejectsBadUsage)
{
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/aot_demo.hot";
    for (const char* extra :
        {"", "--out-dir /tmp/x --gen 1", "--out-dir /tmp/x -o /tmp/x.so", "--emit-cpp --out-dir /tmp/x"})
    {
        const std::string cmd = std::string("'") + HOTC_PATH + "' --aot '" + script + "' " + extra + " >/dev/null 2>&1";
        const int status = std::system(cmd.c_str());
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 2) << extra;
    }
}

// A script that does not parse gives exit code 1, an error at the script line and no
// registration file. A type error appears later, in the compile of the library.
TEST(HotcTool, AotModeReportsParseErrorAtTheScriptLine)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_aot_bad");
    std::filesystem::create_directories(out_dir);
    const std::string script = out_dir + "/broken.hot";
    {
        std::ofstream f(script);
        f << "using \"Actor.h\";\n\nclass Broken : Actor {\n    int x = ;\n}\n";
    }
    const std::string cmd = std::string("'") + HOTC_PATH + "' --aot '" + script + "' --out-dir '" + out_dir + "' -I '"
        + HOT_SOURCE_DIR + "/tests/engine' -I '" + HOT_SOURCE_DIR + "/include' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    std::string text;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        text.append(buf, n);
    const int status = pclose(p);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1) << text;
    EXPECT_NE(text.find("broken.hot:4:"), std::string::npos) << text;
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/hot_aot_registry.cpp"));
}

// The text of compile_commands.json is exact, and a quote or a backslash is escaped.
TEST(HotcTool, CompileCommandsJsonText)
{
    hot::CompileCommandEntry a{"/work", "/work/a.cpp", {"clang++", "-I/inc dir", "/work/a.cpp"}, "/work/a.so"};
    hot::CompileCommandEntry b{"/w\"x", "/w\"x/b.cpp", {"c\\d"}, "/w\"x/b.so"};
    EXPECT_EQ(hot::compile_commands_json({}), "[\n]\n");
    EXPECT_EQ(hot::compile_commands_json({a, b}),
        "[\n"
        "  {\n"
        "    \"directory\": \"/work\",\n"
        "    \"file\": \"/work/a.cpp\",\n"
        "    \"arguments\": [\"clang++\", \"-I/inc dir\", \"/work/a.cpp\"],\n"
        "    \"output\": \"/work/a.so\"\n"
        "  },\n"
        "  {\n"
        "    \"directory\": \"/w\\\"x\",\n"
        "    \"file\": \"/w\\\"x/b.cpp\",\n"
        "    \"arguments\": [\"c\\\\d\"],\n"
        "    \"output\": \"/w\\\"x/b.so\"\n"
        "  }\n"
        "]\n");
}

// The compile command is the command of the backend.
TEST(HotcTool, CompileCommandUsesTheBackendFlags)
{
    hot::ClangFilesOptions o;
    o.compiler = "cc";
    o.include_dirs = {"/i1", "/i2"};
    o.extra_flags = {"-O2"};
    o.resource_dir = "/res";
    std::vector<std::string> expected = {"cc", std::string("-std=") + hot::cxx_std_name(), "-fPIC", "-shared", "-g",
        "-Werror=return-type", "-resource-dir=/res", "-O2", "-I/i1", "-I/i2"};
    for (const std::string& f : hot::format_compile_flags())
        expected.push_back(f); // fmt build only
    for (const char* f : {"/x.cpp", "-o", "/x.so"})
        expected.push_back(f);
    EXPECT_EQ(hot::compile_command(o, "/x.cpp", "/x.so"), expected);
}

// The option std_version replaces the standard of the host in the compile command.
TEST(HotcTool, CompileCommandStdVersionOverridesTheHostStandard)
{
    hot::ClangFilesOptions o;
    o.compiler = "cc";
    o.std_version = "c++17";
    EXPECT_EQ(hot::compile_command(o, "/x.cpp", "/x.so")[1], "-std=c++17");
    o.std_version = "c++23";
    EXPECT_EQ(hot::compile_command(o, "/x.cpp", "/x.so")[1], "-std=c++23");
}

// hotc --compile-commands writes one entry for each generated .cpp.
TEST(HotcTool, CompileCommandsOptionWritesTheDatabase)
{
    namespace fs = std::filesystem;
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_cdb");
    const std::string cdb_dir = scratch.file("hotc_cdb_json");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/boss_v1.hot";
    const std::string engine = std::string(HOT_SOURCE_DIR) + "/tests/engine";
    const std::string inc = std::string(HOT_SOURCE_DIR) + "/include";
    const std::string cmd = std::string("'") + HOTC_PATH + "' '" + script + "' --gen 1 --out-dir '" + out_dir + "' -I '"
        + engine + "' -I '" + inc + "' --cxx '" + HOT_CXX + "' --cxxflag -O1 --compile-commands '" + cdb_dir + "' 2>&1";
    std::string text;
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        text.append(buf, n);
    const int status = pclose(p);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0) << text;

    const std::string cpp = fs::absolute(out_dir + "/boss_v1.g1.cpp").string();
    const std::string so = fs::absolute(out_dir + "/boss_v1.g1.so").string();
    const std::string cxx = HOT_CXX;
    std::string fmt_args; // fmt build only
    for (const std::string& f : hot::format_compile_flags())
        fmt_args += "\"" + f + "\", ";
    const std::string expected =
        "[\n  {\n    \"directory\": \"" + fs::current_path().string() + "\",\n    \"file\": \"" + cpp +
        "\",\n    \"arguments\": [\"" + cxx + "\", \"-std=" + std::string(hot::cxx_std_name()) + "\", \"-fPIC\", \"-shared\", \"-g\", "
        "\"-Werror=return-type\", \"-O1\", \"-I" + engine + "\", \"-I" + inc + "\", " + fmt_args + "\"" + cpp +
        "\", \"-o\", \"" + so + "\"],\n    \"output\": \"" + so + "\"\n  }\n]\n";
    EXPECT_EQ(read_file(cdb_dir + "/compile_commands.json"), expected);
}

// hotc --emit-cpp translates a plain C++ script (a ".cpp" file) to a generation file.
TEST(HotcTool, EmitCppTranslatesACppScript)
{
    std::string text;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/cpp_player_v1.cpp";
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "'" + engine_args() + " 2>&1", text),
        0)
        << text;
    EXPECT_NE(text.find("script_register"), std::string::npos) << text;
}

// The fixture v2 uses HOT_ALIAS and onReload and has no include of "hot/script.h".
// hotc injects the header itself, so the translation works.
TEST(HotcTool, EmitCppInjectsTheScriptHeaderForTheAids)
{
    std::string text;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/cpp_player_v2.cpp";
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "'" + engine_args() + " 2>&1", text),
        0)
        << text;
    EXPECT_NE(text.find("script_register"), std::string::npos) << text;
}

// A pure virtual method is not supported in a ".cpp" script. hotc fails.
TEST(HotcTool, CppScriptWithUnsupportedFormFails)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_cpp_virtual.cpp");
    {
        std::ofstream f(script);
        f << "#include \"Actor.h\"\nclass Foo : public Actor\n{\npublic:\n    virtual void Run() = 0;\n};\n";
    }
    std::string text;
    EXPECT_NE(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "'" + engine_args() + " 2>&1", text),
        0);
    EXPECT_NE(text.find("error"), std::string::npos) << text;
    EXPECT_NE(text.find("virtual"), std::string::npos) << text;
}

// A syntax error in a ".cpp" script makes hotc fail with an error at the script line.
TEST(HotcTool, CppScriptWithSyntaxErrorFails)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_cpp_syntax.cpp");
    {
        std::ofstream f(script);
        f << "#include \"Actor.h\"\nclass Foo : public Actor\n{\npublic:\n    void Tick(float dt) override { int x = ; }\n};\n";
    }
    std::string text;
    EXPECT_NE(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "'" + engine_args() + " 2>&1", text),
        0);
    EXPECT_NE(text.find("hotc_cpp_syntax.cpp:5:"), std::string::npos) << text;
    EXPECT_NE(text.find("error"), std::string::npos) << text;
}

// The tool never writes over an input file. "-o a.so" writes "a.cpp" next to the library.
TEST(HotcTool, OutputNeverOverwritesAnInput)
{
    TestScratch scratch;
    const std::string dir = scratch.file("hotc_no_overwrite");
    std::filesystem::create_directories(dir);
    const std::string script = dir + "/a.cpp";
    const std::string source = "#include \"Actor.h\"\nclass Foo : public Actor\n{\npublic:\n    int x = 1;\n};\n";
    {
        std::ofstream f(script);
        f << source;
    }
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' '" + script + "' --gen 1 -o '" + dir + "/a.so' --cxx '"
                      + HOT_CXX + "'" + engine_args() + " 2>&1",
                  text),
        1)
        << text;
    EXPECT_NE(text.find("the output file"), std::string::npos) << text;
    EXPECT_EQ(read_file(script), source);
    EXPECT_FALSE(std::filesystem::exists(dir + "/a.so"));

    // The same file by another path (a relative path with "..") is found too.
    std::string text2;
    EXPECT_EQ(run_command(std::string("'") + HOTC_PATH + "' --emit-cpp '" + script + "' --gen 1 -o '" + dir
                      + "/../hotc_no_overwrite/a.cpp'" + engine_args() + " 2>&1",
                  text2),
        1)
        << text2;
    EXPECT_NE(text2.find("the output file"), std::string::npos) << text2;
    EXPECT_EQ(read_file(script), source);
}

} // namespace
