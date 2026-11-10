// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The translate cache (hot/translate_cache.h): a hit gives the same result as a translate, and
// each changed input is a miss.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/translate_cache.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{

namespace fs = std::filesystem;

using test_support::TestScratch;

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

void write_file(const std::string& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// A file time one hour ago: the store does not take a header that changed near the translate.
void make_old(const std::string& path, int seconds_back = 3600)
{
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::seconds(seconds_back));
}

const char* kHeader = "#pragma once\nstruct Widget\n{\n    virtual ~Widget() = default;\n"
                      "    virtual int Power() { return 0; }\n};\n";
const char* kBase = "using \"Widget.h\";\nclass Base : Widget {\n    override int Power() { return 1; }\n"
                    "    int Level() { return 1; }\n}\n";
const char* kMain = "using \"Widget.h\";\nusing \"base.hot\";\n"
                    "class Kid : Base {\n    int a = 0;\n    override int Power() { return 2; }\n"
                    "    int Speed() { return this.a; }\n}\n";

// A script with an import and a native include, in a folder of the scratch directory.
struct Fixture
{
    TestScratch scratch;
    std::string dir, header, base, cache;
    hotc::TranslateOptions options;

    Fixture()
    {
        dir = scratch.file("src");
        fs::create_directories(dir);
        header = dir + "/Widget.h";
        base = dir + "/base.hot";
        cache = scratch.file("cache");
        write_file(header, kHeader);
        write_file(base, kBase);
        make_old(header);
        options.script_text = kMain;
        options.script_name = dir + "/main.hot";
        options.generation = 1;
        options.include_dirs = {dir};
    }

    hotc::TranslateResult translate(const hotc::TranslateOptions& o) const
    {
        hotc::TranslateResult r = hotc::translate(o);
        EXPECT_TRUE(r.ok) << r.diagnostics;
        return r;
    }

    // Translates and stores. Gives the result of the translate.
    hotc::TranslateResult fill(hot::TranslateCache& c, const hotc::TranslateOptions& o) const
    {
        const auto started = fs::file_time_type::clock::now();
        hotc::TranslateResult r = translate(o);
        std::string error;
        EXPECT_TRUE(c.store(o, r, started, error)) << error;
        return r;
    }
};

void expect_same(const hotc::TranslateResult& a, const hotc::TranslateResult& b)
{
    EXPECT_EQ(a.ok, b.ok);
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_EQ(a.slots_out, b.slots_out);
    EXPECT_EQ(a.sigs_out, b.sigs_out);
    EXPECT_EQ(a.diagnostics, b.diagnostics);
    EXPECT_EQ(a.warnings, b.warnings);
    EXPECT_EQ(a.imports, b.imports);
    EXPECT_EQ(a.virtual_files, b.virtual_files);
    EXPECT_EQ(a.dependencies, b.dependencies);
    EXPECT_EQ(a.iface, b.iface);
    EXPECT_EQ(a.iface_hash, b.iface_hash);
    EXPECT_EQ(a.import_hashes, b.import_hashes);
    EXPECT_EQ(a.unit, b.unit);
    EXPECT_EQ(a.iface_header, b.iface_header);
    EXPECT_EQ(a.slot_classes, b.slot_classes);
    EXPECT_EQ(a.slot_keys, b.slot_keys);
    ASSERT_EQ(a.import_lookups.size(), b.import_lookups.size());
    for (size_t i = 0; i < a.import_lookups.size(); ++i)
    {
        EXPECT_EQ(a.import_lookups[i].file, b.import_lookups[i].file);
        EXPECT_EQ(a.import_lookups[i].text_hash, b.import_lookups[i].text_hash);
    }
    const auto& sa = a.source_map.segments();
    const auto& sb = b.source_map.segments();
    ASSERT_EQ(sa.size(), sb.size());
    for (size_t i = 0; i < sa.size(); ++i)
    {
        EXPECT_EQ(sa[i].script_begin, sb[i].script_begin);
        EXPECT_EQ(sa[i].script_len, sb[i].script_len);
        EXPECT_EQ(sa[i].gen_begin, sb[i].gen_begin);
        EXPECT_EQ(sa[i].gen_len, sb[i].gen_len);
        EXPECT_EQ(sa[i].file, sb[i].file);
    }
    ASSERT_EQ(a.source_map.files().size(), b.source_map.files().size());
    EXPECT_EQ(a.source_map.generic_names(), b.source_map.generic_names());
}

} // namespace

TEST(TranslateCache, HitEqualsTheTranslate)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    const hotc::TranslateResult real = f.fill(c, f.options);
    ASSERT_FALSE(real.dependencies.empty());
    ASSERT_EQ(real.imports.size(), 1u);
    const std::optional<hotc::TranslateResult> hit = c.lookup(f.options);
    ASSERT_TRUE(hit.has_value());
    expect_same(*hit, real);
    // A position maps in the same way.
    EXPECT_EQ(hit->source_map.map_to_generated(5, 30).has_value(), real.source_map.map_to_generated(5, 30).has_value());
    EXPECT_EQ(c.stats().hits, 1u);
}

TEST(TranslateCache, EditedScriptMisses)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    hotc::TranslateOptions o = f.options;
    o.script_text += "\n";
    EXPECT_FALSE(c.lookup(o).has_value());
    EXPECT_EQ(c.stats().misses, 1u);
}

TEST(TranslateCache, EditedImportMisses)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    write_file(f.base, std::string(kBase) + "\n");
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

TEST(TranslateCache, TouchedHeaderMisses)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    ASSERT_TRUE(c.lookup(f.options).has_value());
    make_old(f.header, 1800); // the same text, a new time
    EXPECT_FALSE(c.lookup(f.options).has_value());
    f.fill(c, f.options);
    ASSERT_TRUE(c.lookup(f.options).has_value());
    write_file(f.header, std::string(kHeader) + "// more\n"); // a new size
    make_old(f.header, 1800);
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

// The slot lines of the classes of the script change the output. The lines of the other
// classes go through, so a hit gives the output of a translate with the new slot text.
TEST(TranslateCache, SlotLinesOfTheScriptClassesAreChecked)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    const hotc::TranslateResult gen1 = f.translate(f.options);
    const std::string other = "Other::f 1\nsig Other::f void(Other_Shell*)\nfield Other::x int\n";
    hotc::TranslateOptions o = f.options;
    o.generation = 2;
    o.slots_in = gen1.slots_out + gen1.sigs_out + other;
    f.fill(c, o);
    ASSERT_TRUE(c.lookup(o).has_value());

    // A changed line of another class: a hit, the same as a translate.
    hotc::TranslateOptions other_changed = o;
    other_changed.slots_in = gen1.slots_out + gen1.sigs_out + "Other::g 1\nsig Other::g int(Other_Shell*)\n";
    const std::optional<hotc::TranslateResult> hit = c.lookup(other_changed);
    ASSERT_TRUE(hit.has_value());
    expect_same(*hit, f.translate(other_changed));

    // A changed line of an own class (Kid) and of an imported class (Base): a miss.
    hotc::TranslateOptions own_changed = o;
    own_changed.slots_in = "Kid::Extra 1\n" + o.slots_in;
    EXPECT_FALSE(c.lookup(own_changed).has_value());
    hotc::TranslateOptions base_changed = o;
    base_changed.slots_in = "Base::Extra 1\n" + o.slots_in;
    EXPECT_FALSE(c.lookup(base_changed).has_value());
}

TEST(TranslateCache, OtherBuildIdMisses)
{
    Fixture f;
    hot::TranslateCache a(f.cache, "id-a");
    hot::TranslateCache b(f.cache, "id-b");
    f.fill(a, f.options);
    EXPECT_TRUE(a.lookup(f.options).has_value());
    EXPECT_FALSE(b.lookup(f.options).has_value());
    EXPECT_FALSE(std::string(hotc::translator_build_id()).empty());
}

// A field rename gives a warning. A hit gives the same warning.
TEST(TranslateCache, HitKeepsTheWarnings)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    const hotc::TranslateResult gen1 = f.translate(f.options);
    hotc::TranslateOptions o = f.options;
    o.generation = 2;
    o.slots_in = gen1.slots_out + gen1.sigs_out;
    o.script_text = std::string(kMain).replace(std::string(kMain).find("int a"), 5, "int b");
    const hotc::TranslateResult real = f.fill(c, o);
    ASSERT_NE(real.warnings.find("rename"), std::string::npos) << real.warnings;
    const std::optional<hotc::TranslateResult> hit = c.lookup(o);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->warnings, real.warnings);
}

TEST(TranslateCache, CorruptEntryMisses)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    const std::string key = c.make_key(f.options).hex();
    const std::string tr = f.cache + "/tr/" + key;
    write_file(tr + ".cpp", "int broken;\n");
    EXPECT_FALSE(c.lookup(f.options).has_value());
    f.fill(c, f.options);
    ASSERT_TRUE(c.lookup(f.options).has_value());
    write_file(tr + ".json", "{\"version\": 1, \"key\": ");
    EXPECT_FALSE(c.lookup(f.options).has_value());
    f.fill(c, f.options);
    write_file(tr + ".data", "{}");
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

TEST(TranslateCache, FailedTranslateHasNoEntry)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    hotc::TranslateOptions o = f.options;
    o.script_text = "class Kid : Missing {\n}\n";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_FALSE(r.ok);
    std::string error;
    EXPECT_FALSE(c.store(o, r, fs::file_time_type::clock::now(), error));
    EXPECT_FALSE(c.lookup(o).has_value());
}

namespace
{

const char* kActorScript = "using \"Actor.h\";\nclass Calc : Actor {\n    float result = 0.0;\n"
                           "    override void Tick(float dt) { this.result = 3.0; }\n}\n";

std::unique_ptr<hot::ClangFilesBackend> make_backend(
    const TestScratch& scratch, const std::string& work, const std::string& cache, const std::string& id = "")
{
    hot::ClangFilesOptions o;
    o.compiler = HOT_CXX;
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"};
    o.work_dir = scratch.file(work);
    o.cache_dir = cache;
    o.translator_id = id;
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

} // namespace

TEST(TranslateCacheBackend, SecondBackendHits)
{
    EnvGuard on("HOT_CACHE", "1");
    EnvGuard tr_on("HOT_TRANSLATE_CACHE", nullptr);
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const hot::ScriptSource source = {"calc.hot", kActorScript};
    {
        auto backend = make_backend(scratch, "w1", cache);
        ASSERT_TRUE(backend->load(source, 1)) << "first load";
        EXPECT_EQ(backend->cache_stats().translate_misses, 1u);
    }
    auto backend = make_backend(scratch, "w2", cache);
    hot::LoadResult r = backend->load(source, 1);
    ASSERT_TRUE(r) << r.error;
    EXPECT_EQ(backend->cache_stats().translate_hits, 1u);
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    // An other translator build id misses.
    auto other = make_backend(scratch, "w3", cache, "other-id");
    ASSERT_TRUE(other->load(source, 1));
    EXPECT_EQ(other->cache_stats().translate_hits, 0u);
    EXPECT_EQ(other->cache_stats().translate_misses, 1u);
    EXPECT_EQ(other->cache_stats().hits, 1u); // the same text gives the same object
}

TEST(TranslateCacheBackend, EnvironmentTurnsOnlyTheTranslateCacheOff)
{
    EnvGuard on("HOT_CACHE", "1");
    EnvGuard tr_off("HOT_TRANSLATE_CACHE", "0");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const hot::ScriptSource source = {"calc.hot", kActorScript};
    ASSERT_TRUE(make_backend(scratch, "w1", cache)->load(source, 1));
    auto backend = make_backend(scratch, "w2", cache);
    ASSERT_TRUE(backend->load(source, 1));
    EXPECT_EQ(backend->cache_stats().hits, 1u);
    EXPECT_EQ(backend->cache_stats().translate_hits, 0u);
    EXPECT_EQ(backend->cache_stats().translate_misses, 0u);
    EXPECT_FALSE(fs::exists(fs::path(cache) / "tr"));
}
