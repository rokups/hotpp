// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the file system of the host (hot::FileSystem) with the backend that HOT_BACKEND
// selected. The headers and the script imports exist only in memory.
//
// CMake defines the macro HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "hot/vfs.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#ifdef _WIN32
    #include <process.h> // getpid
#else
    #include <unistd.h>
#endif

namespace
{

using test_support::kDt;
using test_support::Session;

// A file system in memory. A mutex makes it thread safe, as the interface requires.
class MemoryFs final : public hot::FileSystem
{
public:
    bool read(const std::string& path, std::string& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = files_.find(path);
        if (it == files_.end())
            return false;
        out = it->second.text;
        return true;
    }
    hot::VfsStat stat(const std::string& path) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = files_.find(path);
        if (it == files_.end())
            return {};
        return {true, it->second.version};
    }
    void set(const std::string& path, const std::string& text, uint64_t version)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = {text, version};
    }

private:
    struct File
    {
        std::string text;
        uint64_t version = 0;
    };
    std::mutex mutex_;
    std::map<std::string, File> files_;
};

std::unique_ptr<hot::Backend> backend_with(std::shared_ptr<hot::FileSystem> fs)
{
    hot::DefaultBackendOptions o;
    o.fs = std::move(fs);
    return hot::make_default_backend(o);
}

std::string header(float value)
{
    return "inline float vfs_value() { return " + std::to_string(value) + "f; }\n";
}

std::string module_text(float scale)
{
    return "using \"Actor.h\";\nclass Base : Actor {\n    float scale = 1.0;\n"
           "    virtual float Scale() { return " + std::to_string(scale) + "; }\n}\n";
}

const char* kMain =
    "using \"Actor.h\";\nusing \"v.h\";\nusing \"m.hot\";\n"
    "class Kid : Base {\n    float result = 0.0;\n"
    "    override void Tick(float dt) { this.result = vfs_value() * this.Scale(); }\n}\n";

// The directory of the virtual files: it does not exist on the disk.
const std::string kDir = "/hot_vfs_backend_test";

void set_files(MemoryFs& fs, const std::string& dir, float value, float scale, uint64_t version)
{
    fs.set(dir + "/v.h", header(value), version);
    fs.set(dir + "/m.hot", module_text(scale), version);
}

float tick_result(Session& s)
{
    s.world.Tick(kDt);
    return *s.field("result");
}

// The native header and the script module exist only in memory (an include directory).
TEST(VfsBackend, ImportsFromTheIncludeDirectoryInMemory)
{
    auto fs = std::make_shared<MemoryFs>();
    set_files(*fs, kDir + "/inc", 3.0f, 2.0f, 1);
    hot::DefaultBackendOptions o;
    o.fs = fs;
    o.include_dirs = {kDir + "/inc"};
    auto backend = hot::make_default_backend(o);
    hot::LoadResult r = backend->load({"main.hot", kMain, {}}, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Kid"));
    EXPECT_FLOAT_EQ(tick_result(s), 6.0f);
}

// The same files in the directory of the script.
TEST(VfsBackend, ImportsFromTheScriptDirectoryInMemory)
{
    auto fs = std::make_shared<MemoryFs>();
    set_files(*fs, kDir + "/script", 4.0f, 2.0f, 1);
    auto backend = backend_with(fs);
    hot::LoadResult r = backend->load({kDir + "/script/main.hot", kMain, {}}, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Kid"));
    EXPECT_FLOAT_EQ(tick_result(s), 8.0f);
}

// A new version and a new text of the header and of the module give new behavior at a reload.
TEST(VfsBackend, NewVersionReloadsWithNewBehavior)
{
    auto fs = std::make_shared<MemoryFs>();
    set_files(*fs, kDir + "/script", 3.0f, 2.0f, 1);
    auto backend = backend_with(fs);
    const hot::ScriptSource src{kDir + "/script/main.hot", kMain, {}};
    hot::LoadResult v1 = backend->load(src, 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Kid"));
    s.commit_at_frame_boundary();
    EXPECT_FLOAT_EQ(tick_result(s), 6.0f);

    set_files(*fs, kDir + "/script", 5.0f, 4.0f, 2);
    hot::LoadResult v2 = backend->load(src, 2);
    ASSERT_TRUE(v2) << v2.error;
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the reload commits after this Tick
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_FLOAT_EQ(tick_result(s), 20.0f);
}

// The headers are on the disk and the host file system is null or has no such file.
TEST(VfsBackend, DiskHeadersStillWork)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("hot_vfs_disk_" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "v.h") << header(2.0f);
    std::ofstream(dir / "m.hot") << module_text(3.0f);
    for (const bool with_fs : {false, true})
    {
        hot::DefaultBackendOptions o;
        if (with_fs)
            o.fs = std::make_shared<MemoryFs>();
        o.include_dirs = {dir.string()};
        auto backend = hot::make_default_backend(o);
        hot::LoadResult r = backend->load({"main.hot", kMain, {}}, 1);
        ASSERT_TRUE(r) << r.error;
        Session s;
        ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Kid"));
        EXPECT_FLOAT_EQ(tick_result(s), 6.0f);
    }
    fs::remove_all(dir);
}

// A directory that exists only in the host file system, and that is a relative path.
TEST(VfsBackend, RelativeDirectoryThatExistsOnlyInMemory)
{
    auto fs = std::make_shared<MemoryFs>();
    set_files(*fs, "hot_vfs_virt_only", 3.0f, 2.0f, 1);
    hot::DefaultBackendOptions o;
    o.fs = fs;
    o.include_dirs = {"hot_vfs_virt_only"};
    auto backend = hot::make_default_backend(o);
    hot::LoadResult r = backend->load({"main.hot", kMain, {}}, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Kid"));
    EXPECT_FLOAT_EQ(tick_result(s), 6.0f);
    EXPECT_FALSE(std::filesystem::exists("hot_vfs_virt_only"));
}

// The script folder and an include folder are relative paths with ".." parts.
TEST(VfsBackend, RelativeDirectoriesWithDotDotWork)
{
    auto fs = std::make_shared<MemoryFs>();
    fs->set("../hot_vfs_rel/shared/v.h", header(3.0f), 1);
    fs->set("../hot_vfs_rel/script/m.hot", module_text(2.0f), 1);
    hot::DefaultBackendOptions o;
    o.fs = fs;
    o.include_dirs = {"../hot_vfs_rel/shared"};
    auto backend = hot::make_default_backend(o);
    hot::LoadResult r = backend->load({"../hot_vfs_rel/script/main.hot", kMain, {}}, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Kid"));
    EXPECT_FLOAT_EQ(tick_result(s), 6.0f);
}

// A path that climbs out with ".." does not write outside the mirror directory of the backend.
TEST(VfsBackend, TraversalDoesNotWriteOutsideTheMirror)
{
    const std::string escape = "/tmp/hot_vfs_escape_" + std::to_string(getpid());
    std::filesystem::remove_all(escape);
    auto fs = std::make_shared<MemoryFs>();
    const std::string dir = kDir + "/x/../../../../../../../../../../../../.." + escape;
    set_files(*fs, dir, 3.0f, 2.0f, 1);
    hot::DefaultBackendOptions o;
    o.fs = fs;
    o.include_dirs = {dir};
    auto backend = hot::make_default_backend(o);
    const hot::LoadResult r = backend->load({"main.hot", kMain, {}}, 1);
    EXPECT_TRUE(r) << r.error;
    EXPECT_FALSE(std::filesystem::exists(escape));
}

} // namespace
