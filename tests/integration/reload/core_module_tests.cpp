// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The core module (design 4.9, M8). The core holds the class records, the factories, the
// thunks and the vtables. So every retired generation can unload, generation 1 too.
//
// The stress tests load copies of the library stress_generation (tests/integration/fixtures) as
// generation 1, 2, 3 and so on. They count the loaded modules in the loader stats and in
// /proc/self/maps. The backend tests use the backend that HOT_BACKEND selected.
//
// CMake defines STRESS_SO and HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#ifdef __APPLE__
    #include <mach-o/dyld.h> // _dyld_image_count, _dyld_get_image_name
#endif
#ifdef _WIN32
    #include <windows.h>
    // clang-format off
    #include <tlhelp32.h>
    // clang-format on
    #include <process.h>
    #define getpid _getpid // the MSVC CRT has the POSIX name only as a deprecated name (C4996)
#else
    #include <unistd.h>
#endif

// Internal helpers of module.cpp (not public API).
namespace hot
{
std::filesystem::path core_app_folder(const std::filesystem::path& temp);
void sweep_core_folder(const std::filesystem::path& folder);
} // namespace hot

namespace
{

namespace fs = std::filesystem;
using hot::GenerationInfo;
using hot::GenState;
using test_support::kDt;
using test_support::Session;
using test_support::source;

const GenerationInfo* find_gen(const hot::LoaderStats& st, uint32_t generation)
{
    for (const GenerationInfo& g : st.generations)
        if (g.generation == generation)
            return &g;
    return nullptr;
}

// The core modules and the generations that are not unloaded.
size_t loaded_modules(const hot::LoaderStats& st)
{
    size_t n = st.core_modules;
    for (const GenerationInfo& g : st.generations)
        n += g.state != GenState::Unloaded;
    return n;
}

// The count of different mapped files whose path has "needle". Linux reads /proc/self/maps.
// Windows lists the modules of the process. macOS lists the images of dyld.
size_t mapped_files(const std::string& needle)
{
    std::set<std::string> paths;
#ifdef _WIN32
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32 entry;
    entry.dwSize = sizeof entry;
    for (BOOL ok = Module32First(snap, &entry); ok; ok = Module32Next(snap, &entry))
    {
        std::string path = entry.szExePath;
        for (char& c : path)
            c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string want = needle;
        for (char& c : want)
            c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (path.find(want) != std::string::npos)
            paths.insert(path);
    }
    CloseHandle(snap);
#elif defined(__APPLE__)
    // macOS has no /proc. dyld lists the mapped images. dlclose removes an unmapped image.
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i)
        if (const char* name = _dyld_get_image_name(i); name && std::string(name).find(needle) != std::string::npos)
            paths.insert(name);
#else
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);)
    {
        const size_t slash = line.find('/');
        if (slash != std::string::npos && line.find(needle) != std::string::npos)
            paths.insert(line.substr(slash));
    }
#endif
    return paths.size();
}

int* int_field(const hot::Instance& o, const char* name)
{
    return hot::field_ptr<int>(o.shell, name);
}

// Removes the file of a loaded library. POSIX removes it. Windows refuses for a loaded DLL, so
// the error is ignored there.
void remove_loaded(const fs::path& p)
{
#ifdef _WIN32
    std::error_code ec;
    fs::remove(p, ec);
#else
    fs::remove(p);
#endif
}

// Reloads the stress library "reloads" times. Objects of many generations stay alive, some
// objects die, and some generations have a pin for three reloads. The count of loaded modules
// must stay bounded, and each object must keep its state.
void run_reloads(int reloads)
{
    const fs::path dir = fs::temp_directory_path() / ("hot-stress-" + std::to_string(getpid()));
    fs::create_directories(dir);
    int copies = 0;
    auto load_copy = [&]()
    {
        const fs::path p = dir / ("stress." + std::to_string(++copies) + ".so");
        fs::copy_file(STRESS_SO, p, fs::copy_options::overwrite_existing);
        hot::LoadResult r = hot::module_from_library(p.string());
        if (!r)
            throw std::runtime_error(r.error);
        return std::make_pair(std::move(*r.module), p);
    };
    using PokeFn = void (*)(Actor*, int);

    Session s;
    auto [m1, p1] = load_copy();
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(m1), "Pulse"));
    // The mapping stays; the core has its own copy. Windows cannot remove a loaded DLL: the
    // file stays, and remove_all at the end removes the directory when it can.
    remove_loaded(p1);
    s.commit_at_frame_boundary();
    const uint32_t first = hot::active_generation;
    std::deque<hot::Instance> objs; // objects outside the world; the test deletes them
    std::deque<std::pair<uint32_t, int>> pins; // pinned generation, reload of the unpin
    int pokes = 0;
    size_t max_loaded = 0;
    size_t max_mapped = 0;

    testing::internal::CaptureStderr(); // one commit line and one unload line for each reload
    for (int i = 1; i <= reloads; ++i)
    {
        if (i % 7 == 0)
        {
            hot::pin(hot::active_generation);
            pins.emplace_back(hot::active_generation, i + 3);
        }
        auto [m, p] = load_copy();
        std::string error;
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(m)));
        remove_loaded(p);
        s.world.Tick(kDt); // the commit runs at the frame boundary
        ASSERT_FALSE(s.commit_failed);
        ASSERT_EQ(hot::active_generation, first + static_cast<uint32_t>(i));
        while (!pins.empty() && pins.front().second <= i)
        {
            hot::unpin(pins.front().first);
            pins.pop_front();
        }
        reinterpret_cast<PokeFn>(s.loader.thunk("Pulse::poke"))(s.actor, 1);
        ++pokes;
        if (i % 10 == 0)
            objs.push_back(s.loader.find("Pulse")->create());
        if (i % 25 == 0 && objs.size() > 2)
        {
            delete static_cast<Actor*>(objs.front().native);
            objs.pop_front();
        }
        for (const hot::Instance& o : objs)
            static_cast<Actor*>(o.native)->Tick(kDt);
        s.world.Tick(kDt); // the retired generation unloads at this boundary
        const hot::LoaderStats st = s.loader.stats();
        max_loaded = std::max(max_loaded, loaded_modules(st));
        max_mapped = std::max(max_mapped, mapped_files(dir.string()) + mapped_files("hot-core-"));
        // The core, the active generation and the pinned generations stay.
        ASSERT_LE(loaded_modules(st), 2 + pins.size()) << "reload " << i << "\n" << s.loader.stats_text();
        ASSERT_EQ(st.core_modules, 1u);
    }
    testing::internal::GetCapturedStderr();
    while (!pins.empty())
    {
        hot::unpin(pins.front().first);
        pins.pop_front();
    }
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    const hot::LoaderStats st = s.loader.stats();
    EXPECT_EQ(loaded_modules(st), 2u) << s.loader.stats_text();
    EXPECT_EQ(find_gen(st, first)->state, GenState::Unloaded);
    EXPECT_LE(max_loaded, 2u + 1u); // at most one pin is open at a time
    EXPECT_LE(max_mapped, max_loaded);
    EXPECT_EQ(mapped_files(dir.string()) + mapped_files("hot-core-"), 2u);
    EXPECT_EQ(st.live_objects, 1 + objs.size());

    // The object of generation 1 kept its state over all reloads.
    EXPECT_EQ(*int_field(s.inst, "hits"), pokes);
    EXPECT_EQ(*int_field(s.inst, "ticks"), 2 * reloads + 2);
    for (const hot::Instance& o : objs)
    {
        const int t = *int_field(o, "ticks");
        static_cast<Actor*>(o.native)->Tick(kDt);
        EXPECT_EQ(*int_field(o, "ticks"), t + 1);
        delete static_cast<Actor*>(o.native);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int reload_count(int fallback)
{
    const char* v = std::getenv("HOT_STRESS_RELOADS");
    return v && std::atoi(v) > 0 ? std::atoi(v) : fallback;
}

// The fast variant of the stress test is in the normal suite.
TEST(CoreModule, ManyReloadsKeepTheModuleCountBounded)
{
    run_reloads(reload_count(200));
}

// The full variant (design M8: 1000 reloads). Set HOT_STRESS=1 to run it.
TEST(CoreModule, ThousandReloads)
{
    if (!std::getenv("HOT_STRESS"))
        GTEST_SKIP() << "set HOT_STRESS=1 to run 1000 reloads";
    run_reloads(1000);
}

// A removed class (design 6.1). Its record is in the core. Its live object uses the layout of
// the last generation that had the class, so that generation stays loaded. When the object
// is gone, the generation unloads and the factory makes an object with an empty state.
TEST_P(BackendTests, RemovedClassHoldsOnlyTheGenerationOfItsLayout)
{
    hot::LoadResult v1 = backend->load(source("removed_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("removed_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    hot::LoadResult v3 = backend->load(source("removed_v2.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Hero"));
    s.commit_at_frame_boundary();
    hot::Instance ghost = s.loader.find("Ghost")->create();
    static_cast<Actor*>(ghost.native)->Tick(kDt);
    ASSERT_EQ(*int_field(ghost, "haunts"), 1);

    std::string error;
    testing::internal::CaptureStderr();
    for (hot::LoadResult* r : {&v2, &v3})
    {
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*r->module)));
        s.world.Tick(kDt);
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
    }
    s.world.Tick(kDt);
    hot::LoaderStats st = s.loader.stats();
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired); // the layout of Ghost
    EXPECT_EQ(find_gen(st, 2)->state, GenState::Unloaded);
    EXPECT_EQ(st.core_modules, 1u);
    // The Ghost methods go to the handler. The state stays.
    static_cast<Actor*>(ghost.native)->Tick(kDt);
    EXPECT_EQ(*int_field(ghost, "haunts"), 1);

    delete static_cast<Actor*>(ghost.native);
    s.world.Tick(kDt);
    st = s.loader.stats();
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Unloaded);
    // The factory of the removed class still works. The object has an empty state.
    hot::Instance late = s.loader.find("Ghost")->create();
    static_cast<Actor*>(late.native)->Tick(kDt);
    EXPECT_EQ(int_field(late, "haunts"), nullptr);
    delete static_cast<Actor*>(late.native);
    testing::internal::GetCapturedStderr();
    // The Hero object runs generation 3.
    const int t = *int_field(s.inst, "ticks");
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s.inst, "ticks"), t + 100);
}

// A std::shared_ptr field: its control block has the vtable of the generation that made it.
// So no retired generation unloads while the object lives (kMayHoldCode).
TEST_P(BackendTests, CodeHoldingFieldBlocksTheUnload)
{
    hot::LoadResult v1 = backend->load(source("handle_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend->load(source("handle_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Knight"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    std::string error;
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    for (int i = 0; i < 4; ++i)
        s.world.Tick(kDt);
    testing::internal::GetCapturedStderr();
    ASSERT_FALSE(s.commit_failed);
    const hot::LoaderStats st = s.loader.stats();
    EXPECT_TRUE(st.state_holds_code);
    EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired);
    EXPECT_NE(s.loader.stats_text().find("unload blocked"), std::string::npos);
}

#ifndef _WIN32
// A private TMPDIR: tests that run in parallel (ctest -j) do not add "hot-core-*" entries to the counted folder.
struct PrivateTmp
{
    std::string old;
    bool had = false;
    std::filesystem::path dir;
    PrivateTmp()
    {
        if (const char* o = std::getenv("TMPDIR"))
        {
            had = true;
            old = o;
        }
        std::error_code ec;
        std::string tmpl = (std::filesystem::temp_directory_path(ec) / "hot-test-tmp-XXXXXX").string();
        if (mkdtemp(tmpl.data()))
        {
            dir = tmpl;
            setenv("TMPDIR", tmpl.c_str(), 1);
        }
    }
    ~PrivateTmp()
    {
        if (had)
            setenv("TMPDIR", old.c_str(), 1);
        else
            unsetenv("TMPDIR");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
#endif

#ifndef _WIN32
size_t folder_files(const std::filesystem::path& dir)
{
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        ++n;
    return n;
}

// The paths in /proc/self/maps that hold the needle.
std::set<std::string> mapped_paths(const std::string& needle)
{
    std::set<std::string> paths;
    std::ifstream in("/proc/self/maps");
    for (std::string line; std::getline(in, line);)
        if (line.find(needle) != std::string::npos)
            paths.insert(line.substr(line.find('/')));
    return paths;
}
#endif

// The sweep deletes regular files and symlinks. It keeps a subfolder and every symlink target.
TEST(CoreModule, SweepDeletesFilesOnly)
{
#ifdef _WIN32
    GTEST_SKIP() << "POSIX symlink test";
#else
    namespace fs = std::filesystem;
    const PrivateTmp tmp;
    const fs::path decoy = fs::temp_directory_path() / "decoy";
    { std::ofstream(decoy) << "decoy"; }
    const fs::path dir = hot::core_app_folder(fs::temp_directory_path());
    fs::create_directories(dir / "sub");
    { std::ofstream(dir / "sub" / "inner") << "x"; }
    { std::ofstream(dir / "a.so") << "x"; }
    fs::create_symlink(decoy, dir / "link.so");
    hot::sweep_core_folder(dir);
    EXPECT_FALSE(fs::exists(dir / "a.so"));
    EXPECT_FALSE(fs::is_symlink(dir / "link.so"));
    EXPECT_TRUE(fs::exists(dir / "sub" / "inner"));
    EXPECT_TRUE(fs::exists(decoy));
    EXPECT_TRUE(fs::exists(dir));
#endif
}

// The loader unlinks the copy right after dlopen: the per-app folder holds no file.
TEST(CoreModule, CopyIsUnlinkedAfterLoad)
{
#ifdef _WIN32
    GTEST_SKIP() << "a loaded DLL cannot be deleted";
#else
    namespace fs = std::filesystem;
    const PrivateTmp tmp;
    hot::LoadResult r = hot::module_from_library(STRESS_SO);
    ASSERT_TRUE(r) << r.error;
    hot::LoadResult core = r.module->make_core();
    ASSERT_TRUE(core) << core.error;
    EXPECT_EQ(folder_files(hot::core_app_folder(fs::temp_directory_path())), 0u);
    EXPECT_NE(core.module->register_fn, nullptr);
#endif
}

// Two cores in a row use two different file names.
TEST(CoreModule, CopyNamesDiffer)
{
#ifdef _WIN32
    GTEST_SKIP() << "POSIX test: it reads /proc/self/maps";
#else
    const PrivateTmp tmp;
    hot::LoadResult r = hot::module_from_library(STRESS_SO);
    ASSERT_TRUE(r) << r.error;
    hot::LoadResult c1 = r.module->make_core();
    hot::LoadResult c2 = r.module->make_core();
    ASSERT_TRUE(c1) << c1.error;
    ASSERT_TRUE(c2) << c2.error;
    const std::string folder = hot::core_app_folder(std::filesystem::temp_directory_path()).string();
    EXPECT_EQ(mapped_paths(folder).size(), 2u);
#endif
}

// A planted symlink in the per-app folder stays harmless: the core loads and the target stays.
TEST(CoreModule, CopyDoesNotFollowPlantedSymlink)
{
#ifdef _WIN32
    GTEST_SKIP() << "POSIX symlink test";
#else
    namespace fs = std::filesystem;
    const PrivateTmp tmp;
    const fs::path decoy = fs::temp_directory_path() / "decoy";
    { std::ofstream(decoy) << "decoy"; }
    const fs::path dir = hot::core_app_folder(fs::temp_directory_path());
    fs::create_directories(dir);
    fs::permissions(dir, fs::perms::owner_all);
    for (int n = 0; n < 64; ++n)
        fs::create_symlink(decoy, dir / ("planted" + std::to_string(n) + ".so"));
    hot::LoadResult r = hot::module_from_library(STRESS_SO);
    ASSERT_TRUE(r) << r.error;
    hot::LoadResult core = r.module->make_core();
    EXPECT_TRUE(core) << core.error;
    EXPECT_NE(core.module->register_fn, nullptr);
    core = {};
    r = {};
    std::ifstream in(decoy);
    std::string text;
    std::getline(in, text);
    EXPECT_EQ(text, "decoy");
#endif
}

// A shared folder with a wrong mode, or a symlink as the folder: the code uses a private directory.
// The sweep runs once for each process, so the test checks only the fallback and the load.
TEST(CoreModule, UnsafeSharedFolderFallsBack)
{
#ifdef _WIN32
    GTEST_SKIP() << "POSIX test";
#else
    namespace fs = std::filesystem;
    const PrivateTmp tmp;
    const fs::path dir = hot::core_app_folder(fs::temp_directory_path());
    const fs::path other = fs::temp_directory_path() / "other";
    fs::create_directories(dir);
    fs::create_directories(other);
    fs::permissions(dir, fs::perms::all); // group and others can write
    for (int pass = 0; pass < 2; ++pass)
    {
        hot::LoadResult r = hot::module_from_library(STRESS_SO);
        ASSERT_TRUE(r) << r.error;
        hot::LoadResult core = r.module->make_core();
        ASSERT_TRUE(core) << core.error;
        EXPECT_NE(core.module->register_fn, nullptr);
        EXPECT_EQ(folder_files(pass == 0 ? dir : other), 0u);
        // The private directories are gone: only "dir" or its link and "other" stay in the temp directory.
        size_t private_dirs = 0;
        for (const auto& e : fs::directory_iterator(fs::temp_directory_path()))
            private_dirs += e.path().filename().string().rfind("hot-core~", 0) == 0;
        EXPECT_EQ(private_dirs, 0u);
        if (pass == 0)
        {
            fs::remove(dir);
            fs::create_directory_symlink(other, dir);
        }
    }
#endif
}

} // namespace
