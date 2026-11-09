// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the JIT backend only (HOT_ENABLE_JIT resolves ON).
//
// The backend must not write a file and must not start a process. These tests
// check this, and they check that a generation can go away.
//
// CMake defines these macros: HOT_SOURCE_DIR, HOT_SCRATCH_DIR, HOT_CXX and HOT_RESOURCE_DIR.
#include "hot/backend.h"
#include "hot/command_line.h"
#include "hot/config.h"
#include "hot/jit_backend.h"
#include "support/dwarf_lines.h"
#include "support/f1_scenario.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"

#include "jit_engine.h"
#include "jit_host_allocators.h"
#include "jit_msvc_symbols.h"
#include "jit_search_order.h"

#ifdef _MSC_VER
    #include "hot/paths.h"
    #include "hot/runtime/records.h"
    #include "hot/runtime/shell.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <functional>
    #include <iterator>
    #include <malloc.h>
    #include <map>
    #include <stdexcept>
    #include <typeinfo>
    #include <vector>
    #include <windows.h>
// The epoch of the local statics of the thread (jit_msvc_symbols.cpp).
extern "C" __declspec(thread) int _Init_thread_epoch;

// The exception types of the exception probe. The host and the JIT code have the same text, so the
// two definitions have the same decorated name. A catch matches a type by this name.
    #define HOT_EH_PROBE_TYPES                                                                              \
        struct ProbeHostError                                                                               \
        {                                                                                                   \
            explicit ProbeHostError(int c) : code(c) {}                                                     \
            virtual ~ProbeHostError() = default;                                                            \
            int code;                                                                                       \
        };                                                                                                  \
        struct ProbeJitError                                                                                \
        {                                                                                                   \
            int code;                                                                                       \
        };
    #define HOT_EH_TEXT_(...) #__VA_ARGS__
    #define HOT_EH_TEXT(...) HOT_EH_TEXT_(__VA_ARGS__)
HOT_EH_PROBE_TYPES
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifndef _WIN32
// The GDB JIT interface (the layout is fixed by the GDB manual). LLVM defines the descriptor.
// The tests that use it read an ELF image, so they are POSIX only.
extern "C"
{
    struct jit_code_entry
    {
        jit_code_entry* next_entry;
        jit_code_entry* prev_entry;
        const char* symfile_addr;
        uint64_t symfile_size;
    };
    struct jit_descriptor
    {
        uint32_t version;
        uint32_t action_flag;
        jit_code_entry* relevant_entry;
        jit_code_entry* first_entry;
    };
    extern jit_descriptor __jit_debug_descriptor;
}
#endif

namespace
{

namespace fs = std::filesystem;

hot::ScriptSource source(const char* file)
{
    const std::string path = std::string(HOT_SOURCE_DIR) + "/tests/scripts/" + file;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return {path, ss.str(), {}};
}

// The id of this process.
int current_pid()
{
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

// Runs a shell command and reads its output. Windows has no single quotes: the
// callers use double quotes in the command.
FILE* open_pipe(const std::string& command)
{
#ifdef _WIN32
    return _popen(command.c_str(), "r");
#else
    return popen(command.c_str(), "r");
#endif
}

void close_pipe(FILE* pipe)
{
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
}

#ifndef _WIN32
// The page faults of the children that the process waited for. A child that
// ran and ended always adds page faults here.
long child_faults()
{
    rusage ru{};
    getrusage(RUSAGE_CHILDREN, &ru);
    return ru.ru_minflt + ru.ru_majflt;
}

// The number of the direct children of this process that are alive now.
int live_children()
{
    int n = 0;
    for (const auto& task : fs::directory_iterator("/proc/self/task"))
    {
        std::ifstream f(task.path() / "children");
        for (std::string pid; f >> pid;)
            ++n;
    }
    return n;
}
#endif

// Sets the current directory to a new empty directory. On POSIX it sets TMPDIR to the
// directory too. The destructor restores both and removes the directory.
struct EmptyWorkDir
{
    fs::path dir;
    fs::path old_cwd = fs::current_path();
#ifndef _WIN32
    std::string old_tmp;
    bool had_tmp = false;
#endif

    EmptyWorkDir()
    {
        dir = fs::temp_directory_path() / ("hot_jit_empty_" + std::to_string(current_pid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
#ifndef _WIN32
        if (const char* t = std::getenv("TMPDIR"))
        {
            had_tmp = true;
            old_tmp = t;
        }
        setenv("TMPDIR", dir.c_str(), 1);
#endif
        fs::current_path(dir);
    }
    ~EmptyWorkDir()
    {
        fs::current_path(old_cwd);
#ifndef _WIN32
        if (had_tmp)
            setenv("TMPDIR", old_tmp.c_str(), 1);
        else
            unsetenv("TMPDIR");
#endif
        fs::remove_all(dir);
    }
    size_t entries() const
    {
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir))
        {
            (void)e;
            ++n;
        }
        return n;
    }
};

#ifndef _WIN32
// The detector must see a child process, or the next test proves nothing.
TEST(JitTests, ChildDetectorSeesAChild)
{
    const long before = child_faults();
    ASSERT_EQ(std::system("true"), 0);
    EXPECT_GT(child_faults(), before);
}

TEST(JitTests, NoFileAndNoChildProcess)
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    if (std::string(backend->name()) != "JIT")
        GTEST_SKIP() << "the test needs the JIT backend";
    EmptyWorkDir work;
    const long faults_before = child_faults();
    {
        hot::LoadResult v1 = backend->load(source("player_v1.hot"), 1);
        ASSERT_TRUE(v1) << v1.error;
        hot::LoadResult v2 = backend->load(source("player_v2.hot"), 2);
        ASSERT_TRUE(v2) << v2.error;
        // A bad script also leaves no file and starts no process.
        hot::LoadResult bad = backend->load(source("bad.hot"), 1);
        EXPECT_FALSE(bad);
        f1::run_f1_reload(std::move(*v1.module), std::move(*v2.module));
    }
    EXPECT_EQ(work.entries(), 0u) << "the work directory is not empty";
    EXPECT_EQ(live_children(), 0);
    EXPECT_EQ(child_faults(), faults_before) << "a child process ran";
}
#endif

// The error text names the script. The text has no stray line from the compiler.
TEST(JitTests, ErrorTextNamesTheScript)
{
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    hot::LoadResult bad = backend->load(source("bad.hot"), 1);
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error.find("bad.hot:9:"), std::string::npos) << bad.error;
    EXPECT_NE(bad.error.find(" error: "), std::string::npos) << bad.error;
}

// A precompiled header that the backend made works with the in-memory compile.
TEST(JitTests, PrecompiledHeaderWorks)
{
    EmptyWorkDir work;
    const std::string header = (work.dir / "common.hpp").string();
    const std::string pch = (work.dir / "common.pch").string();
    {
        std::ofstream f(header);
        f << "#include <algorithm>\n#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
             "#include <new>\n#include <string>\n#include <type_traits>\n#include <typeinfo>\n"
             "#include <utility>\n#include \"hot/runtime.h\"\n#include \"Actor.h\"\n"
             "#include \"EngineMath.h\"\n";
    }
    hot::JitOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.resource_dir = HOT_RESOURCE_DIR;
    std::string error;
    {
        hot::JitBackend maker(o);
        ASSERT_TRUE(maker.make_pch(header, pch, error)) << error;
    }
    ASSERT_TRUE(fs::exists(pch));
    o.pch_path = pch;
    hot::JitBackend backend(o);
    hot::LoadResult v1 = backend.load(source("player_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend.load(source("player_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    f1::run_f1_reload(std::move(*v1.module), std::move(*v2.module));
}

// The JIT code reads the call depth of its own thread (errors.h). The main thread is inside a
// call, so its depth is 1. A second thread calls an override that throws hot::IndexOutOfRange.
// That call is the outermost call of the second thread, so it reports the error and returns.
TEST(JitTests, CallDepthIsPerThreadInJitCode)
{
    if (!HOT_RUNTIME_CHECKS)
        GTEST_SKIP() << "the test needs the bounds check to raise the error";
    const hot::ScriptSource src = {"depth_thread.hot", R"(
using "Actor.h";
using "EngineMath.h";

class Player : Actor {
    int[4] vals;

    override void Tick(float dt) {
        int i = 4;
        this.vals[i] = 1;
    }

    override void TakeDamage(float amount) {}
    override void TakeDamage(ref Actor source, float amount) {}
}
)",
        {}};
    hot::DefaultBackendOptions jit_options;
    jit_options.kind = hot::BackendKind::Jit;
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend(jit_options);
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    std::atomic<int> reports{0};
    hot::set_script_error_handler([&](const hot::ScriptErrorReport&) { ++reports; });
    bool escaped = false;
    uint32_t depth_after = 99;
    {
        hot::detail::CallDepth outer;
        std::thread t([&] {
            for (int k = 0; k < 100; ++k)
            {
                try
                {
                    s.actor->Tick(test_support::kDt);
                }
                catch (...)
                {
                    escaped = true;
                }
            }
            depth_after = hot::detail::call_depth();
        });
        t.join();
        EXPECT_EQ(hot::detail::call_depth(), 1u);
    }
    hot::set_script_error_handler({});
    EXPECT_FALSE(escaped);
    EXPECT_EQ(reports.load(), 100);
    EXPECT_EQ(depth_after, 0u);
    EXPECT_EQ(hot::detail::call_depth(), 0u);
}

// Exceptions: one is caught inside the JIT code, one goes through JIT frames to the
// host. Both need the unwind tables of the JIT code (EH frames), which JITLink registers.
TEST(JitTests, ExceptionsWorkInJitCode)
{
    const hot::ScriptSource src = {"throws.hot", R"(
using "Actor.h";
using "EngineMath.h";

class Player : Actor {
    float speed = 5.0;

    override void Tick(float dt) {
        try { throw 7; } catch (int e) { this.speed = 9.0; }
    }

    override void TakeDamage(float amount) {
        throw 42;
    }

    override void TakeDamage(ref Actor source, float amount) {}
}
)",
        {}};
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
    hot::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(test_support::kDt); // the throw and the catch are both in the JIT code
    ASSERT_NE(s.field("speed"), nullptr);
    EXPECT_EQ(*s.field("speed"), 9.0f);
    int caught = 0;
    try
    {
        s.actor->TakeDamage(1.0f); // the throw goes through JIT frames to this frame
    }
    catch (int e)
    {
        caught = e;
    }
    EXPECT_EQ(caught, 42);
}

// A Module that a copy of keep_alive holds stays valid after the backend is gone.
// When the last copy goes, the generation is removed from the JIT.
TEST(JitTests, ModuleOutlivesBackendAndGenerationsCanBeRemoved)
{
    hot::Module kept;
    {
        std::unique_ptr<hot::Backend> backend = hot::make_default_backend();
        for (int i = 0; i < 3; ++i)
        {
            hot::LoadResult r = backend->load(source("player_v1.hot"), 1);
            ASSERT_TRUE(r) << r.error;
            // The same generation loads again and again: each time a new JITDylib.
            if (i == 2)
                kept = std::move(*r.module);
        }
    }
    ASSERT_NE(kept.register_fn, nullptr);
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(kept)));
    EXPECT_NE(s.field("hp"), nullptr);
}

#ifndef _WIN32
// Experiment F3: the JIT registers each object with the debugger. The object is an ELF image
// in memory. Its line table names the .hot file and the lines of the script.
TEST(JitTests, DebuggerGetsObjectWithScriptLines)
{
#ifdef __APPLE__
    GTEST_SKIP() << "Fails on macOS: https://github.com/rokups/hotpp/issues/1";
#endif
    if (!test_support::have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    hot::JitOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/include", std::string(HOT_SOURCE_DIR) + "/tests/engine"};
    o.resource_dir = HOT_RESOURCE_DIR;
    hot::JitBackend backend(o);
    const hot::ScriptSource src = source("player_v2.hot");
    hot::LoadResult r = backend.load(src, 1);
    ASSERT_TRUE(r) << r.error;
    ASSERT_TRUE(backend.debugger_support()) << backend.debugger_error();

    // The newest entry is the last in the list.
    const jit_code_entry* last = __jit_debug_descriptor.first_entry;
    ASSERT_NE(last, nullptr) << "no object was registered";
    while (last->next_entry)
        last = last->next_entry;
    ASSERT_GT(last->symfile_size, 0u);
    test_support::TestScratch scratch;
    const std::string image = scratch.file("jit_debug_object.o");
    {
        std::ofstream f(image, std::ios::binary);
        f.write(last->symfile_addr, static_cast<std::streamsize>(last->symfile_size));
    }
    const std::set<int> lines = test_support::lines_of_file(test_support::dump_line_table(image), "player_v2.hot");
    EXPECT_TRUE(lines.count(test_support::line_with(src.text, "Math.Min")));
}
#endif

// The option turns the plugin off.
TEST(JitTests, DebuggerSupportCanBeOff)
{
    hot::JitOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/include", std::string(HOT_SOURCE_DIR) + "/tests/engine"};
    o.resource_dir = HOT_RESOURCE_DIR;
    o.debugger_support = false;
    hot::JitBackend backend(o);
    hot::LoadResult r = backend.load(source("player_v1.hot"), 1);
    ASSERT_TRUE(r) << r.error;
    EXPECT_FALSE(backend.debugger_support());
}

// A warning in a .cpp script (an error by -Werror) shows the script name, the line, the real
// column and the script text. No generated name is visible.
TEST(JitTests, CppScriptWarningSpeaksScriptText)
{
    {
        // The exact text of the warnings is the text of Clang.
        std::string version;
#ifdef _WIN32
        // The cmd shell needs double quotes. The outer pair of quotes keeps the command whole.
        const std::string command = std::string("\"\"") + HOT_CXX + "\" --version 2>&1\"";
#else
        const std::string command = std::string("'") + HOT_CXX + "' --version 2>&1";
#endif
        if (FILE* p = open_pipe(command))
        {
            char buf[256];
            while (size_t n = fread(buf, 1, sizeof buf, p))
                version.append(buf, n);
            close_pipe(p);
        }
        if (version.find("clang") == std::string::npos)
            GTEST_SKIP() << "the compiler is not Clang";
    }
    hot::JitOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/include", std::string(HOT_SOURCE_DIR) + "/tests/engine"};
    o.resource_dir = HOT_RESOURCE_DIR;
    o.extra_flags = {"-Wall", "-Werror"};
    hot::JitBackend backend(o);
    hot::ScriptSource src = source("cpp_diag_warn.cpp");
    src.name = "warn.cpp"; // a relative name
    hot::LoadResult r = backend.load(src, 1);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error.find("warn.cpp:10:13: error: unused variable 'unused' [-Werror,-Wunused-variable]"),
        std::string::npos)
        << r.error;
    EXPECT_NE(r.error.find("   10 |         int unused = 1;\n      |             ^~~~~~\n"), std::string::npos)
        << r.error;
    EXPECT_NE(r.error.find("warn.cpp:11:16: error: "), std::string::npos) << r.error;
    EXPECT_EQ(r.error.find("warn.cpp:10:1:"), std::string::npos) << r.error; // the column is not 1
    EXPECT_EQ(r.error.find("warn.cpp:11:1:"), std::string::npos) << r.error;
    for (const char* generated : {".g1.cpp", "_Shell", "State_g", "__self", "state_of"})
        EXPECT_EQ(r.error.find(generated), std::string::npos) << generated << "\n" << r.error;
}

// The JIT code has NDEBUG when the host has it, so an inline function of a runtime header has one
// body. The JIT code stays optimized. A header that each file of the script includes first
// stops the compile when a value differs.
TEST(JitTests, JitCodeHasTheNdebugOfTheHost)
{
    test_support::TestScratch scratch;
    const std::string header = scratch.file("check_ndebug.h");
    {
        std::ofstream out(header);
        out << "#if defined(NDEBUG) != HOT_TEST_HOST_NDEBUG\n"
               "#error the JIT code and the host differ in NDEBUG\n"
               "#endif\n"
               "#ifndef __OPTIMIZE__\n"
               "#error the JIT code is not optimized\n"
               "#endif\n";
    }
#ifdef NDEBUG
    const char* host_ndebug = "-DHOT_TEST_HOST_NDEBUG=1";
#else
    const char* host_ndebug = "-DHOT_TEST_HOST_NDEBUG=0";
#endif
    hot::JitOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/include", std::string(HOT_SOURCE_DIR) + "/tests/engine"};
    o.resource_dir = HOT_RESOURCE_DIR;
    o.extra_flags = {host_ndebug, "-include", header};
    hot::JitBackend backend(o);
    hot::LoadResult r = backend.load(source("player_v1.hot"), 1);
    EXPECT_TRUE(r) << r.error;
}

// The JIT engine compiles "text" for "target" (empty: the host). Null on failure.
hot_jit_ir* compile_ir(const std::string& target, const std::string& text)
{
    const std::string path = hot::jit_virtual_path("/hot-jit-test", "link.cpp", fs::current_path().string());
    const std::string target_arg = "--target=" + target;
    std::vector<std::string> flags = hot::host_ms_runtime_lib_flags(target);
    std::vector<const char*> args = {"clang++", "-std=c++17"};
    if (!target.empty())
        args.push_back(target_arg.c_str());
    for (const std::string& f : flags)
        args.push_back(f.c_str());
    args.push_back(path.c_str());
    const char* paths[] = {path.c_str()};
    const char* texts[] = {text.c_str()};
    hot_jit_ir* ir = nullptr;
    char* diagnostics = nullptr;
    const int ok = hot_jit_compile(args.data(), args.size(), paths, texts, 1, &ir, &diagnostics);
    const std::string messages = diagnostics ? diagnostics : "";
    hot_jit_string_free(diagnostics);
    EXPECT_EQ(ok, 1) << messages;
    return ok ? ir : nullptr;
}

// The IR text of the link preparation of hot_jit_add for "text" compiled for "target".
std::string link_ir_text(const std::string& target, const std::string& text)
{
    hot_jit_ir* ir = compile_ir(target, text);
    if (!ir)
        return std::string();
    char* ir_text = hot_jit_ir_link_text(ir);
    std::string out = ir_text ? ir_text : "";
    hot_jit_string_free(ir_text);
    hot_jit_ir_free(ir);
    return out;
}

// RuntimeDyld gives a jump stub to a REL32 reference to an external symbol, also to data. On a
// COFF target the engine makes each data declaration dllimport, so the code reads the address
// from a pointer cell. The control record of a thread variable (emulated TLS) is dllimport too.
// A function keeps its jump stub.
TEST(JitTests, CoffDataDeclarationsAreDllimport)
{
    const std::string ir = link_ir_text("x86_64-pc-windows-msvc",
        "extern \"C\" int g;\n"
        "extern \"C\" __declspec(thread) int e;\n"
        "extern \"C\" int k();\n"
        "int* f() { return &g; }\n"
        "int* h() { return &e; }\n"
        "int c() { return k(); }\n");
    EXPECT_NE(ir.find("@g = external dllimport global i32"), std::string::npos) << ir;
    EXPECT_NE(ir.find("@__emutls_v.e = external dllimport global { i64, i64, ptr, ptr }"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("@e = external dllimport"), std::string::npos) << ir;
    EXPECT_NE(ir.find("declare dso_local i32 @k()"), std::string::npos) << ir;
}

// The JIT code has no native TLS directory. On the MSVC target an access to a thread variable
// must not read the TLS guard of the host or call its on-demand TLS init (jit_engine.cpp).
TEST(JitTests, MsvcThreadVariablesHaveNoTlsGuard)
{
    const std::string ir = link_ir_text("x86_64-pc-windows-msvc",
        "extern \"C\" __declspec(thread) int e;\n"
        "extern thread_local int t;\n"
        "int* h() { return &e; }\n"
        "int* u() { return &t; }\n");
    EXPECT_NE(ir.find("@__emutls_v.e"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("__tls_guard"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("__dyn_tls_on_demand_init"), std::string::npos) << ir;
}

// The unwind tables of COFF x64 JIT code: each RUNTIME_FUNCTION, its UNWIND_INFO, the handler
// and the C++ FuncInfo must be valid for the image base of the table (hot_jit_coff_unwind_check).
// The code has try and catch, destructors, a noexcept function, a throw with an external copy
// constructor, COMDAT functions and a second code section.
TEST(JitTests, CoffUnwindTablesAreValid)
{
    hot_jit_ir* ir = compile_ir("x86_64-pc-windows-msvc",
        "struct D { int* p; ~D(); };\n"
        "struct E { int v; E(int x) : v(x) {} E(const E&); };\n"
        "void ext(int);\n"
        "int f(int x) { D d{&x}; try { ext(x); } catch (int v) { return v; } catch (E& e) { return e.v; }\n"
        "  catch (...) { return -1; } return 0; }\n"
        "void g(int x) noexcept { D d{&x}; ext(x); }\n"
        "void h(int x) { if (x) throw E(x); ext(x); }\n"
        "inline int k(int x) { try { ext(x); } catch (int v) { return v; } return 1; }\n"
        "template <class T> T tt(T x) { D d{nullptr}; ext(x); return x; }\n"
        "int use() { return k(1) + tt(2); }\n"
        "__declspec(code_seg(\".text$mn\")) int m(int x) { D d{&x}; try { ext(x); } catch (E&) { return 2; }\n"
        "  return x; }\n");
    ASSERT_NE(ir, nullptr);
    char* text = hot_jit_coff_unwind_check(ir);
    hot_jit_ir_free(ir);
    const std::string report = text ? text : "";
    hot_jit_string_free(text);
    if (report.rfind("skip: ", 0) == 0)
        GTEST_SKIP() << report; // the LLVM build cannot make x86_64 COFF code
    const size_t eol = report.find('\n');
    ASSERT_NE(eol, std::string::npos) << report;
    EXPECT_EQ(report.substr(eol + 1), "") << report;
    unsigned tables = 0, entries = 0, named = 0, handlers = 0, tries = 0, types = 0;
    ASSERT_EQ(std::sscanf(report.c_str(), "tables %u entries %u named %u handlers %u try %u catch_types %u",
                  &tables, &entries, &named, &handlers, &tries, &types),
        6)
        << report;
    EXPECT_GE(tables, 3u) << report; // COMDAT functions and the second code section have own tables
    EXPECT_GE(handlers, 4u) << report;
    EXPECT_GE(tries, 3u) << report;
    EXPECT_GE(types, 3u) << report;
}

// An ELF or Mach-O target keeps its declarations: the JIT links them through the GOT.
TEST(JitTests, NonCoffDeclarationsStay)
{
    const std::string ir = link_ir_text("x86_64-unknown-linux-gnu",
        "extern \"C\" int g;\n"
        "extern \"C\" thread_local int e;\n"
        "int* f() { return &g; }\n"
        "int* h() { return &e; }\n");
    EXPECT_NE(ir.find("@g = external global i32"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("dllimport"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("__emutls_v"), std::string::npos) << ir;
}

#ifdef _MSC_VER
// The types that the JIT code and the host must see with one layout. The alias has no comma, so
// it can be a macro argument.
using ProbeMap = std::map<int, int>;
#define HOT_PROBE_TYPES(X)                                                                          \
    X(hot::ShellBase) X(hot::StateLayout) X(hot::FieldInfo) X(std::string) X(std::vector<int>)      \
        X(std::shared_ptr<int>) X(ProbeMap) X(std::function<void()>)

// The stdio state and functions that the JIT code and the host must share. Another C runtime has
// another FILE layout: its fflush on a FILE of the host breaks that FILE. Each item names a function
// or an object of the C runtime or of the C++ library.
#define HOT_PROBE_STDIO(X) X(fflush) X(fputs) X(fwrite) X(__acrt_iob_func) X(__stdio_common_vfprintf) X(std::cout)

// The file name of the module that contains "address", for a message.
std::string module_of(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module))
        return "no module";
    char name[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, name, MAX_PATH))
        return "a module without a name";
    return name;
}

// The probe module. probe_symbol(i) gives the address of item i of the allocator family
// (jit_host_allocators.h) as the JIT code sees it. probe_layout(2 * i) and probe_layout(2 * i + 1)
// give the size and the alignment of type i of HOT_PROBE_TYPES. The other functions allocate or
// free a block for the cross checks.
const char* probe_source()
{
#define HOT_PROBE_ADDRESS(n, e) "    (const void*)" #e ",\n"
#define HOT_PROBE_LAYOUT(t) "    sizeof(" #t "), alignof(" #t "),\n"
#define HOT_PROBE_STDIO_ADDRESS(n) "    (const void*)&" #n ",\n"
    return "#include <cstdio>\n#include <cstdlib>\n#include <functional>\n#include <iostream>\n#include <malloc.h>\n"
           "#include <map>\n#include <memory>\n#include <new>\n#include <string>\n#include <typeinfo>\n"
           "#include <vector>\n"
           "static const void* const probe_stdio_table[] = {\n" HOT_PROBE_STDIO(HOT_PROBE_STDIO_ADDRESS) "};\n"
           "extern \"C\" const void* probe_stdio(int i) { return probe_stdio_table[i]; }\n"
           "extern \"C\" const void* probe_stdout() { return stdout; }\n"
           "extern \"C\" void probe_write(int x)\n{\n"
           "    std::printf(\"probe printf %d\\n\", x);\n"
           "    std::fputs(\"probe fputs\\n\", stdout);\n"
           "    std::fflush(stdout);\n"
           "    std::cout << \"probe cout \" << x << std::endl;\n}\n"
           "#include \"hot/runtime/records.h\"\n"
           "#include \"hot/runtime/shell.h\"\n"
           "using ProbeMap = std::map<int, int>;\n"
           "extern \"C\" __declspec(thread) int _Init_thread_epoch;\n"
           "static const void* const probe_table[] = {\n" HOT_JIT_HOST_OPERATORS(HOT_PROBE_ADDRESS)
               HOT_JIT_HOST_C_HEAP(HOT_PROBE_ADDRESS) HOT_JIT_HOST_MSVC_HEAP(HOT_PROBE_ADDRESS) "};\n"
           "static const std::size_t probe_layouts[] = {\n" HOT_PROBE_TYPES(HOT_PROBE_LAYOUT) "};\n"
           "extern \"C\" const void* probe_symbol(int i) { return probe_table[i]; }\n"
           "extern \"C\" std::size_t probe_layout(int i) { return probe_layouts[i]; }\n"
           "extern \"C\" const void* probe_epoch() { return &_Init_thread_epoch; }\n"
           "extern \"C\" const void* probe_type_info_root() { return &__type_info_root_node; }\n"
           "extern \"C\" void* probe_new() { return new int(7); }\n"
           "extern \"C\" void probe_delete(void* p) { delete static_cast<int*>(p); }\n"
           "extern \"C\" void* probe_malloc() { return std::malloc(64); }\n"
           "extern \"C\" void* probe_aligned_malloc() { return _aligned_malloc(64, 64); }\n";
#undef HOT_PROBE_ADDRESS
#undef HOT_PROBE_LAYOUT
#undef HOT_PROBE_STDIO_ADDRESS
}

// A probe in JIT code gives the addresses of the allocator family and of runtime state, and the
// layout of runtime and library types, as the JIT code sees them. These must be the values of the
// host: else the JIT code uses another heap, another state or another type. Then blocks go from
// one side to the other side and back. The text names each value that differs.
// Takes a text of the engine and frees it.
std::string take_text(char* text)
{
    std::string out = text ? text : "";
    hot_jit_string_free(text);
    return out;
}

// One probe module in its own engine. The destructor removes the module, then the engine.
struct JitProbe
{
    hot_jit_engine* engine = nullptr;
    hot_jit_dylib* dylib = nullptr;
    JitProbe() = default;
    JitProbe(const JitProbe&) = delete;
    JitProbe& operator=(const JitProbe&) = delete;
    ~JitProbe()
    {
        if (dylib)
            hot_jit_remove(dylib);
        if (engine)
            hot_jit_engine_free(engine);
    }
};

// Compiles "source" with the runtime flags of the host and links it in a new engine, the same as
// the backend. Returns an empty text, or the reason of the failure.
std::string make_probe(const char* file, const char* source, JitProbe& probe)
{
    const hot::DefaultPaths paths = hot::default_paths();
    const std::string path = hot::jit_virtual_path("/hot-jit-test", file, fs::current_path().string());
    std::vector<std::string> flags = {"clang++", "-std=" + std::string(hot::cxx_std_name()), "-DHOT_MODULE_BUILD=1",
        "-fexceptions", "-fcxx-exceptions", "-I" HOT_SOURCE_DIR "/include"};
    for (const std::string& f : hot::host_ms_runtime_lib_flags(std::string()))
        flags.push_back(f);
    for (const std::string& d : paths.include_dirs)
        flags.push_back("-I" + d);
    if (!paths.resource_dir.empty())
        flags.push_back("-resource-dir=" + paths.resource_dir);
    flags.push_back(path);
    std::vector<const char*> args;
    for (const std::string& f : flags)
        args.push_back(f.c_str());
    const char* file_paths[] = {path.c_str()};
    const char* texts[] = {source};
    hot_jit_ir* ir = nullptr;
    char* diagnostics = nullptr;
    const int compiled = hot_jit_compile(args.data(), args.size(), file_paths, texts, 1, &ir, &diagnostics);
    const std::string messages = take_text(diagnostics);
    if (!compiled)
        return std::string(file) + " does not compile: " + messages;
    char* error = nullptr;
    char* debugger_error = nullptr;
    int debugger_on = 0;
    const std::vector<const void*> host_runtime = hot::jit::host_runtime_addresses();
    probe.engine =
        hot_jit_engine_create(0, host_runtime.data(), host_runtime.size(), &debugger_on, &error, &debugger_error);
    take_text(debugger_error);
    if (!probe.engine)
    {
        hot_jit_ir_free(ir);
        return "no JIT: " + take_text(error);
    }
    std::vector<const char*> names;
    std::vector<uint64_t> values;
    hot::jit::add_msvc_host_symbols(names, values);
    probe.dylib = hot_jit_add(probe.engine, file, ir, names.data(), values.data(), names.size(), &error);
    if (!probe.dylib)
        return std::string(file) + " does not link: " + take_text(error);
    return "";
}

std::string probe_host_runtime()
{
    JitProbe probe;
    if (std::string error = make_probe("probe.cpp", probe_source(), probe); !error.empty())
        return error;
    hot_jit_dylib* dylib = probe.dylib;
    char* error = nullptr;
    auto take = take_text;
    std::string differ;
    auto look = [&](const char* function) -> void* {
        void* f = hot_jit_lookup(dylib, function, &error);
        if (!f)
            differ += std::string(function) + " (" + take(error) + ") ";
        return f;
    };
    auto compare = [&](const std::string& what, const void* jit, const void* host) {
        if (jit == host)
            return;
        std::ostringstream os;
        os << what << " (JIT " << jit << " in " << module_of(jit) << ", host " << host << " in " << module_of(host)
           << ") ";
        differ += os.str();
    };

    struct HostValue
    {
        const char* name;
        const void* address;
    };
#define HOT_PROBE_HOST(n, e) {n, reinterpret_cast<const void*>(e)},
    const HostValue host_values[] = {HOT_JIT_HOST_OPERATORS(HOT_PROBE_HOST) HOT_JIT_HOST_C_HEAP(HOT_PROBE_HOST)
            HOT_JIT_HOST_MSVC_HEAP(HOT_PROBE_HOST)};
#undef HOT_PROBE_HOST
    static_assert(std::size(host_values) == std::size(hot::jit::kHostAllocatorNames));
    if (void* f = look("probe_symbol"))
        for (int i = 0; i < static_cast<int>(std::size(host_values)); ++i)
            compare(host_values[i].name, reinterpret_cast<const void* (*)(int)>(f)(i), host_values[i].address);
    if (void* f = look("probe_epoch"))
        compare("_Init_thread_epoch", reinterpret_cast<const void* (*)()>(f)(), &_Init_thread_epoch);
    if (void* f = look("probe_type_info_root"))
        compare("__type_info_root_node", reinterpret_cast<const void* (*)()>(f)(), &__type_info_root_node);

    struct HostLayout
    {
        const char* type;
        std::size_t size;
        std::size_t align;
    };
#define HOT_PROBE_HOST_LAYOUT(t) {#t, sizeof(t), alignof(t)},
    const HostLayout layouts[] = {HOT_PROBE_TYPES(HOT_PROBE_HOST_LAYOUT)};
#undef HOT_PROBE_HOST_LAYOUT
    if (void* f = look("probe_layout"))
    {
        auto layout = reinterpret_cast<std::size_t (*)(int)>(f);
        for (int i = 0; i < static_cast<int>(std::size(layouts)); ++i)
            if (layout(2 * i) != layouts[i].size || layout(2 * i + 1) != layouts[i].align)
            {
                std::ostringstream os;
                os << layouts[i].type << " (JIT size " << layout(2 * i) << " align " << layout(2 * i + 1)
                   << ", host size " << layouts[i].size << " align " << layouts[i].align << ") ";
                differ += os.str();
            }
    }

    // The JIT code writes to the stdout of the host. Another C runtime has another stdio state.
    const std::size_t before_stdio = differ.size();
#define HOT_PROBE_HOST_STDIO(n) {#n, reinterpret_cast<const void*>(&n)},
    const HostValue host_stdio[] = {HOT_PROBE_STDIO(HOT_PROBE_HOST_STDIO)};
#undef HOT_PROBE_HOST_STDIO
    if (void* f = look("probe_stdio"))
        for (int i = 0; i < static_cast<int>(std::size(host_stdio)); ++i)
            compare(host_stdio[i].name, reinterpret_cast<const void* (*)(int)>(f)(i), host_stdio[i].address);
    if (void* f = look("probe_stdout"))
        compare("stdout (__acrt_iob_func(1))", reinterpret_cast<const void* (*)()>(f)(), stdout);
    // A write through a FILE of another C runtime can break the stdout of the host, so the write
    // runs only when each stdio address is the host address.
    void* jit_write = look("probe_write");
    if (jit_write && differ.size() == before_stdio)
    {
        testing::internal::CaptureStdout();
        reinterpret_cast<void (*)(int)>(jit_write)(5);
        const std::string out = testing::internal::GetCapturedStdout();
        for (const char* line : {"probe printf 5\n", "probe fputs\n", "probe cout 5\n"})
            if (out.find(line) == std::string::npos)
                differ += "the host capture of stdout has no \"" + std::string(line, std::strlen(line) - 1)
                    + "\" (it has \"" + out + "\") ";
    }

    // A block from another heap can stop the process, so the cross checks run only when each
    // address is the host address.
    if (differ.empty())
    {
        void* jit_new = look("probe_new");
        void* jit_delete = look("probe_delete");
        void* jit_malloc = look("probe_malloc");
        void* jit_aligned_malloc = look("probe_aligned_malloc");
        if (jit_new && jit_delete && jit_malloc && jit_aligned_malloc)
        {
            int* a = static_cast<int*>(reinterpret_cast<void* (*)()>(jit_new)());
            if (!a || *a != 7)
                differ += "the JIT new gives a bad block ";
            delete a;
            reinterpret_cast<void (*)(void*)>(jit_delete)(new int(9));
            std::free(reinterpret_cast<void* (*)()>(jit_malloc)());
            void* b = reinterpret_cast<void* (*)()>(jit_aligned_malloc)();
            if (reinterpret_cast<std::uintptr_t>(b) % 64 != 0)
                differ += "the JIT _aligned_malloc gives a bad block ";
            _aligned_free(b);
        }
    }
    return differ;
}

// The exception probe: the JIT code throws and catches C++ exceptions in the JIT code, from the host
// and to the host. Each function gives 1 when its case works.
const char* eh_probe_source()
{
    return "#include <cstring>\n#include <exception>\n#include <stdexcept>\n" HOT_EH_TEXT(HOT_EH_PROBE_TYPES) R"(
extern "C" int eh_local(int k)
{
    try
    {
        switch (k)
        {
        case 0: throw 7;
        case 1: throw std::runtime_error("jit runtime_error");
        case 2: throw ProbeJitError{3};
        case 3:
            try { throw ProbeJitError{4}; }
            catch (...) { throw; }
        case 4:
            try { throw 5; }
            catch (int) { throw std::logic_error("nested"); }
        case 5: throw ProbeHostError(6);
        }
    }
    catch (int v) { return k == 0 && v == 7; }
    catch (const std::runtime_error& e) { return k == 1 && std::strcmp(e.what(), "jit runtime_error") == 0; }
    catch (const std::logic_error& e) { return k == 4 && std::strcmp(e.what(), "nested") == 0; }
    catch (const ProbeJitError& e) { return (k == 2 && e.code == 3) || (k == 3 && e.code == 4); }
    catch (const ProbeHostError& e) { return k == 5 && e.code == 6; }
    catch (...) { return 0; }
    return 0;
}
extern "C" int eh_current()
{
    try { throw std::runtime_error("current"); }
    catch (...)
    {
        std::exception_ptr p = std::current_exception();
        if (!p)
            return 0;
        try { std::rethrow_exception(p); }
        catch (const std::runtime_error& e) { return std::strcmp(e.what(), "current") == 0; }
        catch (...) { return 0; }
    }
    return 0;
}
extern "C" int eh_uncaught()
{
    struct Check { int* out; ~Check() { *out = std::uncaught_exceptions(); } };
    int seen = -1;
    try { Check c{&seen}; throw 1; }
    catch (...) {}
    return seen == 1;
}
extern "C" int eh_catch_host(void (*host_throw)(int), int k)
{
    try { host_throw(k); }
    catch (int v) { return k == 0 && v == 11; }
    catch (const std::runtime_error& e) { return k == 1 && std::strcmp(e.what(), "host runtime_error") == 0; }
    catch (const ProbeHostError& e) { return k == 2 && e.code == 12; }
    catch (...) { return 0; }
    return 0;
}
extern "C" int eh_current_from_host(void (*host_throw)(int))
{
    try { host_throw(1); }
    catch (...) { return std::current_exception() != nullptr; }
    return 0;
}
extern "C" void eh_throw(int k)
{
    switch (k)
    {
    case 0: throw 21;
    case 1: throw std::runtime_error("jit to host");
    case 2: throw ProbeHostError(22);
    case 3: throw ProbeJitError{23};
    case 4:
        try { throw std::runtime_error("rethrow to host"); }
        catch (...) { throw; }
    case 5:
        try { throw 24; }
        catch (...) { std::rethrow_exception(std::current_exception()); }
    }
}
struct Mark { int* p; ~Mark() { ++*p; } };
extern "C" void eh_pass(void (*host_throw)(int), int* destructors)
{
    Mark m{destructors};
    host_throw(1);
}
)";
}

void eh_host_throw(int k)
{
    switch (k)
    {
    case 0: throw 11;
    case 1: throw std::runtime_error("host runtime_error");
    case 2: throw ProbeHostError(12);
    }
}

// Runs each case of the exception probe and gives the names of the cases that fail. Each case
// writes its name before it runs: a case that stops the process is then the last name in the log.
std::string probe_exceptions()
{
    JitProbe probe;
    if (std::string error = make_probe("eh_probe.cpp", eh_probe_source(), probe); !error.empty())
        return error;
    std::string failed;
    char* error = nullptr;
    auto look = [&](const char* function) -> void* {
        void* f = hot_jit_lookup(probe.dylib, function, &error);
        if (!f)
            failed += std::string(function) + " (" + take_text(error) + ") ";
        return f;
    };
    auto check = [&](const std::string& name, auto&& run) {
        std::fprintf(stderr, "[eh-probe] %s\n", name.c_str());
        std::fflush(stderr);
        bool ok = false;
        try
        {
            ok = run();
        }
        catch (...)
        {
            ok = false;
        }
        if (!ok)
            failed += name + " ";
    };
    using HostThrow = void (*)(int);
    if (auto f = reinterpret_cast<int (*)(int)>(look("eh_local")))
        for (int k : {0, 1, 2, 3, 4, 5})
            check("jit-throw-jit-catch-" + std::to_string(k), [&] { return f(k) == 1; });
    if (auto f = reinterpret_cast<int (*)()>(look("eh_current")))
        check("jit-current-exception", [&] { return f() == 1; });
    if (auto f = reinterpret_cast<int (*)()>(look("eh_uncaught")))
        check("jit-uncaught-exceptions", [&] { return f() == 1; });
    if (auto f = reinterpret_cast<int (*)(HostThrow, int)>(look("eh_catch_host")))
        for (int k : {0, 1, 2})
            check("host-throw-jit-catch-" + std::to_string(k), [&] { return f(&eh_host_throw, k) == 1; });
    if (auto f = reinterpret_cast<int (*)(HostThrow)>(look("eh_current_from_host")))
        check("host-throw-jit-current-exception", [&] { return f(&eh_host_throw) == 1; });
    if (auto f = reinterpret_cast<void (*)(int)>(look("eh_throw")))
    {
        check("jit-throw-host-catch-int", [&] {
            try { f(0); }
            catch (int v) { return v == 21; }
            return false;
        });
        check("jit-throw-host-catch-runtime-error", [&] {
            try { f(1); }
            catch (const std::runtime_error& e) { return std::string(e.what()) == "jit to host"; }
            return false;
        });
        check("jit-throw-host-catch-host-class", [&] {
            try { f(2); }
            catch (const ProbeHostError& e) { return e.code == 22; }
            return false;
        });
        check("jit-throw-host-catch-jit-class", [&] {
            try { f(3); }
            catch (const ProbeJitError& e) { return e.code == 23; }
            return false;
        });
        check("jit-rethrow-host-catch", [&] {
            try { f(4); }
            catch (const std::runtime_error& e) { return std::string(e.what()) == "rethrow to host"; }
            return false;
        });
        check("jit-rethrow-exception-host-catch", [&] {
            try { f(5); }
            catch (int v) { return v == 24; }
            return false;
        });
        check("jit-throw-host-current-exception", [&] {
            try { f(1); }
            catch (...) { return std::current_exception() != nullptr; }
            return false;
        });
    }
    if (auto f = reinterpret_cast<void (*)(HostThrow, int*)>(look("eh_pass")))
        check("host-throw-through-jit-frame", [&] {
            int destructors = 0;
            try { f(&eh_host_throw, &destructors); }
            catch (const std::runtime_error&) { return destructors == 1; }
            return false;
        });
    return failed;
}
#endif

// Windows: the JIT code sees the allocator family, the runtime state and the type layouts of the
// host. A block can go from the JIT code to the host and back.
TEST(JitTests, JitCodeSeesTheHostRuntime)
{
#ifndef _MSC_VER
    GTEST_SKIP() << "Windows only";
#else
    EXPECT_EQ(probe_host_runtime(), "");
#endif
}

// Windows: C++ exceptions work in the JIT code, from the host to the JIT code and from the JIT code
// to the host. The text names each case that fails.
TEST(JitTests, JitCodeThrowsAndCatchesCxxExceptions)
{
#ifndef _MSC_VER
    GTEST_SKIP() << "Windows only";
#else
    EXPECT_EQ(probe_exceptions(), "");
#endif
}

// The engine searches the C runtime DLLs of the host first. The host gives one address in each of
// them: ucrtbase, vcruntime and msvcp (debug: ucrtbased, vcruntime140d, msvcp140d), then the
// other loaded parts (vcruntime140_1, for example). Other compilers give no address, because the
// process search has only one C runtime there.
TEST(JitTests, HostGivesEachRuntimeDll)
{
    const std::vector<const void*> addresses = hot::jit::host_runtime_addresses();
#if !defined(_MSC_VER) || !defined(_DLL)
    // A /MT host has no runtime DLL.
    EXPECT_TRUE(addresses.empty());
#else
    // The first three are always there. The other parts of the runtime follow when they are loaded.
    ASSERT_GE(addresses.size(), 3u);
    const char* const dlls[] = {"ucrtbase", "vcruntime", "msvcp"};
    for (size_t i = 0; i < addresses.size(); ++i)
    {
        std::string name = module_of(addresses[i]);
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (i < 3)
            EXPECT_NE(name.find(dlls[i]), std::string::npos) << name;
        EXPECT_TRUE(hot::jit::is_c_runtime_dll(name)) << name;
    }
#endif
}

// The search of the JIT code knows the DLLs of each C or C++ runtime by the file name.
TEST(JitTests, CRuntimeDllNames)
{
    for (const char* dll : {"C:\\Windows\\System32\\msvcrt.dll", "C:/Windows/System32/MSVCP_WIN.dll", "ucrtbase.dll",
             "ucrtbased.dll", "VCRUNTIME140_1.dll", "msvcp140d.dll", "msvcr120.dll", "api-ms-win-crt-stdio-l1-1-0.dll",
             "concrt140.dll"})
        EXPECT_TRUE(hot::jit::is_c_runtime_dll(dll)) << dll;
    for (const char* other : {"C:\\Windows\\System32\\ntdll.dll", "KERNEL32.DLL", "integration_reload_tests.exe",
             "hot_jit_engine.dll", "msvcrt.lib", "api-ms-win-core-synch-l1-2-0.dll", ""})
        EXPECT_FALSE(hot::jit::is_c_runtime_dll(other)) << other;
}

// The runtime DLLs of the host come first, in the order of the host. The other modules follow in
// the load order. Another C runtime (msvcrt.dll, msvcp_win.dll) is not in the search.
TEST(JitTests, SymbolSearchOrderPutsTheHostRuntimeFirst)
{
    const std::vector<std::string> modules = {"D:\\a\\tests.exe", "C:\\Windows\\SYSTEM32\\ntdll.dll",
        "C:\\Windows\\System32\\KERNEL32.DLL", "C:\\Windows\\System32\\msvcrt.dll",
        "C:\\Windows\\System32\\ucrtbase.dll", "C:\\Windows\\SYSTEM32\\VCRUNTIME140.dll",
        "C:\\Windows\\System32\\msvcp_win.dll", "C:\\Windows\\SYSTEM32\\MSVCP140.dll",
        "C:\\Windows\\System32\\USER32.dll", "C:\\Windows\\SYSTEM32\\ucrtbased.dll"};
    // A host index can come two times or be out of range: the result has it one time or not.
    // vcruntime comes before msvcp, and msvcp before ucrtbase, in each order of the host list.
    const std::vector<std::size_t> order = hot::jit::symbol_search_order(modules, {4, 5, 7, 4, 99});
    EXPECT_EQ(order, (std::vector<std::size_t>{5, 7, 4, 0, 1, 2, 8}));
    EXPECT_EQ(hot::jit::symbol_search_order(modules, {7, 4, 5}), order);
    // Without a host list, no C runtime DLL is in the search.
    EXPECT_EQ(hot::jit::symbol_search_order(modules, {}), (std::vector<std::size_t>{0, 1, 2, 8}));
}

// ucrtbase also exports the C++ exception names of vcruntime (__CxxFrameHandler3,
// __current_exception), with another exception state. The JIT code must get these names from
// vcruntime, the same as the host: else std::current_exception() in a JIT catch gives null.
TEST(JitTests, SymbolSearchOrderPutsVcruntimeBeforeUcrtbase)
{
    const std::vector<std::string> modules = {"C:\\Windows\\System32\\ucrtbase.dll",
        "C:\\Windows\\SYSTEM32\\MSVCP140.dll", "C:\\Windows\\SYSTEM32\\concrt140.dll",
        "C:\\Windows\\SYSTEM32\\VCRUNTIME140_1.dll", "C:\\Windows\\SYSTEM32\\VCRUNTIME140.dll",
        "C:\\Windows\\SYSTEM32\\msvcp140_1.dll", "D:\\a\\engine.dll"};
    EXPECT_EQ(hot::jit::host_runtime_rank(modules[4]), 0);
    EXPECT_EQ(hot::jit::host_runtime_rank(modules[3]), 0);
    EXPECT_EQ(hot::jit::host_runtime_rank(modules[1]), 1);
    EXPECT_EQ(hot::jit::host_runtime_rank(modules[2]), 1);
    EXPECT_EQ(hot::jit::host_runtime_rank(modules[0]), 2);
    EXPECT_EQ(hot::jit::host_runtime_rank(modules[6]), 3);
    // The host list of jit::host_runtime_addresses: ucrtbase, vcruntime, msvcp, then the others.
    EXPECT_EQ(hot::jit::symbol_search_order(modules, {0, 4, 1, 3, 5, 2}),
        (std::vector<std::size_t>{4, 3, 1, 5, 2, 0, 6}));
}

// The host gives each name of the allocator family that this platform has, one time, with an
// address. The engine filter knows the same names, and only these names.
TEST(JitTests, HostGivesTheWholeAllocatorFamily)
{
    std::vector<const char*> names;
    std::vector<uint64_t> values;
    hot::jit::add_host_allocator_symbols(names, values);
    ASSERT_EQ(names.size(), values.size());
#ifdef _MSC_VER
    const bool msvc = true;
#else
    const bool msvc = false;
#endif
    size_t expected = 0;
    for (const hot::jit::HostAllocatorName& a : hot::jit::kHostAllocatorNames)
    {
        if (a.msvc_only && !msvc)
            continue;
        ++expected;
        size_t found = 0;
        for (size_t i = 0; i < names.size(); ++i)
            if (std::string(names[i]) == a.name)
            {
                ++found;
                EXPECT_NE(values[i], 0u) << a.name;
            }
        EXPECT_EQ(found, 1u) << a.name;
        EXPECT_TRUE(hot::jit::is_host_allocator_name(a.name, std::strlen(a.name))) << a.name;
    }
    EXPECT_EQ(names.size(), expected);
    EXPECT_GE(expected, 24u);
    EXPECT_FALSE(hot::jit::is_host_allocator_name("mallo", 5));
    EXPECT_FALSE(hot::jit::is_host_allocator_name("malloc_usable_size", 18));
    EXPECT_FALSE(hot::jit::is_host_allocator_name("strdup", 6));
}

} // namespace
