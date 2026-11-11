// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The object of a module, kept as bytes: the first engine makes the object from the IR
// (hot_jit_capture_objects, hot_jit_dylib_object). A new engine loads the bytes
// (hot_jit_add_object). The behavior must be the same as for a load of the IR: the static
// initializers run one time, the exceptions work, the static destructors run at hot_jit_remove, the
// host symbols and the absolute symbols resolve in the new engine, and the debugger sees the object.

#include "support/dwarf_lines.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"

#include "jit_engine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#if !defined(_WIN32)

extern "C"
{
    // The GDB JIT interface (the layout is fixed by the GDB manual). LLVM defines the descriptor.
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

    // The host functions of the script. The test executable exports them (ENABLE_EXPORTS).
    __attribute__((visibility("default"))) int hot_s1_events[8];
    __attribute__((visibility("default"))) void hot_s1_note(int what)
    {
        ++hot_s1_events[what];
    }
    __attribute__((visibility("default"))) int hot_s1_host_twice(int x)
    {
        return 2 * x;
    }
}

namespace
{

    #if defined(__x86_64__) || defined(__aarch64__)
        #define HOT_S1_TLS 1
// Two thread variables of the host. Each engine gets the offset of one of them as the absolute
// symbol "hot_s1_tp_offset", the same as hot_call_depth_tp_offset.
thread_local uint32_t s1_tls_a = 0;
thread_local uint32_t s1_tls_b = 0;

uint64_t tp_offset(uint32_t& v)
{
    return static_cast<uint64_t>(reinterpret_cast<char*>(&v) - static_cast<char*>(__builtin_thread_pointer()));
}
    #else
        #define HOT_S1_TLS 0
    #endif

const char* const s1_path = "/hot-jit-object-test/s1.cpp";

const char* const s1_source = R"(#include <cstdint>
#include <stdexcept>
extern "C" void hot_s1_note(int what);
extern "C" int hot_s1_host_twice(int x);
extern "C" char hot_s1_tp_offset[];
namespace
{
struct Global
{
    int value;
    Global() : value(hot_s1_host_twice(21)) { hot_s1_note(1); }
    ~Global() { hot_s1_note(2); }
};
Global g_global;
struct Local
{
    Local() { hot_s1_note(3); }
    ~Local() { hot_s1_note(4); }
    int hits = 0;
};
struct ScriptError
{
    int code;
};
}
__attribute__((noinline)) int s1_throw(int code)
{
    if (code > 0)
        throw ScriptError{code};
    return 0;
}
__attribute__((noinline)) int s1_throw_std(int code)
{
    if (code > 0)
        throw std::runtime_error("s1");
    return 0;
}
extern "C" int s1_catch(int code)
{
    try { s1_throw(code); } catch (const ScriptError& e) { return e.code; } // catch line
    return -1;
}
extern "C" int s1_catch_std(int code)
{
    try { s1_throw_std(code); } catch (const std::exception& e) { return e.what()[0] == 's' ? code : -2; }
    return -1;
}
extern "C" int s1_local_hits() { static Local local; return ++local.hits; }
extern "C" int s1_global_value() { return g_global.value; }
extern "C" const void* s1_global_address() { return &g_global; }
extern "C" int s1_host(int x) { return hot_s1_host_twice(x) + 1; }
extern "C" std::intptr_t s1_offset() { return reinterpret_cast<std::intptr_t>(hot_s1_tp_offset); }
extern "C" void s1_tls_add(unsigned n)
{
    *reinterpret_cast<unsigned*>(static_cast<char*>(__builtin_thread_pointer())
        + reinterpret_cast<std::intptr_t>(hot_s1_tp_offset)) += n;
}
)";

std::string take_text(char* text)
{
    std::string out = text ? text : "";
    hot_jit_string_free(text);
    return out;
}

// Compiles the script with the flags of the backend (-O1 -fPIC -gline-tables-only).
hot_jit_ir* compile_s1()
{
    std::vector<std::string> flags = {HOT_CXX, "-std=c++20", "-O1", "-fPIC", "-gline-tables-only", "-fexceptions",
        "-fcxx-exceptions", "-resource-dir=" HOT_RESOURCE_DIR, s1_path};
    std::vector<const char*> args;
    for (const std::string& f : flags)
        args.push_back(f.c_str());
    const char* paths[] = {s1_path};
    const char* texts[] = {s1_source};
    hot_jit_ir* ir = nullptr;
    char* diagnostics = nullptr;
    const int ok = hot_jit_compile(args.data(), args.size(), paths, texts, 1, &ir, &diagnostics);
    const std::string messages = take_text(diagnostics);
    EXPECT_EQ(ok, 1) << messages;
    return ok ? ir : nullptr;
}

hot_jit_engine* make_engine(int debugger, int* debugger_on = nullptr)
{
    char* error = nullptr;
    char* debugger_error = nullptr;
    int on = 0;
    hot_jit_engine* engine = hot_jit_engine_create(debugger, nullptr, 0, &on, &error, &debugger_error);
    EXPECT_NE(engine, nullptr) << take_text(error);
    take_text(debugger_error);
    if (debugger_on)
        *debugger_on = on;
    return engine;
}

template <class F>
F* find(hot_jit_dylib* dylib, const char* name)
{
    char* error = nullptr;
    void* p = hot_jit_lookup(dylib, name, &error);
    EXPECT_NE(p, nullptr) << name << ": " << take_text(error);
    return reinterpret_cast<F*>(p);
}

void reset_events()
{
    for (int& e : hot_s1_events)
        e = 0;
}

// The checks of a loaded dylib of the script. "offset" is the value of hot_s1_tp_offset.
void check_s1(hot_jit_dylib* dylib, uint64_t offset)
{
    ASSERT_NE(dylib, nullptr);
    EXPECT_EQ(find<int(int)>(dylib, "s1_catch")(7), 7);
    EXPECT_EQ(find<int(int)>(dylib, "s1_catch_std")(9), 9);
    EXPECT_EQ(find<int()>(dylib, "s1_global_value")(), 42);
    EXPECT_EQ(find<int(int)>(dylib, "s1_host")(5), 11);
    EXPECT_EQ(find<int()>(dylib, "s1_local_hits")(), 1);
    EXPECT_EQ(find<int()>(dylib, "s1_local_hits")(), 2);
    EXPECT_EQ(static_cast<uint64_t>(find<intptr_t()>(dylib, "s1_offset")()), offset);
}

// The objects of the GDB JIT interface now.
std::set<const jit_code_entry*> debug_entries()
{
    std::set<const jit_code_entry*> out;
    for (const jit_code_entry* e = __jit_debug_descriptor.first_entry; e; e = e->next_entry)
        out.insert(e);
    return out;
}

// One of the objects that are not in "before" has the lines of the script. LLVM puts a new object
// at the head of the list. The other new objects come from the platform code of LLJIT.
void check_debugger_has_s1(const std::set<const jit_code_entry*>& before)
{
    test_support::TestScratch scratch;
    const std::string image = scratch.file("jit_object.o");
    const int line = test_support::line_with(s1_source, "// catch line");
    std::string seen;
    for (const jit_code_entry* e = __jit_debug_descriptor.first_entry; e; e = e->next_entry)
    {
        if (before.count(e))
            continue;
        {
            std::ofstream f(image, std::ios::binary | std::ios::trunc);
            f.write(e->symfile_addr, static_cast<std::streamsize>(e->symfile_size));
        }
        const std::set<int> lines = test_support::lines_of_file(test_support::dump_line_table(image), "s1.cpp");
        if (lines.count(line))
            return;
        seen += " " + std::to_string(e->symfile_size) + ":" + std::to_string(lines.size());
    }
    ADD_FAILURE() << "no new object has the line " << line << " of s1.cpp; new objects (size:lines):" << seen;
}

} // namespace

// The object of the first engine loads in a new engine with the same behavior. The first engine
// is gone before the load.
TEST(JitObjectTests, ObjectLoadsInANewEngine)
{
    #if HOT_S1_TLS
    const uint64_t offset_a = tp_offset(s1_tls_a);
    const uint64_t offset_b = tp_offset(s1_tls_b);
    #else
    const uint64_t offset_a = 0x1000, offset_b = 0x2000;
    #endif
    ASSERT_NE(offset_a, offset_b);
    const char* abs_names[] = {"hot_s1_tp_offset"};

    // The load of the IR in the first engine, with the capture of the object.
    std::vector<char> object;
    {
        reset_events();
        hot_jit_engine* first = make_engine(0);
        ASSERT_NE(first, nullptr);
        hot_jit_capture_objects(first, 1);
        hot_jit_ir* ir = compile_s1();
        ASSERT_NE(ir, nullptr);
        char* error = nullptr;
        hot_jit_dylib* dylib = hot_jit_add(first, "s1", ir, abs_names, &offset_a, 1, &error);
        ASSERT_NE(dylib, nullptr) << take_text(error);
        EXPECT_EQ(hot_s1_events[1], 1);
        check_s1(dylib, offset_a);
    #if HOT_S1_TLS
        s1_tls_a = 0;
        find<void(unsigned)>(dylib, "s1_tls_add")(3);
        EXPECT_EQ(s1_tls_a, 3u);
    #endif
        const void* data = nullptr;
        size_t size = 0;
        ASSERT_EQ(hot_jit_dylib_object(dylib, &data, &size), 1);
        ASSERT_GT(size, 0u);
        object.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
        hot_jit_remove(dylib);
        EXPECT_EQ(hot_s1_events[1], 1);
        EXPECT_EQ(hot_s1_events[2], 1);
        EXPECT_EQ(hot_s1_events[3], 1);
        EXPECT_EQ(hot_s1_events[4], 1);
        hot_jit_engine_free(first);
    }

    // The load of the bytes in a new engine with another offset.
    reset_events();
    int debugger_on = 0;
    hot_jit_engine* second = make_engine(1, &debugger_on);
    ASSERT_NE(second, nullptr);
    const std::set<const jit_code_entry*> entries_before = debug_entries();
    char* error = nullptr;
    hot_jit_dylib* dylib = hot_jit_add_object(second, "s1b", object.data(), object.size(), abs_names, &offset_b, 1, &error);
    ASSERT_NE(dylib, nullptr) << take_text(error);
    EXPECT_EQ(hot_s1_events[1], 1) << "the initializer must run exactly one time";
    EXPECT_EQ(hot_jit_dylib_object(dylib, nullptr, nullptr), 0);
    #if defined(__linux__)
    EXPECT_EQ(debugger_on, 1);
    #endif
    #if !defined(__APPLE__)
    if (debugger_on && test_support::have_dwarfdump())
        check_debugger_has_s1(entries_before);
    #endif
    check_s1(dylib, offset_b);
    #if HOT_S1_TLS
    s1_tls_a = 0;
    s1_tls_b = 0;
    find<void(unsigned)>(dylib, "s1_tls_add")(5);
    EXPECT_EQ(s1_tls_a, 0u);
    EXPECT_EQ(s1_tls_b, 5u);
    #endif

    // The core instance: the same bytes a second time, in another dylib of the same engine.
    hot_jit_dylib* core = hot_jit_add_object(second, "s1b.core", object.data(), object.size(), abs_names, &offset_b, 1, &error);
    ASSERT_NE(core, nullptr) << take_text(error);
    EXPECT_EQ(hot_s1_events[1], 2);
    EXPECT_NE(find<const void*()>(core, "s1_global_address")(), find<const void*()>(dylib, "s1_global_address")());
    check_s1(core, offset_b);
    EXPECT_EQ(hot_s1_events[1], 2) << "a lookup must not run the initializer again";
    EXPECT_EQ(hot_s1_events[3], 2);

    hot_jit_remove(core);
    EXPECT_EQ(hot_s1_events[2], 1);
    EXPECT_EQ(hot_s1_events[4], 1);
    hot_jit_remove(dylib);
    EXPECT_EQ(hot_s1_events[2], 2);
    EXPECT_EQ(hot_s1_events[4], 2);
    hot_jit_engine_free(second);
}

// An engine without the capture keeps no object.
TEST(JitObjectTests, NoObjectWithoutCapture)
{
    hot_jit_engine* engine = make_engine(0);
    ASSERT_NE(engine, nullptr);
    hot_jit_ir* ir = compile_s1();
    ASSERT_NE(ir, nullptr);
    const char* abs_names[] = {"hot_s1_tp_offset"};
    const uint64_t offset = 0;
    char* error = nullptr;
    reset_events();
    hot_jit_dylib* dylib = hot_jit_add(engine, "s1", ir, abs_names, &offset, 1, &error);
    ASSERT_NE(dylib, nullptr) << take_text(error);
    EXPECT_EQ(hot_jit_dylib_object(dylib, nullptr, nullptr), 0);
    hot_jit_remove(dylib);
    hot_jit_engine_free(engine);
}

// A bad object gives an error, not a crash.
TEST(JitObjectTests, BadObjectIsAnError)
{
    hot_jit_engine* engine = make_engine(0);
    ASSERT_NE(engine, nullptr);
    const char junk[] = "not an object";
    char* error = nullptr;
    hot_jit_dylib* dylib = hot_jit_add_object(engine, "junk", junk, sizeof junk, nullptr, nullptr, 0, &error);
    EXPECT_EQ(dylib, nullptr);
    EXPECT_FALSE(take_text(error).empty());
    hot_jit_engine_free(engine);
}

#endif // !_WIN32
