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
#include "jit_msvc_symbols.h"

#ifdef _MSC_VER
    #include <malloc.h>
    #include <typeinfo>
// The epoch of the local statics of the thread (jit_msvc_symbols.cpp).
extern "C" __declspec(thread) int _Init_thread_epoch;
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

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
// A probe in JIT code gives the address of a C runtime symbol and of runtime state as the JIT
// code sees it. These must be the addresses of the host: else the JIT code uses another heap or
// another state. The text names each symbol that differs.
std::string probe_host_symbols()
{
    const std::string source = "typedef decltype(sizeof(0)) size_t;\n"
                               "extern \"C\" __declspec(dllimport) void* malloc(size_t);\n"
                               "extern \"C\" __declspec(dllimport) void free(void*);\n"
                               "extern \"C\" __declspec(dllimport) void* _aligned_malloc(size_t, size_t);\n"
                               "extern \"C\" __declspec(dllimport) void _aligned_free(void*);\n"
                               "extern \"C\" __declspec(thread) int _Init_thread_epoch;\n"
                               "struct __type_info_node;\n"
                               "extern __type_info_node __type_info_root_node;\n"
                               "extern \"C\" const void* probe_malloc() { return (const void*)&malloc; }\n"
                               "extern \"C\" const void* probe_free() { return (const void*)&free; }\n"
                               "extern \"C\" const void* probe_aligned_malloc() { return (const void*)&_aligned_malloc; }\n"
                               "extern \"C\" const void* probe_aligned_free() { return (const void*)&_aligned_free; }\n"
                               "extern \"C\" const void* probe_epoch() { return &_Init_thread_epoch; }\n"
                               "extern \"C\" const void* probe_type_info_root() { return &__type_info_root_node; }\n";
    hot_jit_ir* ir = compile_ir("", source);
    if (!ir)
        return "the probe does not compile";
    auto take = [](char* text) {
        std::string out = text ? text : "";
        hot_jit_string_free(text);
        return out;
    };
    char* error = nullptr;
    char* debugger_error = nullptr;
    int debugger_on = 0;
    hot_jit_engine* engine = hot_jit_engine_create(0, &debugger_on, &error, &debugger_error);
    take(debugger_error);
    if (!engine)
    {
        hot_jit_ir_free(ir);
        return "no JIT: " + take(error);
    }
    std::vector<const char*> names;
    std::vector<uint64_t> values;
    hot::jit::add_msvc_host_symbols(names, values);
    hot_jit_dylib* dylib = hot_jit_add(engine, "probe", ir, names.data(), values.data(), names.size(), &error);
    if (!dylib)
    {
        hot_jit_engine_free(engine);
        return "the probe does not link: " + take(error);
    }
    struct Probe
    {
        const char* function;
        const void* host;
    };
    const Probe probes[] = {
        {"probe_malloc", reinterpret_cast<const void*>(&malloc)},
        {"probe_free", reinterpret_cast<const void*>(&free)},
        {"probe_aligned_malloc", reinterpret_cast<const void*>(&_aligned_malloc)},
        {"probe_aligned_free", reinterpret_cast<const void*>(&_aligned_free)},
        {"probe_epoch", &_Init_thread_epoch},
        {"probe_type_info_root", &__type_info_root_node},
    };
    std::string differ;
    for (const Probe& p : probes)
    {
        void* f = hot_jit_lookup(dylib, p.function, &error);
        if (!f)
        {
            differ += std::string(p.function) + " (" + take(error) + ") ";
            continue;
        }
        const void* jit = reinterpret_cast<const void* (*)()>(f)();
        if (jit != p.host)
        {
            std::ostringstream os;
            os << p.function << " (JIT " << jit << ", host " << p.host << ") ";
            differ += os.str();
        }
    }
    hot_jit_remove(dylib);
    hot_jit_engine_free(engine);
    return differ;
}
#endif

// Windows: the JIT code sees the C runtime functions and the runtime state of the host.
TEST(JitTests, JitCodeSeesTheHostRuntime)
{
#ifndef _MSC_VER
    GTEST_SKIP() << "Windows only";
#else
    EXPECT_EQ(probe_host_symbols(), "");
#endif
}

} // namespace
