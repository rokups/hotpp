// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::FileSystem: the disk file system, and the imports that a host file system holds.
#include "hot/hotc_lib.h"
#include "hot/vfs.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace
{

using namespace translator_test;

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
    // The version 0 means unknown.
    void set(const std::string& path, const std::string& text, uint64_t version = 1)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = {text, version};
    }
    void remove(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_.erase(path);
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

TEST(Vfs, DiskReadsAFile)
{
    TempDir dir;
    write_file(dir.path() / "a.txt", "hello");
    std::string text;
    EXPECT_TRUE(hot::disk_file_system()->read((dir.path() / "a.txt").string(), text));
    EXPECT_EQ(text, "hello");
    EXPECT_FALSE(hot::disk_file_system()->read((dir.path() / "none.txt").string(), text));
}

TEST(Vfs, DiskStatGivesExistsAndVersion)
{
    TempDir dir;
    write_file(dir.path() / "a.txt", "hello");
    const std::string path = (dir.path() / "a.txt").string();
    const hot::VfsStat st = hot::disk_file_system()->stat(path);
    EXPECT_TRUE(st.exists);
    EXPECT_NE(st.version, 0u);
    EXPECT_EQ(hot::disk_file_system()->stat(path).version, st.version);
    write_file(dir.path() / "a.txt", "hello, world");
    EXPECT_NE(hot::disk_file_system()->stat(path).version, st.version);
    const hot::VfsStat none = hot::disk_file_system()->stat((dir.path() / "none.txt").string());
    EXPECT_FALSE(none.exists);
    EXPECT_EQ(none.version, 0u);
}

// The native header and the script module exist only in memory.
TEST(Vfs, TranslatesImportsThatExistOnlyInMemory)
{
    TempDir dir;
    const std::string d = dir.path().string();
    auto mem = std::make_shared<MemoryFs>();
    mem->set(d + "/v.h", "#include \"inner.h\"\n");
    mem->set(d + "/inner.h", kHeaderV1);
    mem->set(d + "/m.hot", "using \"v.h\";\nclass Base : Widget {\n    virtual int Power() { return 1; }\n}\n");
    hotc::TranslateOptions o = temp_options(dir, "using \"v.h\";\nusing \"m.hot\";\n"
                                                 "class Kid : Base {\n    override int Power() { return 2; }\n}\n");
    o.fs = mem;
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.virtual_files.size(), 2u);
    EXPECT_EQ(r.virtual_files[0].first, d + "/v.h");
    EXPECT_EQ(r.virtual_files[1].first, d + "/inner.h");
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_EQ(r.imports[0].first, d + "/m.hot");
}

// The dependencies hold the header, the header that it includes, and the imported script.
// A second translation with the same session gives the same list.
TEST(Vfs, DependenciesListTheFilesThatTheTranslationRead)
{
    TempDir dir;
    const std::string d = dir.path().string();
    auto mem = std::make_shared<MemoryFs>();
    mem->set(d + "/v.h", "#include \"inner.h\"\n");
    mem->set(d + "/inner.h", kHeaderV1);
    mem->set(d + "/m.hot", "class Base : Widget {\n    virtual int Power() { return 1; }\n}\n");
    hotc::TranslateOptions o = temp_options(dir, "using \"v.h\";\nusing \"m.hot\";\n"
                                                 "class Kid : Base {\n    override int Power() { return 2; }\n}\n");
    o.fs = mem;
    hotc::TranslatorSession session;
    const hotc::TranslateResult first = hotc::translate(o, session);
    ASSERT_TRUE(first.ok) << first.diagnostics;
    const auto& deps = first.dependencies;
    EXPECT_NE(std::find(deps.begin(), deps.end(), d + "/v.h"), deps.end());
    EXPECT_NE(std::find(deps.begin(), deps.end(), d + "/inner.h"), deps.end());
    EXPECT_NE(std::find(deps.begin(), deps.end(), d + "/m.hot"), deps.end());
    EXPECT_EQ(std::set<std::string>(deps.begin(), deps.end()).size(), deps.size()) << "duplicates";
    for (const std::string& dep : deps)
        EXPECT_EQ(dep.find("/include/c++/"), std::string::npos) << "a system header: " << dep;

    const hotc::TranslateResult second = hotc::translate(o, session);
    ASSERT_TRUE(second.ok) << second.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(second.dependencies, first.dependencies);
}

// The include directory has no folder on the disk.
TEST(Vfs, DirectoryThatExistsOnlyInMemory)
{
    auto mem = std::make_shared<MemoryFs>();
    mem->set("/hot_vfs_none/inc/w.h", kHeaderV1);
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = widget_script("Update(float dt)", "override ");
    o.generation = 1;
    o.include_dirs = {"/hot_vfs_none/inc"};
    o.fs = mem;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.virtual_files.size(), 1u);
}

// A directory that is a relative path, and that has no folder on the disk.
TEST(Vfs, RelativeDirectoryThatExistsOnlyInMemory)
{
    auto mem = std::make_shared<MemoryFs>();
    mem->set("hot_vfs_virt_only/w.h", kHeaderV1);
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = widget_script("Update(float dt)", "override ");
    o.generation = 1;
    o.include_dirs = {"hot_vfs_virt_only"};
    o.fs = mem;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.virtual_files.size(), 1u);
}

// Two headers include each other through ".." parts. The paths must not grow without end.
TEST(Vfs, IncludeCycleWithDotDotEnds)
{
    TempDir dir;
    const std::string d = dir.path().string();
    auto mem = std::make_shared<MemoryFs>();
    mem->set(d + "/a/x.h", "#ifndef HOT_VFS_X\n#define HOT_VFS_X\n#include \"../b/y.h\"\n" + std::string(kHeaderV1)
            + "#endif\n");
    mem->set(d + "/b/y.h", "#include \"../a/x.h\"\n");
    hotc::TranslateOptions o = temp_options(dir, "using \"a/x.h\";\nclass Foo : Widget {\n}\n");
    o.fs = mem;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_LE(r.virtual_files.size(), 4u);
}

// A chain of includes that is deeper than the limit stops without a crash.
TEST(Vfs, DeepIncludeChainStops)
{
    TempDir dir;
    const std::string d = dir.path().string();
    auto mem = std::make_shared<MemoryFs>();
    for (int i = 0; i < 150; ++i)
        mem->set(d + "/h" + std::to_string(i) + ".h", "#include \"h" + std::to_string(i + 1) + ".h\"\n");
    hotc::TranslateOptions o = temp_options(dir, "using \"h0.h\";\nclass Foo {\n}\n");
    o.fs = mem;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_LE(r.virtual_files.size(), 66u);
    EXPECT_GE(r.virtual_files.size(), 60u);
}

// The host file system wins over the disk for the same directory.
TEST(Vfs, HostWinsOverDisk)
{
    TempDir dir;
    const std::string d = dir.path().string();
    write_file(dir.path() / "w.h", "class Widget { public: virtual ~Widget() {} };\n");
    auto mem = std::make_shared<MemoryFs>();
    mem->set(d + "/w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, widget_script("Update(float dt)", "override "));
    o.fs = mem;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.virtual_files.size(), 1u);
    EXPECT_EQ(r.virtual_files[0].first, d + "/w.h");
}

// A header that only the disk holds is not a virtual file.
TEST(Vfs, DiskHeaderIsNotVirtual)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, widget_script("Update(float dt)", "override "));
    o.fs = std::make_shared<MemoryFs>();
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.virtual_files.empty());
}

// With no host file system, the result is the same as before.
TEST(Vfs, NullFileSystemReadsTheDisk)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")));
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(r.virtual_files.empty());
}

// ---- the cache of the session with the versions of the virtual files

// A header with the virtual function Update. And a header without it.
const char* kWithUpdate = "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n";
const char* kNoUpdate = "class Widget { public: virtual ~Widget() {} };\n";

struct CacheFixture
{
    TempDir dir;
    std::string d = dir.path().string();
    std::shared_ptr<MemoryFs> mem = std::make_shared<MemoryFs>();
    hotc::TranslatorSession session;

    hotc::TranslateResult run(const std::shared_ptr<hot::FileSystem>& fs)
    {
        hotc::TranslateOptions o = temp_options(dir, widget_script("Update(float dt)", "override "));
        o.fs = fs;
        return hotc::translate(o, session);
    }
    hotc::TranslateResult run() { return run(mem); }
};

TEST(VfsCache, SameVersionAndTextDoNotParseAgain)
{
    CacheFixture f;
    f.mem->set(f.d + "/w.h", kWithUpdate, 7);
    ASSERT_TRUE(f.run().ok);
    const int n = f.session.parse_count();
    EXPECT_TRUE(f.run().ok);
    EXPECT_EQ(f.session.parse_count(), n);
}

TEST(VfsCache, NewVersionAndTextParseAgain)
{
    CacheFixture f;
    f.mem->set(f.d + "/w.h", kWithUpdate, 1);
    ASSERT_TRUE(f.run().ok);
    const int n = f.session.parse_count();
    f.mem->set(f.d + "/w.h", kNoUpdate, 2);
    const hotc::TranslateResult r = f.run();
    EXPECT_GT(f.session.parse_count(), n);
    EXPECT_FALSE(r.ok); // the new content is used: there is no Update to override
}

TEST(VfsCache, VersionZeroUsesTheContent)
{
    CacheFixture f;
    f.mem->set(f.d + "/w.h", kWithUpdate, 0);
    ASSERT_TRUE(f.run().ok);
    int n = f.session.parse_count();
    EXPECT_TRUE(f.run().ok);
    EXPECT_EQ(f.session.parse_count(), n); // the same text
    f.mem->set(f.d + "/w.h", kNoUpdate, 0);
    EXPECT_FALSE(f.run().ok);
    EXPECT_GT(f.session.parse_count(), n);
}

TEST(VfsCache, IncludedVirtualHeaderIsStamped)
{
    CacheFixture f;
    f.mem->set(f.d + "/w.h", "#include \"inner.h\"\n", 1);
    f.mem->set(f.d + "/inner.h", kWithUpdate, 1);
    ASSERT_TRUE(f.run().ok);
    const int n = f.session.parse_count();
    f.mem->set(f.d + "/inner.h", kNoUpdate, 2);
    EXPECT_FALSE(f.run().ok);
    EXPECT_GT(f.session.parse_count(), n);
}

TEST(VfsCache, VirtualFileHidesADiskHeader)
{
    CacheFixture f;
    write_file(f.dir.path() / "w.h", kWithUpdate);
    ASSERT_TRUE(f.run().ok);
    const int n = f.session.parse_count();
    f.mem->set(f.d + "/w.h", kNoUpdate, 1);
    EXPECT_FALSE(f.run().ok);
    EXPECT_GT(f.session.parse_count(), n);
}

TEST(VfsCache, VirtualFileInAnEarlierDirectoryHidesADiskHeader)
{
    CacheFixture f;
    const std::string first = f.d + "/first";
    write_file(f.dir.path() / "w.h", kWithUpdate);
    hotc::TranslateOptions o = temp_options(f.dir, widget_script("Update(float dt)", "override "));
    o.include_dirs = {first, f.d};
    o.fs = f.mem;
    ASSERT_TRUE(hotc::translate(o, f.session).ok);
    const int n = f.session.parse_count();
    f.mem->set(first + "/w.h", kNoUpdate, 1);
    EXPECT_FALSE(hotc::translate(o, f.session).ok);
    EXPECT_GT(f.session.parse_count(), n);
}

TEST(VfsCache, DeletedVirtualFileIsAnError)
{
    CacheFixture f;
    f.mem->set(f.d + "/w.h", kWithUpdate, 1);
    ASSERT_TRUE(f.run().ok);
    const int n = f.session.parse_count();
    f.mem->remove(f.d + "/w.h");
    const hotc::TranslateResult r = f.run();
    EXPECT_FALSE(r.ok);
    EXPECT_GT(f.session.parse_count(), n);
    EXPECT_FALSE(r.diagnostics.empty());
}

// The host holds the header under its normal path. libclang uses a path with "..".
TEST(VfsCache, HeaderWithDotDotInThePathIsStamped)
{
    CacheFixture f;
    f.mem->set(f.d + "/a/x.h", "#include \"../b/y.h\"\n", 1);
    f.mem->set(f.d + "/b/y.h", kWithUpdate, 1);
    auto run = [&]
    {
        hotc::TranslateOptions o = temp_options(
            f.dir, "using \"a/x.h\";\nclass Foo : Widget {\n    override void Update(float dt) { }\n}\n");
        o.fs = f.mem;
        return hotc::translate(o, f.session);
    };
    ASSERT_TRUE(run().ok);
    const int n = f.session.parse_count();
    EXPECT_TRUE(run().ok);
    EXPECT_EQ(f.session.parse_count(), n);
    f.mem->set(f.d + "/b/y.h", kNoUpdate, 2);
    EXPECT_FALSE(run().ok);
    EXPECT_GT(f.session.parse_count(), n);
}

// A new file system never uses the cache entry of an old one, also not at the same address.
TEST(VfsCache, EachFileSystemHasItsOwnId)
{
    EXPECT_NE(MemoryFs().id(), MemoryFs().id());
    struct Empty final : hot::FileSystem
    {
        bool read(const std::string&, std::string&) override { return false; }
        hot::VfsStat stat(const std::string&) override { return {}; }
    };
    Empty a;
    Empty copy(a);
    EXPECT_NE(a.id(), copy.id());
}

TEST(VfsCache, NewFileSystemWithTheSameVersionDoesNotHitTheOldEntry)
{
    CacheFixture f;
    auto first = std::make_shared<MemoryFs>();
    first->set(f.d + "/w.h", kWithUpdate, 5);
    ASSERT_TRUE(f.run(first).ok);
    const int n = f.session.parse_count();
    first.reset(); // the address can go to the next object
    auto second = std::make_shared<MemoryFs>();
    second->set(f.d + "/w.h", kNoUpdate, 5);
    EXPECT_FALSE(f.run(second).ok);
    EXPECT_GT(f.session.parse_count(), n);
}

TEST(VfsCache, NullFileSystemKeepsTheCache)
{
    CacheFixture f;
    write_file(f.dir.path() / "w.h", kWithUpdate);
    ASSERT_TRUE(f.run(nullptr).ok);
    const int n = f.session.parse_count();
    EXPECT_TRUE(f.run(nullptr).ok);
    EXPECT_EQ(f.session.parse_count(), n);
}

} // namespace
