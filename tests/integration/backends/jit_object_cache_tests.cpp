// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The object cache of the JIT backend (JitOptions::cache_dir, "<cache_dir>/jit"). A load with a
// hit does no Clang compile and no code generation. The module of a hit must behave as the
// module of a compile: the static initializers run one time, an exception passes the JIT code,
// the static destructors run at the unload and the debugger sees the code.
#include "hot/jit_backend.h"
#include "jit_engine.h"
#include "support/dwarf_lines.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The object cache is on Linux only (jit_backend.cpp).
#if !defined(_WIN32) && !defined(__APPLE__)

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

    // The host functions of the header code. The test executable exports them (ENABLE_EXPORTS).
    __attribute__((visibility("default"))) int hot_oc_events[4];
    __attribute__((visibility("default"))) int (*hot_oc_probe_fn)(int) = nullptr;
    __attribute__((visibility("default"))) void hot_oc_note(int what)
    {
        ++hot_oc_events[what];
    }
    __attribute__((visibility("default"))) void hot_oc_register(int (*fn)(int))
    {
        hot_oc_probe_fn = fn;
    }
}

namespace
{

namespace fs = std::filesystem;

// The header code goes into the JIT object: a static initializer (event 1) that gives the host
// a function with a throw and a catch, and a static destructor (event 2).
const char* kProbeHeader = R"(#pragma once
extern "C" void hot_oc_note(int what);
extern "C" void hot_oc_register(int (*fn)(int));
namespace oc_detail
{
inline int probe(int code)
{
    try
    {
        if (code > 0)
            throw code;
    }
    catch (int e)
    {
        return e * 10;
    }
    return -1;
}
struct Global
{
    Global() { hot_oc_note(1); hot_oc_register(&probe); }
    ~Global() { hot_oc_note(2); }
};
static Global global;
}
struct Widget
{
    virtual ~Widget() = default;
    virtual int Power() { return 0; }
};
)";

// No host call: for the test with threads.
const char* kPlainHeader = "#pragma once\nstruct Widget\n{\n    virtual ~Widget() = default;\n"
                           "    virtual int Power() { return 0; }\n};\n";

const char* kScript = "using \"Widget.h\";\nclass Kid : Widget {\n    override int Power() { return 2; }\n}\n";
// A comment gives the same translated text (and the same object), so the edit changes the code.
const char* kScriptEdited = "using \"Widget.h\";\nclass Kid : Widget {\n    override int Power() { return 3; }\n}\n";

class EnvGuard
{
public:
    EnvGuard(const char* name, const char* value) : name_(name)
    {
        if (const char* old = std::getenv(name))
        {
            had_ = true;
            old_ = old;
        }
        if (value)
            test_support::set_env(name, value);
        else
            test_support::unset_env(name);
    }
    ~EnvGuard()
    {
        if (had_)
            test_support::set_env(name_.c_str(), old_.c_str());
        else
            test_support::unset_env(name_.c_str());
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    std::string name_;
    std::string old_;
    bool had_ = false;
};

struct Fixture
{
    test_support::TestScratch scratch;
    std::string dir; // the script and the header
    std::string cache;
    EnvGuard cache_on{"HOT_CACHE", nullptr};
    EnvGuard tr_on{"HOT_TRANSLATE_CACHE", nullptr};
    EnvGuard max_mb{"HOT_CACHE_MAX_MB", nullptr};

    explicit Fixture(const char* header = kProbeHeader)
    {
        dir = scratch.file("src");
        fs::create_directories(dir);
        cache = scratch.file("cache");
        write_header(header);
        for (int& e : hot_oc_events)
            e = 0;
        hot_oc_probe_fn = nullptr;
    }

    void write_header(const std::string& text) const
    {
        const std::string path = dir + "/Widget.h";
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        // The store does not take a header that changed near the translate.
        fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
    }

    std::unique_ptr<hot::JitBackend> make(const std::string& cache_dir, const std::vector<std::string>& flags = {},
        const std::string& id_extra = std::string()) const
    {
        hot::JitOptions o;
        o.compiler = HOT_CXX;
        o.include_dirs = {dir, std::string(HOT_SOURCE_DIR) + "/include"};
        o.resource_dir = HOT_RESOURCE_DIR;
        o.cache_dir = cache_dir;
        o.extra_flags = flags;
        o.cache_id_extra = id_extra;
        return std::make_unique<hot::JitBackend>(std::move(o));
    }

    hot::ScriptSource script(const std::string& text = kScript) const { return {dir + "/kid.hot", text}; }

    fs::path obj_dir() const { return fs::path(cache) / "jit" / "obj"; }

    // The paths of the files with the extension in the object directory.
    std::vector<fs::path> entries(const char* ext) const
    {
        std::vector<fs::path> out;
        std::error_code ec;
        for (fs::directory_iterator it(obj_dir(), ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ext)
                out.push_back(it->path());
        return out;
    }
};

std::string read_all(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Writes the file and keeps its time: the stamp in the manifest still matches.
void overwrite_keep_time(const fs::path& path, const std::string& text)
{
    const fs::file_time_type t = fs::last_write_time(path);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    fs::last_write_time(path, t);
}

// The 24-byte header of the cache file (jit_backend.cpp) with a correct hash for "object".
std::string with_header(const std::string& object)
{
    uint64_t h = 14695981039346656037ULL;
    for (const char c : object)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    uint64_t z = h;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    std::string out = "HOTJITO1";
    for (uint64_t v : {static_cast<uint64_t>(object.size()), z})
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    return out + object;
}

// Loads the script, checks the module and unloads it. The static initializer runs one time and
// the static destructor runs at the unload.
void load_and_check(hot::JitBackend& backend, const hot::ScriptSource& src, uint32_t generation)
{
    const int inits = hot_oc_events[1];
    const int dtors = hot_oc_events[2];
    hot_oc_probe_fn = nullptr;
    {
        hot::LoadResult r = backend.load(src, generation);
        ASSERT_TRUE(r) << r.error;
        EXPECT_EQ(hot_oc_events[1], inits + 1);
        ASSERT_NE(hot_oc_probe_fn, nullptr);
        EXPECT_EQ(hot_oc_probe_fn(3), 30); // a throw and a catch in the JIT code
        EXPECT_EQ(hot_oc_probe_fn(0), -1);
        EXPECT_EQ(hot_oc_events[2], dtors);
    }
    EXPECT_EQ(hot_oc_events[2], dtors + 1);
}

TEST(JitObjectCache, SecondBackendLoadsWithoutACompile)
{
    Fixture f;
    {
        auto backend = f.make(f.cache);
        ASSERT_NO_FATAL_FAILURE(load_and_check(*backend, f.script(), 1));
        EXPECT_EQ(backend->compiles(), 1u);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().misses, 1u);
    }
    ASSERT_EQ(f.entries(".o").size(), 1u);
    ASSERT_EQ(f.entries(".json").size(), 1u);
    // The initializer function has the name of the first load (__orc_init_func.<id>). The
    // generic platform of LLJIT runs each function with the prefix, so the name does not matter.
    EXPECT_NE(read_all(f.entries(".o")[0]).find("__orc_init_func.kid.g1.e0.0"), std::string::npos);

    auto backend = f.make(f.cache);
    // Another generation gives another id: the object has the old name.
    ASSERT_NO_FATAL_FAILURE(load_and_check(*backend, f.script(), 5));
    EXPECT_EQ(backend->compiles(), 0u);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().misses, 0u);
    EXPECT_EQ(backend->last_timings().compile_ms, 0);

    // The core instance of a hit gets the same object in its own JITDylib.
    const int inits = hot_oc_events[1];
    hot::LoadResult r = backend->load(f.script(), 6);
    ASSERT_TRUE(r) << r.error;
    ASSERT_TRUE(r.module->make_core);
    hot::LoadResult core = r.module->make_core();
    ASSERT_TRUE(core) << core.error;
    EXPECT_EQ(hot_oc_events[1], inits + 2);
    EXPECT_EQ(hot_oc_probe_fn(4), 40);
    hot::LoadResult again = r.module->make_core();
    EXPECT_FALSE(again);
    EXPECT_EQ(backend->compiles(), 0u);
}

TEST(JitObjectCache, ChangedInputsMiss)
{
    Fixture f;
    {
        auto backend = f.make(f.cache);
        ASSERT_TRUE(backend->load(f.script(), 1));
        EXPECT_EQ(backend->compiles(), 1u);
    }
    ASSERT_EQ(f.entries(".json").size(), 1u);
    // Each case is a miss with a compile.
    const auto expect_miss = [&](hot::JitBackend& backend, const hot::ScriptSource& src, const char* what)
    {
        hot::LoadResult r = backend.load(src, 1);
        ASSERT_TRUE(r) << what << ": " << r.error;
        EXPECT_EQ(backend.compiles(), 1u) << what;
        EXPECT_EQ(backend.cache_stats().hits, 0u) << what;
        EXPECT_EQ(backend.cache_stats().misses, 1u) << what;
    };
    {
        auto backend = f.make(f.cache);
        expect_miss(*backend, f.script(kScriptEdited), "script");
    }
    EXPECT_EQ(f.entries(".json").size(), 2u);
    {
        auto backend = f.make(f.cache, {"-DOC_EXTRA=1"});
        expect_miss(*backend, f.script(), "compile argument");
    }
    EXPECT_EQ(f.entries(".json").size(), 3u);
    {
        auto backend = f.make(f.cache, {}, "another compiler");
        expect_miss(*backend, f.script(), "compiler id");
    }
    EXPECT_EQ(f.entries(".json").size(), 4u);
    // A comment in the header: the translated text and the key stay, the manifest misses. The
    // store replaces the entry, so the number of entries stays.
    f.write_header(std::string(kProbeHeader) + "// a comment\n");
    {
        auto backend = f.make(f.cache);
        expect_miss(*backend, f.script(), "header");
    }
    EXPECT_EQ(f.entries(".json").size(), 4u);
    // The new entry hits.
    auto backend = f.make(f.cache);
    ASSERT_TRUE(backend->load(f.script(), 1));
    EXPECT_EQ(backend->compiles(), 0u);
}

TEST(JitObjectCache, BadEntryCompilesAgainAndIsReplaced)
{
    Fixture f;
    {
        auto backend = f.make(f.cache);
        ASSERT_TRUE(backend->load(f.script(), 1));
    }
    ASSERT_EQ(f.entries(".o").size(), 1u);
    const fs::path object = f.entries(".o")[0];
    const std::string good = read_all(object);
    // A damaged byte with the same size and time: the stamp matches, the hash does not.
    std::string damaged = good;
    damaged[damaged.size() / 2] = static_cast<char>(damaged[damaged.size() / 2] ^ 0x5a);
    // An object with a correct hash that does not link: hot_jit_add_object fails.
    const std::string junk = with_header(std::string(good.size() - 24, '\x7f'));
    for (const std::string* bad : {static_cast<const std::string*>(&damaged), &junk})
    {
        overwrite_keep_time(object, *bad);
        {
            auto backend = f.make(f.cache);
            ASSERT_NO_FATAL_FAILURE(load_and_check(*backend, f.script(), 1));
            EXPECT_EQ(backend->compiles(), 1u);
            EXPECT_EQ(backend->cache_stats().hits, 0u);
            EXPECT_EQ(backend->cache_stats().misses, 1u);
        }
        EXPECT_EQ(read_all(object), good) << "the store replaces the bad entry";
        auto backend = f.make(f.cache);
        ASSERT_NO_FATAL_FAILURE(load_and_check(*backend, f.script(), 1));
        EXPECT_EQ(backend->compiles(), 0u);
    }
}

TEST(JitObjectCache, EmptyCacheDirOrEnvironmentWritesNoObject)
{
    Fixture f;
    {
        auto backend = f.make("");
        ASSERT_TRUE(backend->load(f.script(), 1));
        ASSERT_TRUE(backend->load(f.script(), 2));
        EXPECT_EQ(backend->compiles(), 2u);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().misses, 0u);
    }
    EXPECT_FALSE(fs::exists(f.cache));
    EnvGuard off("HOT_CACHE", "0");
    for (int i = 0; i < 2; ++i)
    {
        auto backend = f.make(f.cache);
        ASSERT_TRUE(backend->load(f.script(), 1));
        EXPECT_EQ(backend->compiles(), 1u);
        EXPECT_EQ(backend->cache_stats().hits, 0u);
        EXPECT_EQ(backend->cache_stats().misses, 0u);
    }
    EXPECT_FALSE(fs::exists(fs::path(f.cache) / "jit"));
}

TEST(JitObjectCache, ReloadOfAnUnchangedUnitHits)
{
    Fixture f;
    auto backend = f.make(f.cache);
    ASSERT_NO_FATAL_FAILURE(load_and_check(*backend, f.script(), 1));
    ASSERT_NO_FATAL_FAILURE(load_and_check(*backend, f.script(), 2));
    EXPECT_EQ(backend->compiles(), 1u);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().misses, 1u);
}

// The debugger sees the code of a hit: the line table names the .hot file.
TEST(JitObjectCache, DebuggerGetsObjectOfAHit)
{
    if (!test_support::have_dwarfdump())
        GTEST_SKIP() << "llvm-dwarfdump is not installed";
    Fixture f;
    {
        auto backend = f.make(f.cache);
        ASSERT_TRUE(backend->load(f.script(), 1));
    }
    auto backend = f.make(f.cache);
    std::set<const jit_code_entry*> before;
    for (const jit_code_entry* e = __jit_debug_descriptor.first_entry; e; e = e->next_entry)
        before.insert(e);
    hot::LoadResult r = backend->load(f.script(), 1);
    ASSERT_TRUE(r) << r.error;
    ASSERT_EQ(backend->compiles(), 0u);
    ASSERT_TRUE(backend->debugger_support()) << backend->debugger_error();
    const std::string image = f.scratch.file("hit_debug_object.o");
    const int line = test_support::line_with(kScript, "return 2");
    bool found = false;
    for (const jit_code_entry* e = __jit_debug_descriptor.first_entry; e && !found; e = e->next_entry)
    {
        if (before.count(e) || e->symfile_size == 0)
            continue;
        {
            std::ofstream out(image, std::ios::binary | std::ios::trunc);
            out.write(e->symfile_addr, static_cast<std::streamsize>(e->symfile_size));
        }
        found = test_support::lines_of_file(test_support::dump_line_table(image), "kid.hot").count(line) != 0;
    }
    EXPECT_TRUE(found) << "no new object has the line " << line << " of kid.hot";
}

// Two backends on one directory at the same time store and read the same entries.
TEST(JitObjectCache, TwoBackendsShareTheDirectory)
{
    Fixture f(kPlainHeader);
    const std::vector<std::string> texts = {kScript, kScriptEdited};
    std::vector<std::string> errors(2);
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t)
        threads.emplace_back(
            [&, t]
            {
                auto backend = f.make(f.cache);
                for (uint32_t g = 1; g <= 4; ++g)
                {
                    hot::LoadResult r = backend->load(f.script(texts[(g + t) % 2]), g);
                    if (!r)
                        errors[t] += r.error;
                }
            });
    for (std::thread& t : threads)
        t.join();
    EXPECT_EQ(errors[0], "");
    EXPECT_EQ(errors[1], "");
    EXPECT_EQ(f.entries(".json").size(), 2u);
    auto backend = f.make(f.cache);
    for (const std::string& text : texts)
        ASSERT_TRUE(backend->load(f.script(text), 1));
    EXPECT_EQ(backend->compiles(), 0u);
}

// The layout of Generation in jit_backend.cpp: the module keeps it in "keep_alive".
struct GenerationLayout
{
    std::shared_ptr<void> impl;
    hot_jit_dylib* dylib;
};

bool holds_object(const hot::LoadResult& r)
{
    const auto* g = static_cast<const GenerationLayout*>(r.module->keep_alive.get());
    return hot_jit_dylib_object(g->dylib, nullptr, nullptr) != 0;
}

TEST(JitObjectCache, MissKeepsNoObjectCopyInTheCoreDylib)
{
    Fixture f;
    auto backend = f.make(f.cache);
    hot::LoadResult r = backend->load(f.script(), 1);
    ASSERT_TRUE(r) << r.error;
    EXPECT_EQ(f.entries(".o").size(), 1u); // the main object is in the cache
    EXPECT_FALSE(holds_object(r));
    ASSERT_TRUE(r.module->make_core);
    hot::LoadResult core = r.module->make_core();
    ASSERT_TRUE(core) << core.error;
    EXPECT_FALSE(holds_object(core));
}

// The same relative include dir, another current directory and another header of the same name.
TEST(JitObjectCache, RelativeIncludeDirOfAnotherWorkingDirectoryMisses)
{
    Fixture f;
    const std::string script_dir = f.scratch.file("script");
    fs::create_directories(script_dir);
    const std::string roots[2] = {f.scratch.file("a"), f.scratch.file("b")};
    for (int i = 0; i < 2; ++i)
    {
        fs::create_directories(roots[i] + "/inc");
        const std::string path = roots[i] + "/inc/Widget.h";
        std::ofstream(path, std::ios::binary) << kProbeHeader << (i ? "// other header\n" : "");
        fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
    }
    struct CwdGuard
    {
        fs::path old = fs::current_path();
        ~CwdGuard() { fs::current_path(old); }
    } guard;
    for (int i = 0; i < 2; ++i)
    {
        fs::current_path(roots[i]);
        hot::JitOptions o;
        o.compiler = HOT_CXX;
        o.include_dirs = {"inc", std::string(HOT_SOURCE_DIR) + "/include"};
        o.resource_dir = HOT_RESOURCE_DIR;
        o.cache_dir = f.cache;
        hot::JitBackend backend(std::move(o));
        hot::LoadResult r = backend.load({script_dir + "/kid.hot", kScript}, 1);
        ASSERT_TRUE(r) << r.error;
        EXPECT_EQ(backend.compiles(), 1u) << "run " << i;
        EXPECT_EQ(backend.cache_stats().hits, 0u) << "run " << i;
    }
}

} // namespace

#endif
