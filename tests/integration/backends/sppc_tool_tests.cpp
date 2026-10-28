// Tests of the tool sppc as a child process.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_BINARY_DIR, SPPC_PATH and SPP_CXX.
#include <gtest/gtest.h>

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "spp/backend.h"
#include "support/script_files.h"
#include "support/test_support.h"

namespace {

using test_support::expect_errors_at_script;
using test_support::line_with;
using test_support::read_file;

// sppc on a bad script: exit code 1, an error at the script line, no library.
TEST(SppcTool, SppcReportsErrorAtTheScriptLine)
{
    const std::string out_dir = std::string(SPP_BINARY_DIR) + "/sppc_diag";
    std::filesystem::remove_all(out_dir);
    const std::string script = std::string(SPP_SOURCE_DIR) + "/example/scripts/bad.spp";
    const std::string cmd = std::string("'") + SPPC_PATH + "' '" + script + "' --gen 1 --out-dir '" +
                            out_dir + "' -I '" + SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR +
                            "/include' --cxx '" + SPP_CXX + "' 2>&1";
    std::string text;
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    const int status = pclose(p);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1) << text;
    expect_errors_at_script(text, line_with(read_file(script), "\"fast\""));
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/bad.g1.so"));
}

// The error text speaks script names. --raw-diagnostics gives the text of the compiler.
TEST(SppcTool, RawDiagnosticsKeepsTheCompilerText)
{
    const std::string out_dir = std::string(SPP_BINARY_DIR) + "/sppc_raw_diag";
    std::filesystem::remove_all(out_dir);
    const std::string script = std::string(SPP_SOURCE_DIR) + "/example/scripts/bad.spp";
    const std::string base = std::string("'") + SPPC_PATH + "' '" + script + "' --gen 1 --out-dir '" + out_dir +
                             "' -I '" + SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR +
                             "/include' --cxx '" + SPP_CXX + "' ";
    std::string script_names, raw;
    FILE* p = popen((base + "2>&1").c_str(), "r");
    ASSERT_NE(p, nullptr);
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) script_names.append(buf, n);
    pclose(p);
    p = popen((base + "--raw-diagnostics 2>&1").c_str(), "r");
    ASSERT_NE(p, nullptr);
    while (size_t n = fread(buf, 1, sizeof buf, p)) raw.append(buf, n);
    pclose(p);
    EXPECT_NE(script_names.find("cannot assign a value of type 'string' to 'float'"), std::string::npos) << script_names;
    EXPECT_EQ(script_names.find("st->"), std::string::npos) << script_names;
    EXPECT_NE(raw.find("assigning to 'float' from incompatible type 'const char[5]'"), std::string::npos) << raw;
    EXPECT_NE(raw.find("st->speed"), std::string::npos) << raw;
}

}  // namespace

namespace {

// Runs a shell command. Gives the exit code and the output.
int run_command(const std::string& cmd, std::string& text)
{
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) return -1;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string player_args()
{
    return std::string("'") + SPP_SOURCE_DIR + "/example/scripts/player_v1.spp' --gen 1 -I '" +
           SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR + "/include'";
}

// sppc --emit-cpp prints the C++ to the standard output, or writes it with -o.
TEST(SppcTool, EmitCppModeTranslatesWithoutCompiling)
{
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp " + player_args() + " 2>&1", text), 0)
        << text;
    EXPECT_NE(text.find("spp::deref"), std::string::npos);

    const std::string out = std::string(SPP_BINARY_DIR) + "/sppc_emit.cpp";
    std::filesystem::remove(out);
    std::string text2;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp " + player_args() + " -o '" + out +
                              "' 2>&1",
                          text2),
              0)
        << text2;
    EXPECT_NE(read_file(out).find("spp::deref"), std::string::npos);  // the #line names differ
}

// sppc -o x.so builds one library. A mode flag of the other mode gives a usage error.
TEST(SppcTool, OutputOptionBuildsOneLibrary)
{
    const std::string dir = std::string(SPP_BINARY_DIR) + "/sppc_one_so";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string text;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' " + player_args() + " -o '" + dir +
                              "/x.so' --cxx '" + SPP_CXX + "' 2>&1",
                          text),
              0)
        << text;
    EXPECT_TRUE(std::filesystem::exists(dir + "/x.so"));

    std::string text2;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp " + player_args() + " --out-dir '" + dir +
                              "' 2>&1",
                          text2),
              2);
}

// A field with a borrowed pointer is an error by default. --no-strict-borrowed makes it a warning.
TEST(SppcTool, StrictBorrowedIsTheDefault)
{
    const std::string script = std::string(SPP_BINARY_DIR) + "/sppc_strict.spp";
    {
        std::FILE* f = std::fopen(script.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("using \"Actor.h\";\nclass P : Actor {\n    Actor* target = null;\n}\n", f);
        std::fclose(f);
    }
    const std::string args = std::string("' --gen 1 -I '") + SPP_SOURCE_DIR + "/example/engine' -I '" +
                             SPP_SOURCE_DIR + "/include'";
    std::string strict, relaxed;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp '" + script + args + " 2>&1", strict), 1)
        << strict;
    EXPECT_NE(strict.find("a field cannot hold a borrowed pointer"), std::string::npos) << strict;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp --no-strict-borrowed '" + script + args +
                              " 2>&1",
                          relaxed),
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

std::string write_interp_script(const std::string& name)
{
    const std::string script = std::string(SPP_BINARY_DIR) + "/" + name;
    std::FILE* f = std::fopen(script.c_str(), "w");
    if (f) {
        std::fputs(kInterpScript, f);
        std::fclose(f);
    }
    return script;
}

std::string engine_args()
{
    return std::string(" --gen 1 -I '") + SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR +
           "/include'";
}

// --format=fmt only changes the text. The default is std::format.
TEST(SppcTool, FormatOptionSelectsTheFormatFunction)
{
    const std::string script = write_interp_script("sppc_interp_text.spp");
    std::string std_text, fmt_text, bad;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp '" + script + "'" + engine_args() + " 2>&1",
                          std_text),
              0)
        << std_text;
    EXPECT_NE(std_text.find("#include <format>"), std::string::npos) << std_text;
    EXPECT_NE(std_text.find("std::format(\"hp = {}, name {}, r {:.2f} {{x}} ${{y}} {}\""), std::string::npos)
        << std_text;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp --format=fmt '" + script + "'" +
                              engine_args() + " 2>&1",
                          fmt_text),
              0)
        << fmt_text;
    EXPECT_NE(fmt_text.find("#include <fmt/format.h>"), std::string::npos) << fmt_text;
    EXPECT_NE(fmt_text.find("fmt::format(\"hp = {}"), std::string::npos) << fmt_text;
    EXPECT_EQ(fmt_text.find("std::format"), std::string::npos) << fmt_text;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp --format=printf '" + script + "'" +
                              engine_args() + " 2>&1",
                          bad),
              2);
#if __has_include(<fmt/format.h>)
    // fmt is installed: the generated text compiles.
    const std::string out = std::string(SPP_BINARY_DIR) + "/sppc_interp_fmt.cpp";
    std::string log;
    EXPECT_EQ(run_command(std::string("'") + SPPC_PATH + "' --emit-cpp --format=fmt -o '" + out + "' '" + script +
                              "'" + engine_args() + " 2>&1",
                          log),
              0)
        << log;
    std::string cc;
    EXPECT_EQ(run_command(std::string("'") + SPP_CXX + "' -std=c++20 -fsyntax-only -DFMT_HEADER_ONLY -I '" +
                              SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR + "/include' '" + out +
                              "' 2>&1",
                          cc),
              0)
        << cc;
#endif
}

#if __has_include(<format>)
// The script is compiled with std::format and run.
TEST(SppcTool, InterpolationRunsWithStdFormat)
{
    const std::string script = write_interp_script("sppc_interp_run.spp");
    const std::string so = std::string(SPP_BINARY_DIR) + "/sppc_interp_run.so";
    std::filesystem::remove(so);
    std::string log;
    ASSERT_EQ(run_command(std::string("'") + SPPC_PATH + "' '" + script + "'" + engine_args() + " -o '" + so +
                              "' --cxx '" + SPP_CXX + "' 2>&1",
                          log),
              0)
        << log;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(test_support::library(so.c_str()), "Hero"));
    s.world.Tick(1.0f / 60);
    EXPECT_EQ(s.actor->Name, "hp = 7, name L7, r 0.50 {x} ${y} hi");
}
#endif

// --ship writes the C++ of each script and the registration file. It does not compile.
TEST(SppcTool, ShipModeWritesTheCppAndTheRegistrationFile)
{
    const std::string out_dir = std::string(SPP_BINARY_DIR) + "/sppc_ship";
    std::filesystem::remove_all(out_dir);
    const std::string scripts = std::string("'") + SPP_SOURCE_DIR + "/example/scripts/ship_demo.spp' '" +
                                SPP_SOURCE_DIR + "/example/scripts/props_v1.spp'";
    const std::string cmd = std::string("'") + SPPC_PATH + "' --ship " + scripts + " --out-dir '" + out_dir +
                            "' -I '" + SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR + "/include' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    std::string text;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    const int status = pclose(p);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << text;
    const std::string demo = read_file(out_dir + "/ship_demo.ship.cpp");
    EXPECT_NE(demo.find("#define script_register spp_script_register_ship_demo"), std::string::npos);
    EXPECT_NE(demo.find("extern \"C\" void script_register("), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/ship_demo.g1.so"));
    const std::string reg = read_file(out_dir + "/spp_shipped_registry.cpp");
    EXPECT_NE(reg.find("{\"ship_demo\", &spp_script_register_ship_demo}"), std::string::npos) << reg;
    EXPECT_NE(reg.find("{\"props_v1\", &spp_script_register_props_v1}"), std::string::npos) << reg;
    EXPECT_NE(reg.find("shipped_table = {kShippedScripts, 2}"), std::string::npos) << reg;
}

// --ship needs an output directory and does not take the options of the other modes.
TEST(SppcTool, ShipModeRejectsBadUsage)
{
    const std::string script = std::string(SPP_SOURCE_DIR) + "/example/scripts/ship_demo.spp";
    for (const char* extra : {"", "--out-dir /tmp/x --gen 1", "--out-dir /tmp/x -o /tmp/x.so", "--emit-cpp --out-dir /tmp/x"}) {
        const std::string cmd = std::string("'") + SPPC_PATH + "' --ship '" + script + "' " + extra + " >/dev/null 2>&1";
        const int status = std::system(cmd.c_str());
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 2) << extra;
    }
}

// A script that does not parse gives exit code 1, an error at the script line and no
// registration file. A type error appears later, in the compile of the library.
TEST(SppcTool, ShipModeReportsParseErrorAtTheScriptLine)
{
    const std::string out_dir = std::string(SPP_BINARY_DIR) + "/sppc_ship_bad";
    std::filesystem::remove_all(out_dir);
    std::filesystem::create_directories(out_dir);
    const std::string script = out_dir + "/broken.spp";
    {
        std::ofstream f(script);
        f << "using \"Actor.h\";\n\nclass Broken : Actor {\n    int x = ;\n}\n";
    }
    const std::string cmd = std::string("'") + SPPC_PATH + "' --ship '" + script + "' --out-dir '" + out_dir +
                            "' -I '" + SPP_SOURCE_DIR + "/example/engine' -I '" + SPP_SOURCE_DIR + "/include' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    std::string text;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) text.append(buf, n);
    const int status = pclose(p);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1) << text;
    EXPECT_NE(text.find("broken.spp:4:"), std::string::npos) << text;
    EXPECT_FALSE(std::filesystem::exists(out_dir + "/spp_shipped_registry.cpp"));
}

}  // namespace
