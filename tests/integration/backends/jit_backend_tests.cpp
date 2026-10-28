// Tests for the JIT backend only (SPP_BACKEND=JIT).
//
// The backend must not write a file and must not start a process. These tests
// check this, and they check that a generation can go away.
//
// CMake defines these macros: SPP_SOURCE_DIR, SPP_CXX and SPP_RESOURCE_DIR.
#include <gtest/gtest.h>

#include <sys/resource.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "spp/backend.h"
#include "spp/jit_backend.h"
#include "support/f1_scenario.h"

namespace {

namespace fs = std::filesystem;

spp::ScriptSource source(const char* file)
{
    const std::string path = std::string(SPP_SOURCE_DIR) + "/example/scripts/" + file;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return {path, ss.str()};
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
    for (const auto& task : fs::directory_iterator("/proc/self/task")) {
        std::ifstream f(task.path() / "children");
        for (std::string pid; f >> pid;) ++n;
    }
    return n;
}

// Sets the current directory and TMPDIR to a new empty directory. The
// destructor restores both and removes the directory.
struct EmptyWorkDir {
    fs::path dir;
    fs::path old_cwd = fs::current_path();
    std::string old_tmp;
    bool had_tmp = false;

    EmptyWorkDir()
    {
        dir = fs::temp_directory_path() / ("spp_jit_empty_" + std::to_string(getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        if (const char* t = std::getenv("TMPDIR")) {
            had_tmp = true;
            old_tmp = t;
        }
        setenv("TMPDIR", dir.c_str(), 1);
        fs::current_path(dir);
    }
    ~EmptyWorkDir()
    {
        fs::current_path(old_cwd);
        if (had_tmp) setenv("TMPDIR", old_tmp.c_str(), 1);
        else unsetenv("TMPDIR");
        fs::remove_all(dir);
    }
    size_t entries() const
    {
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
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
    std::unique_ptr<spp::Backend> backend = spp::make_default_backend();
    ASSERT_STREQ(backend->name(), "JIT");
    EmptyWorkDir work;
    const long faults_before = child_faults();
    {
        spp::LoadResult v1 = backend->load(source("player_v1.spp"), 1);
        ASSERT_TRUE(v1) << v1.error;
        spp::LoadResult v2 = backend->load(source("player_v2.spp"), 2);
        ASSERT_TRUE(v2) << v2.error;
        // A bad script also leaves no file and starts no process.
        spp::LoadResult bad = backend->load(source("bad.spp"), 1);
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
    std::unique_ptr<spp::Backend> backend = spp::make_default_backend();
    spp::LoadResult bad = backend->load(source("bad.spp"), 1);
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error.find("bad.spp:9:"), std::string::npos) << bad.error;
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
             "#include <utility>\n#include \"spp/runtime.h\"\n#include \"Actor.h\"\n"
             "#include \"Math.h\"\n";
    }
    spp::JitOptions o;
    o.compiler = SPP_CXX;
    o.include_dirs = {std::string(SPP_SOURCE_DIR) + "/example/engine", std::string(SPP_SOURCE_DIR) + "/include"};
    o.resource_dir = SPP_RESOURCE_DIR;
    std::string error;
    {
        spp::JitBackend maker(o);
        ASSERT_TRUE(maker.make_pch(header, pch, error)) << error;
    }
    ASSERT_TRUE(fs::exists(pch));
    o.pch_path = pch;
    spp::JitBackend backend(o);
    spp::LoadResult v1 = backend.load(source("player_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend.load(source("player_v2.spp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    f1::run_f1_reload(std::move(*v1.module), std::move(*v2.module));
}

// Exceptions: one is caught inside the JIT code, one goes through JIT frames to the
// host. Both need the unwind tables of the JIT code (EH frames), which JITLink registers.
TEST(JitTests, ExceptionsWorkInJitCode)
{
    const spp::ScriptSource src = {"throws.spp", R"(
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
)"};
    std::unique_ptr<spp::Backend> backend = spp::make_default_backend();
    spp::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module)));
    s.world.Tick(test_support::kDt);  // the throw and the catch are both in the JIT code
    ASSERT_NE(s.field("speed"), nullptr);
    EXPECT_EQ(*s.field("speed"), 9.0f);
    int caught = 0;
    try {
        s.actor->TakeDamage(1.0f);  // the throw goes through JIT frames to this frame
    } catch (int e) {
        caught = e;
    }
    EXPECT_EQ(caught, 42);
}

// A Module that a copy of keep_alive holds stays valid after the backend is gone.
// When the last copy goes, the generation is removed from the JIT.
TEST(JitTests, ModuleOutlivesBackendAndGenerationsCanBeRemoved)
{
    spp::Module kept;
    {
        std::unique_ptr<spp::Backend> backend = spp::make_default_backend();
        for (int i = 0; i < 3; ++i) {
            spp::LoadResult r = backend->load(source("player_v1.spp"), 1);
            ASSERT_TRUE(r) << r.error;
            // The same generation loads again and again: each time a new JITDylib.
            if (i == 2) kept = std::move(*r.module);
        }
    }
    ASSERT_NE(kept.register_fn, nullptr);
    test_support::Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(kept)));
    EXPECT_NE(s.field("hp"), nullptr);
}

}  // namespace
