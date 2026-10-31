// Tests for the JIT backend only (HOT_ENABLE_JIT resolves ON).
//
// The backend must not write a file and must not start a process. These tests
// check this, and they check that a generation can go away.
//
// CMake defines these macros: HOT_SOURCE_DIR, HOT_SCRATCH_DIR, HOT_CXX and HOT_RESOURCE_DIR.
#include "hot/backend.h"
#include "hot/jit_backend.h"
#include "support/dwarf_lines.h"
#include "support/f1_scenario.h"
#include "support/scratch_dir.h"
#include "support/script_files.h"

#include <gtest/gtest.h>
#include <sys/resource.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

// The GDB JIT interface (the layout is fixed by the GDB manual). LLVM defines the descriptor.
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

// Sets the current directory and TMPDIR to a new empty directory. The
// destructor restores both and removes the directory.
struct EmptyWorkDir
{
    fs::path dir;
    fs::path old_cwd = fs::current_path();
    std::string old_tmp;
    bool had_tmp = false;

    EmptyWorkDir()
    {
        dir = fs::temp_directory_path() / ("hot_jit_empty_" + std::to_string(getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        if (const char* t = std::getenv("TMPDIR"))
        {
            had_tmp = true;
            old_tmp = t;
        }
        setenv("TMPDIR", dir.c_str(), 1);
        fs::current_path(dir);
    }
    ~EmptyWorkDir()
    {
        fs::current_path(old_cwd);
        if (had_tmp)
            setenv("TMPDIR", old_tmp.c_str(), 1);
        else
            unsetenv("TMPDIR");
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
    ASSERT_STREQ(backend->name(), "JIT");
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
             "#include \"Math.h\"\n";
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

// Exceptions: one is caught inside the JIT code, one goes through JIT frames to the
// host. Both need the unwind tables of the JIT code (EH frames), which JITLink registers.
TEST(JitTests, ExceptionsWorkInJitCode)
{
    const hot::ScriptSource src = {"throws.hot", R"(
using "Actor.h";
using "Math.h";

class Player : Actor {
    float speed = 5.0;

    override void Tick(float dt) {
        try { throw 7; } catch (int e) { this.speed = 9.0; }
    }

    override void TakeDamage(float amount) {
        throw 42;
    }

    override void TakeDamage(Actor& source, float amount) {}
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

} // namespace
