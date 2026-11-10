// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// ObjectCache: the key, the depfile parser, store, lookup and trim.
#include "hot/object_cache.h"

#include "hot/runtime/process.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using hot::ObjectCache;

void write_text(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string read_text(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_files(const fs::path& dir)
{
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file())
            ++n;
    return n;
}

// A directory for the files of a test. The destructor removes it.
struct TestDir
{
    TestDir()
    {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
            ("hot_object_cache_test_" + std::to_string(hot::detail::process_id()) + "_" + std::to_string(counter++));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TestDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

ObjectCache::KeyInputs base_inputs()
{
    ObjectCache::KeyInputs in;
    in.cpp_text = "int main() { return 0; }\n";
    in.args = {"-c", "-O2", "-std=c++17"};
    in.compiler_id = "clang 18.1.0";
    in.virtual_files = {{"a.h", "int a;"}, {"b.h", "int b;"}};
    return in;
}

// A cache with one header and one object file, ready for store().
struct ObjectCacheStore : ::testing::Test
{
    TestDir root;
    fs::path header = root.path / "header.h";
    fs::path obj_tmp = root.path / "unit.o.tmpfile";
    std::string work_dir = (root.path / "work").string();
    ObjectCache cache{root.path / "cache", 1000000};
    ObjectCache::Key key = ObjectCache::make_key(base_inputs());

    void SetUp() override
    {
        fs::create_directories(work_dir);
        write_text(header, "#define X 1\n");
        write_text(obj_tmp, "OBJECT-BYTES");
    }
    std::string depfile() const { return "unit.o: " + (root.path / "work" / "unit.cpp").string() + " " + header.string() + "\n"; }
    bool store(std::string& error) { return cache.store(key, obj_tmp, depfile(), work_dir, error); }
    fs::path entry(const char* ext) const { return root.path / "cache" / "obj" / (key.hex() + ext); }
};

} // namespace

// ------------------------------------------------------------ Key

TEST(ObjectCacheKey, SameInputsGiveSameKey)
{
    const auto a = ObjectCache::make_key(base_inputs());
    const auto b = ObjectCache::make_key(base_inputs());
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.hex().size(), 32u);
}

TEST(ObjectCacheKey, VirtualFileOrderDoesNotMatter)
{
    auto in = base_inputs();
    const auto a = ObjectCache::make_key(in);
    std::swap(in.virtual_files[0], in.virtual_files[1]);
    EXPECT_EQ(a, ObjectCache::make_key(in));
}

TEST(ObjectCacheKey, EachInputChangesTheKey)
{
    const auto base = ObjectCache::make_key(base_inputs());

    auto in = base_inputs();
    in.args[1] = "-O0";
    EXPECT_NE(base, ObjectCache::make_key(in));

    in = base_inputs();
    in.cpp_text[3] ^= 1;
    EXPECT_NE(base, ObjectCache::make_key(in));

    in = base_inputs();
    in.compiler_id = "clang 18.1.1";
    EXPECT_NE(base, ObjectCache::make_key(in));

    in = base_inputs();
    in.virtual_files[1].second = "int c;";
    EXPECT_NE(base, ObjectCache::make_key(in));

    in = base_inputs();
    in.virtual_files[1].first = "c.h";
    EXPECT_NE(base, ObjectCache::make_key(in));
}

TEST(ObjectCacheKey, ArgumentBoundaryIsNotAmbiguous)
{
    auto x = base_inputs();
    auto y = base_inputs();
    x.args = {"ab", "c"};
    y.args = {"a", "bc"};
    EXPECT_NE(ObjectCache::make_key(x), ObjectCache::make_key(y));
}

TEST(ObjectCacheKey, NormalizeArgsRemovesTheWorkDirectory)
{
    const std::string work = "/tmp/hot_work/12345";
    const auto out = ObjectCache::normalize_args(
        {"-I/tmp/hot_work/12345/gen", "-c", "/tmp/hot_work/12345/unit.cpp", "-MF=/tmp/hot_work/12345/u.d",
            "/tmp/hot_work/123456/x", "-I/usr/include", "/tmp/hot_work/12345"},
        work + "/");
    ASSERT_EQ(out.size(), 7u);
    EXPECT_EQ(out[0], "-I@work@/gen");
    EXPECT_EQ(out[1], "-c");
    EXPECT_EQ(out[2], "@work@/unit.cpp");
    EXPECT_EQ(out[3], "-MF=@work@/u.d");
    EXPECT_EQ(out[4], "/tmp/hot_work/123456/x"); // another directory with the same prefix text
    EXPECT_EQ(out[5], "-I/usr/include");
    EXPECT_EQ(out[6], "@work@");
    // Two process ids give the same arguments.
    EXPECT_EQ(ObjectCache::normalize_args({"-I/tmp/hot_work/999/gen"}, "/tmp/hot_work/999"),
        ObjectCache::normalize_args({"-I/tmp/hot_work/12345/gen"}, "/tmp/hot_work/12345"));
}

TEST(ObjectCacheKey, NormalizeArgsAcceptsWindowsSeparators)
{
    const auto out = ObjectCache::normalize_args({"-IC:\\hot_work\\77\\gen", "C:/hot_work/77/unit.cpp"}, "C:\\hot_work\\77");
    EXPECT_EQ(out[0], "-I@work@\\gen");
    EXPECT_EQ(out[1], "@work@/unit.cpp");
}

TEST(ObjectCacheEnv, DisabledOnlyByZero)
{
    const char* old = getenv("HOT_CACHE");
    const bool had = old != nullptr;
    const std::string saved = old ? old : "";
    test_support::unset_env("HOT_CACHE");
    EXPECT_TRUE(ObjectCache::enabled_by_env());
    test_support::set_env("HOT_CACHE", "0");
    EXPECT_FALSE(ObjectCache::enabled_by_env());
    test_support::set_env("HOT_CACHE", "1");
    EXPECT_TRUE(ObjectCache::enabled_by_env());
    if (had)
        test_support::set_env("HOT_CACHE", saved.c_str());
    else
        test_support::unset_env("HOT_CACHE");
}

// ------------------------------------------------------------ Depfile

TEST(ObjectCacheDepfile, ParsesContinuationsAndEscapedSpaces)
{
    std::vector<std::string> deps;
    const std::string text =
        "out/unit.o: src/unit.cpp \\\n"
        "  /usr/include/my\\ dir/a.h \\\r\n"
        "  /usr/include/b.h\n"
        "\n"
        "/usr/include/b.h:\n";
    ASSERT_TRUE(ObjectCache::parse_depfile(text, deps));
    ASSERT_EQ(deps.size(), 3u);
    EXPECT_EQ(deps[0], "src/unit.cpp");
    EXPECT_EQ(deps[1], "/usr/include/my dir/a.h");
    EXPECT_EQ(deps[2], "/usr/include/b.h");
}

TEST(ObjectCacheDepfile, ParsesWindowsPaths)
{
    std::vector<std::string> deps;
    ASSERT_TRUE(ObjectCache::parse_depfile("C:/out/unit.o: C:/src/a.h C:\\\\src\\\\b.h C:\\src\\c.h\n", deps));
    ASSERT_EQ(deps.size(), 3u);
    EXPECT_EQ(deps[0], "C:/src/a.h");
    EXPECT_EQ(deps[1], "C:\\src\\b.h");
    EXPECT_EQ(deps[2], "C:\\src\\c.h");
}

TEST(ObjectCacheDepfile, RejectsTextWithoutTarget)
{
    std::vector<std::string> deps;
    EXPECT_FALSE(ObjectCache::parse_depfile("no colon here\n", deps));
    EXPECT_FALSE(ObjectCache::parse_depfile("", deps));
}

// ------------------------------------------------------------ Store and lookup

TEST_F(ObjectCacheStore, StoreThenLookupHits)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    EXPECT_FALSE(fs::exists(obj_tmp));
    const auto hit = cache.lookup(key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->object, entry(".o"));
    EXPECT_EQ(read_text(hit->object), "OBJECT-BYTES");
    EXPECT_TRUE(fs::exists(entry(".d")));
    EXPECT_NE(read_text(entry(".d")).find("header.h"), std::string::npos);
}

TEST_F(ObjectCacheStore, LookupOfUnknownKeyMisses)
{
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, ChangedMtimeMisses)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    fs::last_write_time(header, fs::last_write_time(header) + std::chrono::seconds(5));
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, ChangedSizeMisses)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    const auto t = fs::last_write_time(header);
    write_text(header, "#define X 12345\n");
    fs::last_write_time(header, t); // the same time: the size alone must decide
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, MissingHeaderMisses)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    fs::remove(header);
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, FileInWorkDirectoryHasNoStamp)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error; // unit.cpp in the work directory does not exist
    EXPECT_TRUE(cache.lookup(key).has_value());
    EXPECT_EQ(read_text(entry(".json")).find(work_dir), std::string::npos);
}

TEST_F(ObjectCacheStore, StoreFailsForMissingHeader)
{
    fs::remove(header);
    std::string error;
    EXPECT_FALSE(store(error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, EntryWithoutManifestMisses)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    fs::remove(entry(".json"));
    EXPECT_TRUE(fs::exists(entry(".o")));
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, LeftOverTempFileIsNotAnEntry)
{
    fs::create_directories(root.path / "cache" / "obj");
    write_text(entry(".o"), "partial");
    write_text(root.path / "cache" / "obj" / (key.hex() + ".json.tmp1.0"), "{}");
    EXPECT_FALSE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, CorruptManifestMisses)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    const std::string good = read_text(entry(".json"));
    for (const std::string& bad : {std::string(), std::string("not json"), good.substr(0, good.size() / 2),
             std::string("{\"version\":1,\"key\":\"x\",\"headers\":[]}"), std::string("{\"version\":1}"),
             std::string("[1,2,3]")})
    {
        write_text(entry(".json"), bad);
        EXPECT_FALSE(cache.lookup(key).has_value()) << bad;
    }
}

TEST_F(ObjectCacheStore, StatsCountHitsAndMisses)
{
    std::string error;
    EXPECT_FALSE(cache.lookup(key).has_value());
    ASSERT_TRUE(store(error)) << error;
    EXPECT_TRUE(cache.lookup(key).has_value());
    EXPECT_TRUE(cache.lookup(key).has_value());
    EXPECT_FALSE(cache.lookup(ObjectCache::Key{1, 2}).has_value());
    const auto s = cache.stats();
    EXPECT_EQ(s.hits, 2u);
    EXPECT_EQ(s.misses, 2u);
}

TEST_F(ObjectCacheStore, TwoStoresOfSameKeyKeepOneValidEntry)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    write_text(obj_tmp, "OBJECT-BYTES");
    ASSERT_TRUE(store(error)) << error;
    EXPECT_TRUE(cache.lookup(key).has_value());
    EXPECT_EQ(count_files(root.path / "cache" / "obj"), 3u); // .o, .d and .json: no temporary file
    EXPECT_EQ(read_text(entry(".o")), "OBJECT-BYTES");
}

TEST_F(ObjectCacheStore, HitTouchesTheManifest)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    fs::last_write_time(entry(".json"), fs::file_time_type::clock::now() - std::chrono::hours(5));
    const auto old = fs::last_write_time(entry(".json"));
    ASSERT_TRUE(cache.lookup(key).has_value());
    EXPECT_GT(fs::last_write_time(entry(".json")), old);
}

TEST_F(ObjectCacheStore, RemoveDeletesTheEntry)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    cache.remove(key);
    EXPECT_EQ(count_files(root.path / "cache" / "obj"), 0u);
    EXPECT_FALSE(cache.lookup(key).has_value());
}

// ------------------------------------------------------------ Trim

TEST_F(ObjectCacheStore, TrimRemovesOldestEntryOverTheCap)
{
    ObjectCache small(root.path / "cache", 2000);
    const ObjectCache::Key older{10, 20};
    const ObjectCache::Key newer{30, 40};
    std::string error;
    for (const ObjectCache::Key& k : {older, newer})
    {
        write_text(obj_tmp, std::string(1000, 'x'));
        ASSERT_TRUE(small.store(k, obj_tmp, depfile(), work_dir, error)) << error;
    }
    const fs::path obj = root.path / "cache" / "obj";
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(obj / (older.hex() + ".json"), now - std::chrono::hours(3));
    fs::last_write_time(obj / (newer.hex() + ".json"), now - std::chrono::hours(1));

    small.trim();
    EXPECT_FALSE(small.lookup(older).has_value());
    EXPECT_FALSE(fs::exists(obj / (older.hex() + ".o")));
    EXPECT_TRUE(small.lookup(newer).has_value());
}

TEST_F(ObjectCacheStore, TrimKeepsEntriesUnderTheCap)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    cache.trim();
    EXPECT_TRUE(cache.lookup(key).has_value());
}

TEST_F(ObjectCacheStore, TrimRemovesOnlyOldTemporaryFiles)
{
    std::string error;
    ASSERT_TRUE(store(error)) << error;
    const fs::path obj = root.path / "cache" / "obj";
    const fs::path young = obj / "aaaa.o.tmp1.0";
    const fs::path old = obj / "bbbb.o.tmp1.1";
    write_text(young, "y");
    write_text(old, "o");
    fs::last_write_time(old, fs::file_time_type::clock::now() - std::chrono::hours(3));
    cache.trim();
    EXPECT_TRUE(fs::exists(young));
    EXPECT_FALSE(fs::exists(old));
    EXPECT_TRUE(cache.lookup(key).has_value());
}
