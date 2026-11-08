// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the tool hotc as a child process.
//
// CMake defines these macros: HOT_SOURCE_DIR, HOT_SCRATCH_DIR, HOTC_PATH and HOT_CXX (the script
// compiler). The tests run each program from an argument list, without a shell.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/command_line.h"
#include "hot/config.h"
#include "hot/module.h"
#include "hot/paths.h"
#include "support/process.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{

using test_support::expect_errors_at_script;
using test_support::line_with;
using test_support::read_file;
using test_support::TestScratch;

// The arguments of a program.
using Args = std::vector<std::string>;

const std::string kSource = HOT_SOURCE_DIR;

// The lists of arguments, one after the other, as one list.
Args cat(std::initializer_list<Args> parts)
{
    Args all;
    for (const Args& part : parts)
        all.insert(all.end(), part.begin(), part.end());
    return all;
}

// The flag of the C runtime library of the module on an MSVC host. The backend adds it (see
// clang_files_backend.cpp). A program that links hot_runtime_core (/MD) needs the same flag,
// or lld-link reports a mismatch of 'RuntimeLibrary'. The list is empty with MinGW and on Linux.
Args ms_runtime_flags()
{
#if defined(_MSC_VER)
    #ifdef _DEBUG
    return {"-fms-runtime-lib=dll_dbg"};
    #else
    return {"-fms-runtime-lib=dll"};
    #endif
#else
    return {};
#endif
}

// Runs a program with the arguments (no shell). Gives the exit code (-1: the program did not start
// or a signal killed it). The text is the standard output and the standard error, with "\n" line ends.
int run_command(const Args& argv, std::string& text)
{
    const test_support::RunResult result = test_support::run_process(argv);
    text = test_support::lf_only(result.output);
    return result.exit_code;
}

// The target of the build. They are empty on Linux. A MinGW build has the triple and the sysroot.
// CMake gives them to hotc in the same way (--target, --sysroot).
const hot::DefaultPaths& build_paths()
{
    static const hot::DefaultPaths paths = hot::default_paths();
    return paths;
}

// Runs hotc with the target of the build (and the import library of the host, see below).
int run_hotc(const Args& args, std::string& text)
{
    Args argv = {HOTC_PATH};
    if (!build_paths().target_triple.empty())
        argv.insert(argv.end(), {"--target", build_paths().target_triple});
    if (!build_paths().sysroot.empty())
        argv.insert(argv.end(), {"--sysroot", build_paths().sysroot});
    // Windows: a call with --cxx builds a module DLL. The DLL links the import library of the host
    // (this test program) to reach the runtime symbols. It is empty on other systems.
    if (!build_paths().host_import_library.empty() && std::find(args.begin(), args.end(), "--cxx") != args.end())
        argv.insert(argv.end(), {"--host-implib", build_paths().host_import_library});
    argv.insert(argv.end(), args.begin(), args.end());
    return run_command(argv, text);
}

// Runs the script compiler with the target flags of the build: the triple, the sysroot and, for
// MinGW, the libstdc++ headers of the sysroot (as the backend). The link flags of MinGW do not
// apply to a compile without a link, so the compiler must not report them.
int run_cxx(const Args& args, std::string& text)
{
    Args argv = {HOT_CXX};
    const hot::DefaultPaths& paths = build_paths();
    if (!paths.target_triple.empty())
        argv.push_back("--target=" + paths.target_triple);
    if (!paths.sysroot.empty())
        argv.push_back("--sysroot=" + paths.sysroot);
    const Args mingw = hot::mingw_target_flags(paths.target_triple, paths.sysroot);
    argv.insert(argv.end(), mingw.begin(), mingw.end());
    if (!mingw.empty())
        argv.push_back("-Wno-unused-command-line-argument");
    argv.insert(argv.end(), args.begin(), args.end());
    return run_command(argv, text);
}

// The name of a module that hotc builds: the platform suffix (.so or .dll).
std::string module_file(const std::string& dir, const std::string& stem)
{
    return dir + "/" + stem + hot::module_suffix();
}

// The include directories of the engine headers and of the Hot++ headers.
Args include_args()
{
    return {"-I", kSource + "/tests/engine", "-I", kSource + "/include"};
}

// The generation and the include directories of hotc.
Args engine_args()
{
    return cat({{"--gen", "1"}, include_args()});
}

// The flags that reject each C++20 feature and each extension of the compiler. Generated code and
// the runtime headers need C++17 only (DECISIONS).
const Args kCxx17Flags = {"-std=c++17", "-fsyntax-only", "-pedantic", "-Wc++20-extensions", "-Werror"};

// hotc on a bad script: exit code 1, an error at the script line, no library.
TEST(HotcTool, HotcReportsErrorAtTheScriptLine)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_diag");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/bad.hot";
    std::string text;
    EXPECT_EQ(
        run_hotc(cat({{script, "--gen", "1", "--out-dir", out_dir}, include_args(), {"--cxx", HOT_CXX}}), text), 1)
        << text;
    expect_errors_at_script(text, line_with(read_file(script), "\"fast\""));
    EXPECT_FALSE(std::filesystem::exists(module_file(out_dir, "bad.g1")));
}

// The error text speaks script names. --raw-diagnostics gives the text of the compiler.
TEST(HotcTool, RawDiagnosticsKeepsTheCompilerText)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_raw_diag");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/bad.hot";
    const Args base = cat({{script, "--gen", "1", "--out-dir", out_dir}, include_args(), {"--cxx", HOT_CXX}});
    std::string script_names, raw;
    run_hotc(base, script_names);
    run_hotc(cat({base, {"--raw-diagnostics"}}), raw);
    EXPECT_NE(script_names.find("cannot assign a value of type 'string' to 'float'"), std::string::npos)
        << script_names;
    EXPECT_EQ(script_names.find("st->"), std::string::npos) << script_names;
    EXPECT_NE(raw.find("assigning to 'float' from incompatible type 'const char[5]'"), std::string::npos) << raw;
    EXPECT_NE(raw.find("st->speed"), std::string::npos) << raw;
}

} // namespace

namespace
{

Args player_args()
{
    return cat({{kSource + "/tests/scripts/player_v1.hot"}, engine_args()});
}

// hotc --emit-cpp prints the C++ to the standard output, or writes it with -o.
TEST(HotcTool, EmitCppModeTranslatesWithoutCompiling)
{
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp"}, player_args()}), text), 0) << text;
    EXPECT_NE(text.find("hot::deref"), std::string::npos);

    TestScratch scratch;
    const std::string out = scratch.file("hotc_emit.cpp");
    std::string text2;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp"}, player_args(), {"-o", out}}), text2), 0) << text2;
    EXPECT_NE(read_file(out).find("hot::deref"), std::string::npos); // the #line names differ
}

// A warning in a .cpp script (here an error by -Werror) shows the script file, the line, the real
// column and the script text. No generated name is visible. --raw-diagnostics keeps the compiler text.
TEST(HotcTool, CppScriptWarningSpeaksScriptText)
{
    {
        // The exact text of the warnings is the text of Clang.
        std::string version;
        run_cxx({"--version"}, version);
        if (version.find("clang") == std::string::npos)
            GTEST_SKIP() << "the compiler is not Clang";
    }
    TestScratch scratch;
    const std::string script = scratch.file("warn.cpp");
    std::filesystem::copy_file(std::string(HOT_SOURCE_DIR) + "/tests/scripts/cpp_diag_warn.cpp", script);
    const Args base = cat({{"--gen", "1", "--out-dir", scratch.file("hotc_cpp_warn")}, include_args(),
        {"--cxx", HOT_CXX, "--cxxflag", "-Wall", "--cxxflag", "-Werror", script}});
    std::string text, raw;
    EXPECT_NE(run_hotc(base, text), 0) << text;
    EXPECT_NE(run_hotc(cat({base, {"--raw-diagnostics"}}), raw), 0) << raw;
    for (const std::string* t : {&text, &raw})
    {
        EXPECT_NE(
            t->find("warn.cpp:10:13: error: unused variable 'unused' [-Werror,-Wunused-variable]"), std::string::npos)
            << *t;
        EXPECT_NE(t->find("warn.cpp:11:16: error: "), std::string::npos) << *t;
    }
    EXPECT_NE(text.find("   10 |         int unused = 1;\n      |             ^~~~~~\n"), std::string::npos) << text;
    EXPECT_NE(text.find("   11 |         if (hp = dt)\n"), std::string::npos) << text;
    EXPECT_EQ(text.find("warn.cpp:10:1:"), std::string::npos) << text; // the column is not 1
    EXPECT_EQ(text.find("warn.cpp:11:1:"), std::string::npos) << text;
    EXPECT_NE(text.find("[-Werror,-Wparentheses]"), std::string::npos) << text;
    for (const char* generated : {".g1.cpp", "_Shell", "State_g", "__self", "state_of"})
        EXPECT_EQ(text.find(generated), std::string::npos) << generated << "\n" << text;
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
    EXPECT_EQ(run_hotc({"--emit-cpp", script, "--gen", "1"}, text), 1);
    EXPECT_EQ(run_hotc({"--emit-cpp", script, "--gen", "1", "--raw-diagnostics"}, raw), 1);
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
    EXPECT_EQ(run_hotc(cat({player_args(), {"-o", module_file(dir, "x"), "--cxx", HOT_CXX}}), text), 0) << text;
    EXPECT_TRUE(std::filesystem::exists(module_file(dir, "x")));

    std::string text2;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp"}, player_args(), {"--out-dir", dir}}), text2), 2);
}

// A field with a borrowed pointer is an error by default. --no-strict-borrowed makes it a warning.
TEST(HotcTool, StrictBorrowedIsTheDefault)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_strict.hot");
    {
        std::FILE* f = std::fopen(script.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("using \"Actor.h\";\nclass P : Actor {\n    Actor** target = null;\n    ~P() { target = null; }\n}\n", f);
        std::fclose(f);
    }
    std::string strict, relaxed;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", script}, engine_args()}), strict), 1) << strict;
    EXPECT_NE(strict.find("a field cannot hold a borrowed pointer"), std::string::npos) << strict;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--no-strict-borrowed", script}, engine_args()}), relaxed), 0) << relaxed;
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

// Translates the script and compiles the C++ with -std=c++17. A script with "${...}" uses
// --format=fmt, because std::format is C++20. Without the fmt headers, such a script is not checked.
void expect_compiles_as_cxx17(const std::string& script)
{
    Args format, defines;
    if (read_file(script).find("${") != std::string::npos)
    {
#if __has_include(<fmt/format.h>)
        format = {"--format=fmt"};
        defines = {"-DFMT_HEADER_ONLY"};
#else
        return;
#endif
    }
    const std::string out = script + ".cxx17.cpp";
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp"}, format, {"-o", out, script}, engine_args()}), log), 0) << log;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({kCxx17Flags, defines, include_args(), {out}}), cc), 0) << "C++17: " << script << "\n" << cc;
}

// An "async" method needs C++20. The generated C++ of the async scripts compiles clean as C++20,
// also as optimized AOT code.
TEST(HotcTool, AsyncScriptsCompileAsCxx20)
{
    TestScratch scratch;
    for (const char* name : {"async_basic", "async_virtual"})
    {
        const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/" + name + ".hot";
        const std::string out = scratch.file(std::string(name) + ".cpp");
        std::string log;
        EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--std=c++20", "-o", out, script}, engine_args()}), log), 0) << log;
        const Args flags = {"-std=c++20", "-fsyntax-only", "-Werror"};
        std::string cc;
        EXPECT_EQ(run_cxx(cat({flags, include_args(), {out}}), cc), 0) << name << "\n" << cc;

        const std::string aot_dir = scratch.file(std::string(name) + "_aot");
        EXPECT_EQ(
            run_hotc(
                cat({{"--aot", "--aot-optimized", "--std=c++20", script, "--out-dir", aot_dir}, include_args()}), log),
            0)
            << log;
        EXPECT_EQ(run_cxx(cat({flags, include_args(), {aot_dir + "/" + name + ".aot.cpp"}}), cc), 0)
            << name << " (optimized AOT)\n"
            << cc;
    }
}

// The same script as C++17 is an error: "async" needs C++20.
TEST(HotcTool, AsyncScriptNeedsCxx20)
{
    TestScratch scratch;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/async_basic.hot";
    std::string log;
    EXPECT_NE(
        run_hotc(cat({{"--emit-cpp", "--std=c++17", "-o", scratch.file("a17.cpp"), script}, engine_args()}), log), 0);
    EXPECT_NE(log.find("'async' needs C++20"), std::string::npos) << log;
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
        "class Hero : Actor {\n    $Weapon wp;\n    int n = 0;\n\n"
        "    void Make(ref $Weapon w) { w = new $Weapon(); }\n"
        "    int Peek(in $Weapon w) { if (w == null) return 0; return w.dmg; }\n"
        "    void Run() {\n"
        "        Make(this.wp);\n"
        "        $Weapon l = this.wp;\n"
        "        ref $Weapon r = l;\n"
        "        ref q = this.n;\n"
        "        q = Peek(r);\n"
        "        foreach (ref x in std.vector<int>()) { x = 1; }\n"
        "        foreach (ref x in std.vector<int>()) { this.n = x; }\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string out = scratch.file("hotc_class_ref.cpp");
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0) << log;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++20", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
}

// Classes that use each other through handles (task 47): the golden script builds a library, and
// the C++ compiles as C++17. A body uses the members of a class that comes later in the order.
TEST(HotcTool, ClassesThatUseEachOtherBuildALibrary)
{
    TestScratch scratch;
    const std::string dir = scratch.file("hotc_cycle_handles");
    std::filesystem::create_directories(dir);
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/cycle_handles_v1.hot";
    std::string text;
    EXPECT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", module_file(dir, "cycle"), "--cxx", HOT_CXX}}), text), 0)
        << text;
    EXPECT_TRUE(std::filesystem::exists(module_file(dir, "cycle")));
    const std::string copy = scratch.file("hotc_cycle_handles.hot");
    std::ofstream(copy) << read_file(script);
    expect_compiles_as_cxx17(copy);
}

// The module attribute std in a prelude: the generated C++ names the standard library "mystd".
// The header StdAlias.h has "namespace mystd = std;", so the C++ compiles as C++17.
TEST(HotcTool, ModuleAttributeStdCompiles)
{
    TestScratch scratch;
    const std::string prelude = scratch.file("hotc_module_attrs.prelude");
    const std::string script = scratch.file("hotc_module_attrs.hot");
    const std::string out = scratch.file("hotc_module_attrs.cpp");
    {
        std::ofstream(prelude) << "[std(\"mystd\")];\n";
        std::ofstream(script) << "using \"Actor.h\";\nusing \"StdAlias.h\";\nusing <vector>;\n\n"
                                 "class Counter : Actor {\n    std.string label = \"c\";\n"
                                 "    std.vector<int> history = std.vector<int>();\n"
                                 "    override void Tick(float dt) {\n"
                                 "        this.history.push_back(1);\n"
                                 "        this.label = std.to_string(this.history.size());\n    }\n}\n";
    }
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--prelude", prelude, "-o", out, script}, engine_args()}), log), 0) << log;
    const std::string cpp = read_file(out);
    EXPECT_NE(cpp.find("mystd::string"), std::string::npos);
    EXPECT_EQ(cpp.find("\x01std::"), std::string::npos);
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++17", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
}

// The module attribute array in a prelude: the generated C++ names the host array template. The header
// ArrayAlias.h has the alias template, so the C++ compiles as C++17.
TEST(HotcTool, ModuleAttributeArrayCompiles)
{
    TestScratch scratch;
    const std::string prelude = scratch.file("hotc_module_array.prelude");
    const std::string script = scratch.file("hotc_module_array.hot");
    const std::string out = scratch.file("hotc_module_array.cpp");
    {
        std::ofstream(prelude) << "[array(\"hostns::FixedArray<$T, $N>\")];\n";
        std::ofstream(script) << "using \"Actor.h\";\nusing \"ArrayAlias.h\";\n\n"
                                 "class Grid : Actor {\n    int[4] cells;\n    float[2][3] weights;\n"
                                 "    override void Tick(float dt) {\n"
                                 "        this.cells[1] = 7;\n"
                                 "        int[2] local;\n"
                                 "        this.weights[1][2] = dt;\n    }\n}\n";
    }
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--prelude", prelude, "-o", out, script}, engine_args()}), log), 0) << log;
    const std::string cpp = read_file(out);
    EXPECT_NE(cpp.find("hostns::FixedArray<int32_t, 4> cells"), std::string::npos);
    EXPECT_EQ(cpp.find("std::array"), std::string::npos);
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++17", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
}

// The stubs with checked_index and the custom array template: the generated C++ has the
// specializations of hot::checked_index and compiles as C++17. The stub of a type that the
// headers do not declare gives none.
TEST(HotcTool, CheckedIndexStubsCompile)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_checked_index.hot");
    const std::string out = scratch.file("hotc_checked_index.cpp");
    {
        std::ofstream(script) << "using \"Actor.h\";\nusing \"CheckedTypes.h\";\nusing <deque>;\n\n"
                                 "[array(\"hostns2::Fixed<$T, $N>\")];\n"
                                 "[native, checked_index] class hostns2.Ring<*>;\n"
                                 "[native, checked_index] class hostns2.Plain;\n"
                                 "[native, checked_index] class hostns2.Missing<*>;\n"
                                 "[native, checked_index] class std.deque<*>;\n"
                                 "[native, checked_index] class hostns2.__detail.Deep<*>;\n"
                                 "[native, checked_index] class hostns2.Versioned<*>;\n\n"
                                 "class Grid : Actor {\n    int[4] cells;\n    hostns2.Ring<int> ring;\n"
                                 "    hostns2.Plain plain;\n"
                                 "    override void Tick(float dt) {\n"
                                 "        this.cells[1] = 7;\n        this.ring[2] = 3;\n"
                                 "        this.plain[0] = 1;\n    }\n}\n";
    }
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0) << log;
    const std::string cpp = read_file(out);
    EXPECT_NE(cpp.find("struct checked_index< ::hostns2::Ring<A...>>"), std::string::npos) << cpp;
    EXPECT_NE(cpp.find("struct checked_index< ::hostns2::Plain>"), std::string::npos) << cpp;
    EXPECT_NE(cpp.find("checked_index< ::hostns2::Fixed<T, N>>"), std::string::npos) << cpp;
    EXPECT_EQ(cpp.find("Missing"), std::string::npos) << cpp;
    EXPECT_NE(cpp.find("checked_index< ::std::deque<A...>>"), std::string::npos) << cpp;
    EXPECT_NE(cpp.find("checked_index< ::hostns2::__detail::Deep<A...>>"), std::string::npos) << cpp;
    EXPECT_NE(cpp.find("checked_index< ::hostns2::Versioned<A...>>"), std::string::npos) << cpp;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++17", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
}

// The module attribute array with the alias template FixedArray of ArrayAlias.h: the partial
// specialization through the alias compiles as C++17.
TEST(HotcTool, ArrayAliasCheckedIndexCompiles)
{
    TestScratch scratch;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/module_attrs_array_v1.hot";
    const std::string out = scratch.file("hotc_array_alias.cpp");
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0) << log;
    const std::string cpp = read_file(out);
    EXPECT_NE(cpp.find("checked_index< ::hostns::FixedArray<T, N>>"), std::string::npos) << cpp;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++17", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
}

// A stub with checked_index must fit the kind of its name. The message gives the fix.
void expect_checked_index_error(const char* name, const char* stub, const char* message)
{
    TestScratch scratch;
    const std::string script = scratch.file(std::string("hotc_ci_err_") + name + ".hot");
    const std::string out = scratch.file(std::string("hotc_ci_err_") + name + ".cpp");
    {
        std::ofstream(script) << "using \"Actor.h\";\nusing \"CheckedTypes.h\";\nusing \"ArrayAlias.h\";\n\n"
                              << stub << "\n\nclass Grid : Actor {\n}\n";
    }
    std::string log;
    EXPECT_NE(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0) << log;
    EXPECT_NE(log.find(message), std::string::npos) << log;
    EXPECT_NE(log.find(":5:"), std::string::npos) << log; // the line of the stub
}

TEST(HotcTool, CheckedIndexOnAClassTemplateNeedsThePattern)
{
    expect_checked_index_error("exact_tpl", "[native, checked_index] class hostns2.Ring;",
        "'checked_index' on 'hostns2::Ring': it is a class template; write 'hostns2::Ring<*>'");
}

TEST(HotcTool, CheckedIndexPatternOnAPlainClassIsAnError)
{
    expect_checked_index_error("pattern_plain", "[native, checked_index] class hostns2.Plain<*>;",
        "'hostns2::Plain' is not a class template; write it as 'hostns2::Plain'");
}

TEST(HotcTool, CheckedIndexPatternOnAnAliasTemplateIsAnError)
{
    expect_checked_index_error("pattern_alias", "[native, checked_index] class hostns.FixedArray<*>;",
        "is an alias template or has a non-type parameter. For an array type use the module attribute 'array'");
}

TEST(HotcTool, CheckedIndexPatternOnATemplateWithASizeParameterIsAnError)
{
    expect_checked_index_error("pattern_nttp", "[native, checked_index] class hostns2.Fixed<*>;",
        "is an alias template or has a non-type parameter. For an array type use the module attribute 'array'");
}

TEST(HotcTool, CheckedIndexExactNameOnATemplateWithASizeParameterIsAnError)
{
    expect_checked_index_error("exact_nttp", "[native, checked_index] class hostns2.Fixed;",
        "'checked_index' on 'hostns2::Fixed': it is an alias template or has a non-type parameter");
}

// With HOT_RUNTIME_CHECKS=0 hot::index of a checked container has no test. The program defines
// the throw function to abort, so a test that stays makes the run fail.
TEST(HotcTool, CheckedIndexHasNoTestWithoutRuntimeChecks)
{
    TestScratch scratch;
    const std::string src = scratch.file("hotc_unchecked_index.cpp");
#ifdef _WIN32
    const std::string exe = scratch.file("hotc_unchecked_index.exe");
#else
    const std::string exe = scratch.file("hotc_unchecked_index.bin");
#endif
    {
        std::ofstream(src) << "#include \"hot/runtime/deref.h\"\n#include <cstdlib>\n#include <vector>\n"
                              "namespace hot::detail {\n"
                              "void throw_index_out_of_range(std::size_t, std::size_t) { std::abort(); }\n"
                              "void throw_null_reference(const char*) { std::abort(); }\n}\n"
                              "struct Lax { int v = 3; std::size_t size() const { return 1; }\n"
                              "  int& operator[](std::size_t) { return v; } };\n"
                              "namespace hot { template <> struct checked_index<Lax> : std::true_type {}; }\n"
                              "int main() { Lax l; std::vector<int> g(1); return hot::index(l, 5) == 3 ? 0 : 1; }\n";
    }
    std::string log;
    Args compile = {"-std=c++17", "-DHOT_RUNTIME_CHECKS=0", "-I", kSource + "/include", src, "-o", exe};
#ifdef _WIN32
    // The exceptions of the runtime are in the library. The same library object also defines the two
    // throw functions of this program. The first definition (the program) wins.
    compile.push_back(HOT_RUNTIME_LIB);
    #ifdef __MINGW32__
    compile.push_back("-Wl,--allow-multiple-definition");
    #else
    // MSVC target: lld-link does not know the GNU ld option.
    compile.push_back("-Wl,/force:multiple");
    for (const std::string& f : ms_runtime_flags())
        compile.push_back(f);
    #endif
#endif
    ASSERT_EQ(run_cxx(compile, log), 0) << log;
    EXPECT_EQ(run_command({exe}, log), 0) << log;
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
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0) << log;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++20", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
}

// "ref r = p;" and "foreach (ref e in p)" write through a value parameter: the generated
// function must not take it as "const T&".
TEST(HotcTool, VarReferenceToAValueParameterCompiles)
{
    TestScratch scratch;
    expect_script_compiles(scratch, "hotc_param_ref.hot",
        "using \"Actor.h\";\nusing <vector>;\n\n"
        "class Hero : Actor {\n"
        "    void A(std.vector<int> p) { ref r = p; r.clear(); }\n"
        "    void B(std.vector<int> p) { foreach (ref e in p) { e = 0; } }\n"
        "    int C(std.vector<int> p) { int s = 0; foreach (ref e in p) { s += e; } return s; }\n"
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
        "    void Run($Foo g) {\n"
        "        if (g == null) return;\n"
        "        g.Heal(); g.Heal(2.0); Twice(); this.Twice(4);\n"
        "        var f = new $Foo(); var f2 = new $Foo(7); var h = f ?? f2;\n"
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
        "    void Bump($Foo other = null) => Heal();\n"
        "    $Foo Pick($Foo a, $Foo b) => a ?? b;\n"
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
        "        $Hero me = new $Hero(); $Grunt a = new $Grunt(); int n = 0; int hits = 0;\n"
        "        if (a is $Grunt g && g.rage > 1) { g.Rage(); hits += g.rage; }\n"
        "        if (me is $Grunt g2) { hits += 100; } else if (a is $Grunt g3) { hits += 10; }\n"
        "        while (a is $Grunt g4 && g4.rage < 6) { g4.Rage(); n += 1; if (n == 2) continue; }\n"
        "        this.Name = \"hits ${hits} n ${n}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = module_file(scratch.path(), "hotc_is_bind");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
        "    int Total($Core other) {\n"
        "        if (other == null) return 0;\n"
        "        return other.secret + other.Twice() + Twice();\n"
        "    }\n"
        "    int Make() { $Core c = new $Core(10); return c.secret; }\n"
        "    int Via() { var f = () => { Level = 7; return secret + this.Twice(); }; return f(); }\n"
        "}\n"
        "class Hero : Core {\n"
        "    override void Tick(float dt) {\n"
        "        $Core other = new $Core();\n"
        "        int via = Via();\n"
        "        this.Name = \"total ${Total(other)} sub ${shared + Thrice()} make ${Make()} via ${via} level "
        "${Level}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = module_file(scratch.path(), "hotc_access");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
        "    $Unit u = new $Unit();\n"
        "    Acc acc;\n"
        "    void Fill(int Name) {\n"
        "        $Unit k = u;\n"
        "        if (k == null) return;\n"
        "        k.hp = 3; seen.push_back(Name); where.y = k.hp;\n"
        "    }\n"
        "    override void Tick(float dt) {\n"
        "        Fill(4);\n"
        "        $Unit k = u;\n"
        "        if (k == null) return;\n"
        "        acc.Add(5);\n"
        "        Move(Vec3(1, 2, 3));\n"
        "        var p = Position;\n"
        "        Position.x = p.x + 1;\n"
        "        Name = \"hp ${k.hp} seen ${seen.size()} eff ${acc.Effective()} hist ${acc.history.size()} "
        "y ${where.y} px ${Position.x} p ${p.x}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = module_file(scratch.path(), "hotc_bare_members");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
        "        ref (c, d) = this.pr; c += 1; d += 2;\n"
        "        var (e, g) = Mk();\n"
        "        std.vector<int> xs; xs.push_back(5); xs.push_back(6); xs.push_back(7);\n"
        "        std.map<int, int> m; m[1] = 2; m[3] = 4;\n"
        "        int sum = 0;\n"
        "        foreach (var (k, v) in m) sum += k * v;\n"
        "        foreach (ref (k, v) in m) v += 1;\n"
        "        foreach (ref (k, v) in m) sum += v;\n"
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
    const std::string so = module_file(scratch.path(), "hotc_bindings");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
        "    void TakeW(Weapon* w) { }\n    void RefW(ref Weapon w) { }\n"
        "    void TakeI(Item* i) { }\n    void RefI(ref Item i) { }\n"
        "    void Hurt($Hero h, float d) { }\n"
        "    void Join($Hero h) { }\n    void Nat(Actor* a) { }\n    $Hero buddy;\n    $Hero Me() { return this; }\n"
        "    void Run(Weapon w, Item i, $Hero other, Actor a) {\n"
        "        TakeW((Weapon*)w); RefW(*w); TakeI((Item*)i); RefI(*i);\n"
        "        Join(this); Nat(this); this.buddy = this; var me = this; Join(me);\n"
        "        if (other == null) return;\n"
        "        other.Hurt(this, 2.0);\n"
        "        a.TakeDamage(*this, 2.0);\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string out = scratch.file("hotc_handle_deref.cpp");
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0) << log;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++20", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
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
    EXPECT_NE(run_hotc(cat({{"--emit-cpp", "-o", out, script}, engine_args()}), log), 0);
    EXPECT_NE(log.find("field of move-only type 'std::unique_ptr<Weapon>' is not supported"), std::string::npos) << log;
}

// --format=fmt only changes the text. The default is std::format.
TEST(HotcTool, FormatOptionSelectsTheFormatFunction)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_text.hot");
    std::string std_text, fmt_text, bad;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", script}, engine_args()}), std_text), 0) << std_text;
    EXPECT_NE(std_text.find("#include <format>"), std::string::npos) << std_text;
    EXPECT_NE(std_text.find("std::format(\"hp = {}, name {}, r {:.2f} {{x}} ${{y}} {}\""), std::string::npos)
        << std_text;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--format=fmt", script}, engine_args()}), fmt_text), 0) << fmt_text;
    EXPECT_NE(fmt_text.find("#include <fmt/format.h>"), std::string::npos) << fmt_text;
    EXPECT_NE(fmt_text.find("fmt::format(\"hp = {}"), std::string::npos) << fmt_text;
    EXPECT_EQ(fmt_text.find("std::format"), std::string::npos) << fmt_text;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--format=printf", script}, engine_args()}), bad), 2);
#if __has_include(<fmt/format.h>)
    // fmt is installed: the generated text compiles.
    const std::string out = scratch.file("hotc_interp_fmt.cpp");
    std::string log;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--format=fmt", "-o", out, script}, engine_args()}), log), 0) << log;
    std::string cc;
    EXPECT_EQ(run_cxx(cat({{"-std=c++20", "-fsyntax-only", "-DFMT_HEADER_ONLY"}, include_args(), {out}}), cc), 0) << cc;
#endif
}

// --view selects the view type of "T[]": hot::span (the default) or std::span (C++20 only).
TEST(HotcTool, ViewOptionSelectsTheViewType)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_view.hot");
    {
        std::ofstream f(script);
        f << "class Foo {\n    int Sum(int[] s) { return s[0]; }\n}\n";
    }
    const auto emit = [&](const Args& flags, std::string& text)
    { return run_hotc(cat({{"--emit-cpp"}, flags, {script}, engine_args()}), text); };
    std::string def, hot_view, std_view, bad, c17;
    // Without --view, the emitted code uses the view type of this build.
    EXPECT_EQ(emit({"--std=c++20"}, def), 0) << def;
    EXPECT_NE(def.find(std::string(hot::view_type_name()) + "::span<int32_t> s"), std::string::npos) << def;
    EXPECT_EQ(emit({"--view=hot", "--std=c++17"}, hot_view), 0) << hot_view;
    EXPECT_NE(hot_view.find("hot::span<int32_t> s"), std::string::npos) << hot_view;
    EXPECT_EQ(emit({"--view=std", "--std=c++20"}, std_view), 0) << std_view;
    EXPECT_NE(std_view.find("std::span<int32_t> s"), std::string::npos) << std_view;
    EXPECT_EQ(std_view.find("hot::span<"), std::string::npos) << std_view;
    EXPECT_NE(emit({"--view=std", "--std=c++17"}, c17), 0) << c17;
    EXPECT_NE(c17.find("needs C++20"), std::string::npos) << c17;
    EXPECT_EQ(emit({"--view=other"}, bad), 2) << bad;
}

// In build mode the view type of this hotc is fixed. A different --view is an error (exit code 2).
TEST(HotcTool, ViewOptionMismatchWithTheBuildIsAnError)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_view_mismatch.hot");
    {
        std::ofstream f(script);
        f << "class Foo {\n    int Sum(int[] s) { return s[0]; }\n}\n";
    }
    const std::string other = std::string(hot::view_type_name()) == "hot" ? "std" : "hot";
    std::string text;
    EXPECT_EQ(run_hotc({script, "--gen", "1", "--out-dir", scratch.file("hotc_view_mismatch_out"),
                           std::string("--view=") + other, "-I", kSource + "/include"},
                  text),
        2)
        << text;
    EXPECT_NE(text.find("--view=" + other), std::string::npos) << text;
}

#if __has_include(<fmt/format.h>)
// The text of --format=fmt compiles as C++17. This is the format of a C++17 build.
TEST(HotcTool, FormatFmtOutputCompilesAsCxx17)
{
    TestScratch scratch;
    const std::string script = write_interp_script(scratch, "hotc_interp_fmt17.hot");
    const std::string out = scratch.file("hotc_interp_fmt17.cpp");
    std::string log, cc;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--format=fmt", "-o", out, script}, engine_args()}), log), 0) << log;
    EXPECT_EQ(run_cxx(cat({kCxx17Flags, {"-DFMT_HEADER_ONLY"}, include_args(), {out}}), cc), 0) << cc;
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
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", "--format=std", "-o", out, script}, engine_args()}), log), 0) << log;
    EXPECT_EQ(run_cxx(cat({{"-std=c++20", "-fsyntax-only"}, include_args(), {out}}), cc), 0) << cc;
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
    EXPECT_EQ(
        run_hotc(cat({{script}, engine_args(),
                     {"-o", module_file(scratch.path(), "hotc_interp_mismatch"), std::string("--format=") + other}}),
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
        "    $Hero Make() { runs += 1; return new $Hero(); }\n"
        "    override void Tick(float dt) {\n"
        "        $Hero a = new $Hero(); $Hero n = null;\n"
        "        $Hero r1 = a ?? Make();\n"
        "        int after_first = runs;\n"
        "        $Hero r2 = n ?? Make();\n"
        "        $Hero r3 = n ?? null;\n"
        "        this.Name = \"first ${after_first} second ${runs} same ${r1 == a} made ${r2 != null} null ${r3 == "
        "null}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = module_file(scratch.path(), "hotc_coalesce_run");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
        "        $Pick a = new $Pick(); $Pick n = null; $Sub s = new $Sub();\n"
        "        var p = a ?? n;\n"
        "        if (p == null) return;\n"
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
        "        if (z == null) return;\n"
        "        z.Name = \"set ${r1} ${r3} ${r4}\";\n"
        "    }\n}\n",
        f);
    std::fclose(f);
    expect_compiles_as_cxx17(script);
    const std::string so = module_file(scratch.path(), "hotc_coalesce_var");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
    const std::string so = module_file(scratch.path(), "hotc_interp_run");
    std::string log;
    ASSERT_EQ(run_hotc(cat({{script}, engine_args(), {"-o", so, "--cxx", HOT_CXX}}), log), 0) << log;
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
        "    $Unit Find() { return null; }\n"
        "    int Mode(std.string s) {\n"
        "        switch (s) {\n"
        "            case \"idle\": return 0;\n"
        "            case \"walk\":\n"
        "            case \"run\": return 1;\n"
        "            default: return 2;\n"
        "        }\n"
        "    }\n"
        "    override void Tick(float dt) {\n"
        "        $Unit u = Find();\n"
        "        u?.Hit();\n"
        "        u?.hp = 3;\n"
        "        $Unit w = u ?? new $Unit();\n"
        "        w.Hit(2);\n"
        "        std.vector<int> none = [];\n"
        "        var (a, b) = std.make_pair(1, 2);\n"
        "        var (u1, u2) = std.make_pair(Find(), Find());\n"
        "        $Unit picked = u1 ?? u2;\n"
        "        var fn = () => a + b;\n"
        "        int n = fn() + Mode(\"run\");\n"
        "        foreach (var (k, v) in named) { var g = (int z) => z + v + a; n += g(1); }\n"
        "        foreach (var (i, x) in enumerate(ids)) n += (int)i * x;\n"
        "        foreach (int i in range(1, 3)) n += i;\n"
        "        try { n += a; } finally { n += b; }\n"
        "        if (picked is $Unit p && p.hp > 0) n += p.hp;\n"
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
        // The include name is relative to the include directory (-I). A path with a drive letter or a
        // backslash is not portable in the text of an include line.
        const std::string name = std::filesystem::relative(h, kSource + "/include").generic_string();
        std::fputs(("#include \"" + name + "\"\n").c_str(), f);
        std::fclose(f);
        std::string cc;
        EXPECT_EQ(run_cxx(cat({kCxx17Flags, {"-I", kSource + "/include", tu}}), cc), 0) << h << "\n" << cc;
    }
}

// --aot writes the C++ of each script and the registration file. It does not compile.
TEST(HotcTool, AotModeWritesTheCppAndTheRegistrationFile)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_aot");
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--aot", kSource + "/tests/scripts/aot_demo.hot", kSource + "/tests/scripts/props_v1.hot",
                                "--out-dir", out_dir},
                           include_args()}),
                  text),
        0)
        << text;
    const std::string demo = read_file(out_dir + "/aot_demo.aot.cpp");
    EXPECT_NE(demo.find("#define script_register hot_script_register_aot_demo"), std::string::npos);
    EXPECT_NE(demo.find("extern \"C\" void script_register("), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(module_file(out_dir, "aot_demo.g1")));
    // Each script gets its interface header. The TU of the script does not include it.
    const std::string iface = read_file(out_dir + "/aot_demo.iface.h");
    EXPECT_NE(iface.find("#pragma once\n"), std::string::npos) << iface;
    EXPECT_NE(iface.find("struct Fighter_Shell"), std::string::npos) << iface;
    EXPECT_EQ(demo.find("aot_demo.iface.h"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(out_dir + "/props_v1.iface.h"));
    const std::string reg = read_file(out_dir + "/hot_aot_registry.cpp");
    EXPECT_NE(reg.find("{\"aot_demo\", &hot_script_register_aot_demo, "), std::string::npos) << reg;
    EXPECT_NE(reg.find("{\"props_v1\", &hot_script_register_props_v1, "), std::string::npos) << reg;
    EXPECT_NE(reg.find("aot_table = {kAotScripts, 2, kAotSlots}"), std::string::npos) << reg;
}

// --aot-optimized adds the define to each script and "true" to the table.
TEST(HotcTool, AotOptimizedWritesTheDefineAndTheTableFlag)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_aot_opt");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/aot_demo.hot";
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--aot", "--aot-optimized", script, "--out-dir", out_dir}, include_args()}), text), 0)
        << text;
    const std::string demo = read_file(out_dir + "/aot_demo.aot.cpp");
    EXPECT_NE(demo.find("\n#define HOT_AOT_OPTIMIZED 1\n"), std::string::npos);
    const std::string reg = read_file(out_dir + "/hot_aot_registry.cpp");
    EXPECT_NE(reg.find("aot_table = {kAotScripts, 1, kAotSlots, true}"), std::string::npos) << reg;
}

// Optimized AOT code has no state_of, no ShellBase and no init_state: the shell has the fields.
TEST(HotcTool, AotOptimizedFieldAccessUsesTheShell)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_aot_opt_fields");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/aot_opt_fields.hot";
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--aot", "--aot-optimized", script, "--out-dir", out_dir}, include_args()}), text), 0)
        << text;
    const std::string out = read_file(out_dir + "/aot_opt_fields.aot.cpp");
    EXPECT_NE(out.find("hot::ShellCore"), std::string::npos) << out;
    EXPECT_EQ(out.find("state_of("), std::string::npos) << out;
    EXPECT_EQ(out.find("ShellBase"), std::string::npos) << out;
    EXPECT_EQ(out.find("init_state"), std::string::npos) << out;
    EXPECT_EQ(out.find("destroy_state"), std::string::npos) << out;
    EXPECT_EQ(out.find("track_object"), std::string::npos) << out;
}

// The same for a C++ script: the fields of another object and of this in a lambda.
TEST(HotcTool, AotOptimizedCppScriptHasNoStateOf)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_aot_opt_cpp_fields");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/aot_cover_cpp.cpp";
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--aot", "--aot-optimized", script, "--out-dir", out_dir}, include_args()}), text), 0)
        << text;
    const std::string out = read_file(out_dir + "/aot_cover_cpp.aot.cpp");
    EXPECT_NE(out.find("CppReader"), std::string::npos) << out;
    EXPECT_EQ(out.find("state_of("), std::string::npos) << out;
    EXPECT_EQ(out.find("ShellBase"), std::string::npos) << out;
}

// --aot-optimized without --aot is a usage error. The tool prints the reason and the usage text.
TEST(HotcTool, AotOptimizedWithoutAotPrintsTheUsage)
{
    TestScratch scratch;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/aot_demo.hot";
    std::string text;
    EXPECT_EQ(run_hotc(Args{"--aot-optimized", script, "--out-dir", scratch.file("aot_out")}, text), 2) << text;
    EXPECT_NE(text.find("hotc: --aot-optimized needs --aot"), std::string::npos) << text;
    EXPECT_NE(text.find("usage: hotc"), std::string::npos) << text;
}

// --aot needs an output directory and does not take the options of the other modes.
TEST(HotcTool, AotModeRejectsBadUsage)
{
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/aot_demo.hot";
    TestScratch scratch;
    const std::string dir = scratch.file("x");
    const std::vector<Args> extras = {{}, {"--out-dir", dir, "--gen", "1"},
        {"--out-dir", dir, "-o", module_file(scratch.path(), "x")}, {"--emit-cpp", "--out-dir", dir}};
    for (const Args& extra : extras)
    {
        std::string text;
        EXPECT_EQ(run_hotc(cat({{"--aot", script}, extra}), text), 2) << text;
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
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--aot", script, "--out-dir", out_dir}, include_args()}), text), 1) << text;
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
    std::vector<std::string> expected = {"cc", std::string("-std=") + hot::cxx_std_name(),
#ifndef _WIN32
        "-fPIC",
#endif
        "-shared", "-g", "-Werror=return-type", "-fwrapv", "-fvisibility=hidden", "-fvisibility-inlines-hidden",
#ifdef _WIN32
        "-DHOT_MODULE_BUILD=1",
#elif !defined(__APPLE__)
        "-fno-plt"
#endif
    };
#ifdef __APPLE__
    expected.push_back("-Wl,-undefined,dynamic_lookup");
#endif
    for (const std::string& f : ms_runtime_flags())
        expected.push_back(f); // MSVC host only
    for (const char* f : {"-resource-dir=/res", "-O2", "-I/i1", "-I/i2"})
        expected.push_back(f);
    for (const std::string& f : hot::format_compile_flags())
        expected.push_back(f); // fmt build only
    for (const std::string& f : hot::checks_compile_flags())
        expected.push_back(f); // HOT_RUNTIME_CHECKS=OFF build only
    for (const std::string& f : hot::thread_safe_compile_flags())
        expected.push_back(f); // HOT_THREAD_SAFE=OFF build only
    for (const std::string& f : hot::traceback_compile_flags())
        expected.push_back(f); // HOT_TRACEBACK=ON build only
    for (const char* f : {"/x.cpp", "-o", "/x.so"})
        expected.push_back(f);
    EXPECT_EQ(hot::compile_command(o, "/x.cpp", "/x.so"), expected);
}

// A Darwin target links with "-undefined dynamic_lookup" and has no "-fno-plt" (an ELF flag).
// The test runs on each host, because the target triple selects the flags.
TEST(HotcTool, CompileCommandForDarwinTarget)
{
    hot::ClangFilesOptions o;
    o.compiler = "clang++";
    o.target_triple = "arm64-apple-macosx13.0";
    const std::vector<std::string> cmd = hot::compile_command(o, "/x.cpp", "/x.so");
    auto has = [&](const char* f) { return std::find(cmd.begin(), cmd.end(), f) != cmd.end(); };
    EXPECT_TRUE(has("--target=arm64-apple-macosx13.0"));
    EXPECT_TRUE(has("-Wl,-undefined,dynamic_lookup"));
    EXPECT_FALSE(has("-fno-plt"));
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
#ifdef _WIN32
    GTEST_SKIP() << "the expected text has the POSIX flags; Windows has other flags, the target flags and "
                    "the JSON escape of the backslash in each path";
#endif
    namespace fs = std::filesystem;
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_cdb");
    const std::string cdb_dir = scratch.file("hotc_cdb_json");
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/boss_v1.hot";
    const std::string engine = std::string(HOT_SOURCE_DIR) + "/tests/engine";
    const std::string inc = std::string(HOT_SOURCE_DIR) + "/include";
    std::string text;
    ASSERT_EQ(run_hotc(cat({{script, "--gen", "1", "--out-dir", out_dir}, {"-I", engine, "-I", inc},
                           {"--cxx", HOT_CXX, "--cxxflag", "-O1", "--compile-commands", cdb_dir}}),
                  text),
        0)
        << text;

    const std::string cpp = fs::absolute(out_dir + "/boss_v1.g1.cpp").string();
    const std::string so = fs::absolute(module_file(out_dir, "boss_v1.g1")).string();
    const std::string cxx = HOT_CXX;
    std::string fmt_args; // fmt build only
    for (const std::string& f : hot::format_compile_flags())
        fmt_args += "\"" + f + "\", ";
    for (const std::string& f : hot::checks_compile_flags())
        fmt_args += "\"" + f + "\", "; // HOT_RUNTIME_CHECKS=OFF build only
    for (const std::string& f : hot::thread_safe_compile_flags())
        fmt_args += "\"" + f + "\", "; // HOT_THREAD_SAFE=OFF build only
    for (const std::string& f : hot::traceback_compile_flags())
        fmt_args += "\"" + f + "\", "; // HOT_TRACEBACK=ON build only
    const std::string expected =
        "[\n  {\n    \"directory\": \"" + fs::current_path().string() + "\",\n    \"file\": \"" + cpp +
        "\",\n    \"arguments\": [\"" + cxx + "\", \"-std=" + std::string(hot::cxx_std_name()) + "\", \"-fPIC\", \"-shared\", \"-g\", "
        "\"-Werror=return-type\", \"-fwrapv\", \"-fvisibility=hidden\", \"-fvisibility-inlines-hidden\", \"-fno-plt\", \"-O1\", \"-I" + engine + "\", \"-I" + inc + "\", " + fmt_args + "\"" + cpp +
        "\", \"-o\", \"" + so + "\"],\n    \"output\": \"" + so + "\"\n  }\n]\n";
    EXPECT_EQ(read_file(cdb_dir + "/compile_commands.json"), expected);
}

// hotc --emit-cpp translates a plain C++ script (a ".cpp" file) to a generation file.
TEST(HotcTool, EmitCppTranslatesACppScript)
{
    std::string text;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/cpp_player_v1.cpp";
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", script}, engine_args()}), text), 0) << text;
    EXPECT_NE(text.find("script_register"), std::string::npos) << text;
}

// The fixture v2 uses HOT_ALIAS and onReload and has no include of "hot/script.h".
// hotc injects the header itself, so the translation works.
TEST(HotcTool, EmitCppInjectsTheScriptHeaderForTheAids)
{
    std::string text;
    const std::string script = std::string(HOT_SOURCE_DIR) + "/tests/scripts/cpp_player_v2.cpp";
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp", script}, engine_args()}), text), 0) << text;
    EXPECT_NE(text.find("script_register"), std::string::npos) << text;
}

// A deleted method is not supported in a ".cpp" script. hotc fails.
TEST(HotcTool, CppScriptWithUnsupportedFormFails)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_cpp_virtual.cpp");
    {
        std::ofstream f(script);
        f << "#include \"Actor.h\"\nclass Foo : public Actor\n{\npublic:\n    void Run() = delete;\n};\n";
    }
    std::string text;
    EXPECT_NE(run_hotc(cat({{"--emit-cpp", script}, engine_args()}), text), 0);
    EXPECT_NE(text.find("error"), std::string::npos) << text;
    EXPECT_NE(text.find("deleted"), std::string::npos) << text;
}

// A syntax error in a ".cpp" script makes hotc fail with an error at the script line.
TEST(HotcTool, CppScriptWithSyntaxErrorFails)
{
    TestScratch scratch;
    const std::string script = scratch.file("hotc_cpp_syntax.cpp");
    {
        std::ofstream f(script);
        f << "#include \"Actor.h\"\nclass Foo : public Actor\n{\npublic:\n    void Tick(float dt) override { int x = ; "
             "}\n};\n";
    }
    std::string text;
    EXPECT_NE(run_hotc(cat({{"--emit-cpp", script}, engine_args()}), text), 0);
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
    EXPECT_EQ(
        run_hotc(cat({{script, "--gen", "1", "-o", module_file(dir, "a"), "--cxx", HOT_CXX}, engine_args()}), text), 1)
        << text;
    EXPECT_NE(text.find("the output file"), std::string::npos) << text;
    EXPECT_EQ(read_file(script), source);
    EXPECT_FALSE(std::filesystem::exists(module_file(dir, "a")));

    // The same file by another path (a relative path with "..") is found too.
    std::string text2;
    EXPECT_EQ(
        run_hotc(cat({{"--emit-cpp", script, "--gen", "1", "-o", dir + "/../hotc_no_overwrite/a.cpp"}, engine_args()}),
            text2),
        1)
        << text2;
    EXPECT_NE(text2.find("the output file"), std::string::npos) << text2;
    EXPECT_EQ(read_file(script), source);
}

// --depfile writes "<output>: deps" in the Make format. The headers that the script includes
// are in the list.
TEST(HotcTool, EmitCppWritesTheDepfile)
{
    TestScratch scratch;
    const std::string out = scratch.file("hotc_dep_emit.cpp");
    const std::string dep = scratch.file("hotc_dep_emit.d");
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--emit-cpp"}, player_args(), {"-o", out, "--depfile", dep}}), text), 0) << text;
    const std::string d = read_file(dep);
    EXPECT_EQ(d.rfind(out + ":", 0), 0u) << d;
    EXPECT_NE(d.find("/tests/engine/Actor.h"), std::string::npos) << d;
    EXPECT_NE(d.find("/tests/engine/EngineMath.h"), std::string::npos) << d;
    EXPECT_EQ(d.find("/include/c++/"), std::string::npos) << d; // no system header
    EXPECT_EQ(d.back(), '\n');
    EXPECT_EQ(std::count(d.begin(), d.end(), '\n'), 1) << d;
}

// A failed translation leaves no depfile.
TEST(HotcTool, EmitCppWritesNoDepfileOnAnError)
{
    TestScratch scratch;
    const std::string dep = scratch.file("hotc_dep_bad.d");
    const std::string out = scratch.file("hotc_dep_bad.cpp");
    const std::string script = scratch.file("hotc_dep_bad.hot");
    std::ofstream(script) << "using \"Actor.h\";\nclass Dep : Nothing {\n}\n";
    std::string text;
    EXPECT_EQ(
        run_hotc(cat({{"--emit-cpp", script, "--gen", "1"}, include_args(), {"-o", out, "--depfile", dep}}), text), 1)
        << text;
    EXPECT_FALSE(std::filesystem::exists(dep));
}

// In AOT mode the file has one rule for each script.
TEST(HotcTool, AotModeWritesOneDepfileRuleForEachScript)
{
    TestScratch scratch;
    const std::string out_dir = scratch.file("hotc_dep_aot");
    const std::string dep = scratch.file("hotc_dep_aot.d");
    std::string text;
    EXPECT_EQ(run_hotc(cat({{"--aot", kSource + "/tests/scripts/aot_demo.hot", kSource + "/tests/scripts/props_v1.hot",
                                "--out-dir", out_dir},
                           include_args(), {"--depfile", dep}}),
                  text),
        0)
        << text;
    const std::string d = read_file(dep);
    EXPECT_NE(d.find(out_dir + "/aot_demo.aot.cpp:"), std::string::npos) << d;
    EXPECT_NE(d.find(out_dir + "/props_v1.aot.cpp:"), std::string::npos) << d;
    EXPECT_EQ(std::count(d.begin(), d.end(), '\n'), 2) << d;
    EXPECT_NE(d.find("/tests/engine/Actor.h"), std::string::npos) << d;
}

// Build mode: the rule has the library as the output.
TEST(HotcTool, BuildModeWritesTheDepfile)
{
    TestScratch scratch;
    const std::string so = module_file(scratch.path(), "hotc_dep_build");
    const std::string dep = scratch.file("hotc_dep_build.d");
    std::string text;
    EXPECT_EQ(run_hotc(cat({player_args(), {"-o", so, "--cxx", HOT_CXX, "--depfile", dep}}), text), 0) << text;
    const std::string d = read_file(dep);
    EXPECT_EQ(d.rfind(so + ":", 0), 0u) << d;
    EXPECT_NE(d.find("/tests/engine/Actor.h"), std::string::npos) << d;
}

} // namespace
