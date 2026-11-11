// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The translate cache (hot/translate_cache.h): a hit gives the same result as a translate, and
// each changed input is a miss.
#include "hot/backend.h"
#include "hot/clang_files_backend.h"
#include "hot/translate_cache.h"
#include "hot/vfs.h"
#include "support/scratch_dir.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

// An interface edit of the import (a new method) is a miss.
TEST(TranslateCache, EditedImportMisses)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    std::string edited = kBase;
    edited.insert(edited.rfind("}"), "    int Extra() { return 3; }\n");
    write_file(f.base, edited);
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

// A white space edit outside a body of the import is a miss: the view has the text.
TEST(TranslateCache, WhiteSpaceEditOfAnImportMisses)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    write_file(f.base, std::string(kBase) + "\n");
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

// An importer stays a hit after a body edit of its import, and the hit is the result of a new
// translation: the C++ text, the warnings, the slots, the imports and the source map.
// "edited" is the new text of the import.
// With "moves" true, the edit moves a segment of the import: the hit must move it too.
void expect_importer_hit(const std::string& edited, bool moves)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    c.set_verify_hits(true);
    const hotc::TranslateResult before = f.fill(c, f.options);
    write_file(f.base, edited);
    const hotc::TranslateResult real = f.translate(f.options);
    ASSERT_NE(before.imports, real.imports) << "the import text changes";
    const std::optional<hotc::TranslateResult> hit = c.lookup(f.options);
    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(hit->ok) << hit->diagnostics;
    EXPECT_EQ(c.stats().view_hits, 1u);
    expect_same(*hit, real);
    size_t moved = 0;
    ASSERT_EQ(before.source_map.segments().size(), real.source_map.segments().size());
    for (size_t i = 0; i < real.source_map.segments().size(); ++i)
        moved += before.source_map.segments()[i].script_begin != real.source_map.segments()[i].script_begin;
    EXPECT_EQ(moved != 0, moves) << moved;
    // The hit has the searches of the current text.
    ASSERT_EQ(hit->import_lookups.size(), real.import_lookups.size());
    for (size_t i = 0; i < real.import_lookups.size(); ++i)
    {
        EXPECT_EQ(hit->import_lookups[i].view_hash, real.import_lookups[i].view_hash);
        EXPECT_EQ(hit->import_lookups[i].view_masked, real.import_lookups[i].view_masked);
    }
    // Each generated line maps to the same place, also a line that maps into the import.
    bool into_import = false;
    const int lines = static_cast<int>(std::count(real.cpp.begin(), real.cpp.end(), '\n'));
    for (int line = 1; line <= lines; ++line)
        // The columns reach past a long generated name, for example "hot_state_Base::Power".
        for (int col = 1; col <= 120; col += 3)
        {
            const std::optional<hotc::SourceLocation> a = hit->source_map.locate(line, col);
            const std::optional<hotc::SourceLocation> b = real.source_map.locate(line, col);
            ASSERT_EQ(a.has_value(), b.has_value()) << line << ":" << col;
            if (!a)
                continue;
            EXPECT_EQ(a->file, b->file);
            EXPECT_EQ(a->pos, b->pos) << line << ":" << col;
            into_import = into_import || a->file != 0;
        }
    EXPECT_TRUE(into_import) << "no generated place maps into the import";
    // A place of the import maps to the same generated place.
    for (int line = 1; line <= 12; ++line)
        for (int col = 1; col < 40; col += 3)
            EXPECT_EQ(hit->source_map.map_to_generated(1, line, col), real.source_map.map_to_generated(1, line, col))
                << line << ":" << col;
}

// The edit keeps the size of the body: no line of the import moves.
TEST(TranslateCache, ImporterHitsAfterABodyEditOfAnImport)
{
    std::string edited = kBase;
    edited.replace(edited.find("return 1; }\n}"), 9, "return 7;");
    expect_importer_hit(edited, false);
}

// The edit adds lines to the first body: the later method moves.
TEST(TranslateCache, ImporterHitsAfterABodyEditThatAddsLines)
{
    std::string edited = kBase;
    edited.replace(edited.find("{ return 1; }"), 13, "\n    {\n        int t = 1;\n        return t;\n    }");
    expect_importer_hit(edited, true);
}

// The check of a view hit finds an entry that differs from a new translate. The test changes a
// line of the stored C++ text and the hash in the manifest. The hit is then an error result.
// A warning of the importer can have a place in an import: here the borrowed pointer field of a
// class of the import. A body edit above the field keeps the view and moves the warning. So an
// entry with such a warning has no view hit: the lookup is a miss, not an old warning line.
TEST(TranslateCache, WarningInAnImportPreventsAViewHit)
{
    Fixture f;
    write_file(f.header, std::string(kHeader) + "struct Item { int v; };\n");
    make_old(f.header);
    const std::string base = "using \"Widget.h\";\nclass Base : Widget {\n    int Level() { return 1; }\n"
                             "    Item* p;\n}\n";
    write_file(f.base, base);
    hotc::TranslateOptions o = f.options;
    o.strict_borrowed = false;
    hot::TranslateCache c(f.cache);
    c.set_verify_hits(true);
    const hotc::TranslateResult before = f.fill(c, o);
    ASSERT_NE(before.warnings.find("base.hot:4:"), std::string::npos) << before.warnings;
    std::string edited = base;
    edited.replace(edited.find("{ return 1; }"), 13, "{\n        return 1;\n    }");
    write_file(f.base, edited);
    const hotc::TranslateResult real = f.translate(o);
    ASSERT_NE(real.warnings.find("base.hot:6:"), std::string::npos) << real.warnings;
    const std::optional<hotc::TranslateResult> hit = c.lookup(o);
    EXPECT_FALSE(hit.has_value()) << (hit ? hit->diagnostics + hit->warnings : std::string());
    EXPECT_EQ(c.stats().view_hits, 0u);
}

// Writes the files, fills the cache, writes the edit, and expects a view hit that is equal to a
// new translate.
void expect_view_hit(Fixture& f, const hotc::TranslateOptions& o, const std::string& path, const std::string& edited)
{
    hot::TranslateCache c(f.cache);
    c.set_verify_hits(true);
    const hotc::TranslateResult before = f.fill(c, o);
    write_file(path, edited);
    const hotc::TranslateResult real = f.translate(o);
    ASSERT_NE(before.imports, real.imports) << "the import text changes";
    const std::optional<hotc::TranslateResult> hit = c.lookup(o);
    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(hit->ok) << hit->diagnostics;
    EXPECT_EQ(c.stats().view_hits, 1u);
    expect_same(*hit, real);
}

// A body edit of the first import changes its size. The places of the second import come after it
// in the parse, so they move too. The hit must have the segments of a new translate.
TEST(TranslateCache, ImporterHitsWhenAnEarlierImportChangesItsSize)
{
    Fixture f;
    const std::string other = f.dir + "/other.hot";
    write_file(other, "using \"Widget.h\";\nclass Other : Widget {\n    int Get() { return 4; }\n"
                      "    int More() { return this.Get() + 1; }\n}\n");
    hotc::TranslateOptions o = f.options;
    o.script_text = "using \"Widget.h\";\nusing \"base.hot\";\nusing \"other.hot\";\n"
                    "class Kid : Base {\n    override int Power() { return 2; }\n"
                    "    int Speed($Other x) { if (x == null) return 0; return x.More() + this.Level(); }\n}\n";
    std::string edited = kBase;
    edited.replace(edited.find("{ return 1; }\n    int Level"), 13, "{\n        int t = 1;\n\n        return t;\n    }");
    expect_view_hit(f, o, f.base, edited);
}

// A body edit of an import of an import keeps the importer a hit.
TEST(TranslateCache, ImporterHitsAfterABodyEditOfATransitiveImport)
{
    Fixture f;
    const std::string c1 = f.dir + "/c1.hot";
    const std::string c1_text = "using \"Widget.h\";\nclass C1 : Widget {\n    int Twice() { return 2; }\n"
                                "    override int Power() { return 1; }\n}\n";
    write_file(c1, c1_text);
    write_file(f.base, "using \"Widget.h\";\nusing \"c1.hot\";\nclass Base : C1 {\n"
                       "    int Level() { return this.Twice(); }\n}\n");
    std::string edited = c1_text;
    edited.replace(edited.find("{ return 2; }"), 13, "{\n        int t = 2;\n        return t;\n    }");
    expect_view_hit(f, f.options, c1, edited);
}

TEST(TranslateCache, VerifiedViewHitFindsAWrongEntry)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    fs::path cpp, json;
    for (const auto& e : fs::directory_iterator(fs::path(f.cache) / "tr"))
    {
        if (e.path().extension() == ".cpp")
            cpp = e.path();
        else if (e.path().extension() == ".json")
            json = e.path();
    }
    ASSERT_FALSE(cpp.empty());
    ASSERT_FALSE(json.empty());
    std::string text, man;
    {
        std::ifstream a(cpp, std::ios::binary), b(json, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(a), {});
        man.assign(std::istreambuf_iterator<char>(b), {});
    }
    const auto hex = [](const std::string& t)
    {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hotc::text_hash(t)));
        return std::string(buf);
    };
    const size_t at = man.find(hex(text));
    ASSERT_NE(at, std::string::npos);
    const size_t line2 = text.find('\n') + 1;
    std::string wrong = text;
    wrong.insert(line2, "// wrong\n");
    man.replace(at, 16, hex(wrong));
    write_file(cpp.string(), wrong);
    write_file(json.string(), man);
    std::string edited = kBase;
    edited.replace(edited.find("return 1; }\n}"), 9, "return 7;");
    write_file(f.base, edited);

    c.set_verify_hits(false);
    const std::optional<hotc::TranslateResult> unchecked = c.lookup(f.options);
    ASSERT_TRUE(unchecked.has_value());
    EXPECT_TRUE(unchecked->ok);
    c.set_verify_hits(true);
    const std::optional<hotc::TranslateResult> checked = c.lookup(f.options);
    ASSERT_TRUE(checked.has_value());
    EXPECT_FALSE(checked->ok);
    EXPECT_NE(checked->diagnostics.find("cpp line 2"), std::string::npos) << checked->diagnostics;
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

// The generation is not in the key. An entry of generation 1 hits at generation 3. The hit has
// the slot output of a new translate at generation 3: the new lines have the number 3.
TEST(TranslateCache, LaterGenerationHitsTheTranslateCache)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    c.set_verify_hits(true);
    const hotc::TranslateResult gen1 = f.fill(c, f.options);
    hotc::TranslateOptions o = f.options;
    o.generation = 3;
    const hotc::TranslateResult real = f.translate(o);
    ASSERT_NE(gen1.slots_out, real.slots_out) << "the slot lines have the generation";
    const std::optional<hotc::TranslateResult> hit = c.lookup(o);
    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(hit->ok) << hit->diagnostics;
    EXPECT_EQ(hit->cpp, gen1.cpp);
    expect_same(*hit, real);
    EXPECT_EQ(c.stats().hits, 1u);
    // A translate at generation 0 fails, so the lookup misses.
    hotc::TranslateOptions zero = f.options;
    zero.generation = 0;
    EXPECT_FALSE(c.lookup(zero).has_value());
}

namespace
{

// The main script without the function Speed: the slot of Speed is defunct.
std::string main_without_speed()
{
    std::string s = kMain;
    const size_t at = s.find("    int Speed()");
    return s.erase(at, s.find('\n', at) + 1 - at);
}

// The main script with another signature of Speed.
std::string main_with_new_speed()
{
    std::string s = kMain;
    const std::string old = "int Speed() { return this.a; }";
    return s.replace(s.find(old), old.size(), "int Speed(int k) { return this.a + k; }");
}

// Stores "script" at generation 2 with the slot history of generation 1. A lookup at generation 3
// with the same history hits, and the hit is a new translate at generation 3 with that history.
void expect_history_hit(const std::string& script)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    c.set_verify_hits(true);
    const hotc::TranslateResult gen1 = f.translate(f.options);
    hotc::TranslateOptions o = f.options;
    o.script_text = script;
    o.slots_in = gen1.slots_out + gen1.sigs_out;
    o.generation = 2;
    f.fill(c, o);
    o.generation = 3;
    const hotc::TranslateResult real = f.translate(o);
    const std::optional<hotc::TranslateResult> hit = c.lookup(o);
    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(hit->ok) << hit->diagnostics;
    expect_same(*hit, real);
}

} // namespace

TEST(TranslateCache, LaterGenerationHitsWithADefunctSlot)
{
    expect_history_hit(main_without_speed());
}

TEST(TranslateCache, LaterGenerationHitsWithASignatureChange)
{
    expect_history_hit(main_with_new_speed());
}

// A slot history that changes the text is a miss: an entry without history (the functions are
// new, so they get thunks) and a lookup with the history of an earlier generation (no thunks).
TEST(TranslateCache, OtherSlotHistoryMissesAtALaterGeneration)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    c.set_verify_hits(true);
    const hotc::TranslateResult gen1 = f.fill(c, f.options);
    hotc::TranslateOptions o = f.options;
    o.slots_in = gen1.slots_out + gen1.sigs_out;
    o.generation = 3;
    ASSERT_NE(f.translate(o).cpp, gen1.cpp) << "the history changes the text";
    EXPECT_FALSE(c.lookup(o).has_value());

    // An entry with the history of generation 1, made at generation 2. At generation 1 the same
    // lines are of this generation, not older: a miss. Without the history (a restart): a miss.
    hot::TranslateCache d(f.scratch.file("cache2"));
    d.set_verify_hits(true);
    hotc::TranslateOptions o2 = o;
    o2.generation = 2;
    o2.script_text = main_without_speed();
    f.fill(d, o2);
    hotc::TranslateOptions same_gen = o2;
    same_gen.generation = 1;
    EXPECT_FALSE(d.lookup(same_gen).has_value());
    hotc::TranslateOptions restart = o2;
    restart.generation = 1;
    restart.slots_in.clear();
    EXPECT_FALSE(d.lookup(restart).has_value());
    EXPECT_EQ(d.stats().hits, 0u);
}

// The exported generation number is in the text, so another generation misses.
TEST(TranslateCache, ExportedGenerationMissesAtAnotherGeneration)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    hotc::TranslateOptions o = f.options;
    o.export_generation = true;
    f.fill(c, o);
    EXPECT_TRUE(c.lookup(o).has_value());
    o.generation = 3;
    EXPECT_FALSE(c.lookup(o).has_value());
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

// The compiler id is in the key of the translate cache. The compiler id has the variables that
// change the include path of the compiler and of libclang. A changed variable is a miss.
TEST(TranslateCacheBackend, IncludePathVariablesArePartOfTheKey)
{
    EnvGuard on("HOT_CACHE", "1");
    EnvGuard tr_on("HOT_TRANSLATE_CACHE", nullptr);
    EnvGuard cpath("CPATH", nullptr);
    EnvGuard cplus("CPLUS_INCLUDE_PATH", nullptr);
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string extra = scratch.file("extra_inc");
    fs::create_directories(extra);
    const hot::ScriptSource source = {"calc.hot", kActorScript};
    ASSERT_TRUE(make_backend(scratch, "w1", cache)->load(source, 1));
    int n = 2;
    for (const char* name : {"CPATH", "CPLUS_INCLUDE_PATH"})
    {
        EnvGuard changed(name, extra.c_str());
        auto backend = make_backend(scratch, "w" + std::to_string(n++), cache);
        ASSERT_TRUE(backend->load(source, 1)) << name;
        EXPECT_EQ(backend->cache_stats().translate_hits, 0u) << name;
        EXPECT_EQ(backend->cache_stats().translate_misses, 1u) << name;
    }
    auto backend = make_backend(scratch, "w9", cache);
    ASSERT_TRUE(backend->load(source, 1));
    EXPECT_EQ(backend->cache_stats().translate_hits, 1u);
}

namespace
{

const char* kFooHeader = "#pragma once\nstruct Foo\n{\n    virtual ~Foo() = default;\n"
                         "    virtual int Power() { return 0; }\n};\n";
const char* kOtherFoo = "#pragma once\nstruct Foo\n{\n    virtual ~Foo() = default;\n"
                        "    virtual float Power() { return 0.0f; }\n};\n";
const char* kFooScript = "using \"Foo.h\";\nclass Kid : Foo {\n    override int Power() { return 2; }\n}\n";

// A script in "src" and the include dirs "a" and "b". Only "b" has the header "Foo.h".
struct ShadowFixture
{
    TestScratch scratch;
    std::string src, a, b, cache;
    hotc::TranslateOptions options;

    ShadowFixture()
    {
        src = scratch.file("src");
        a = scratch.file("a");
        b = scratch.file("b");
        cache = scratch.file("cache");
        for (const std::string& d : {src, a, b})
            fs::create_directories(d);
        write_file(b + "/Foo.h", kFooHeader);
        make_old(b + "/Foo.h");
        options.script_text = kFooScript;
        options.script_name = src + "/main.hot";
        options.generation = 1;
        options.include_dirs = {a, b};
    }

    void fill(hot::TranslateCache& c, const hotc::TranslateOptions& o) const
    {
        const auto started = fs::file_time_type::clock::now();
        const hotc::TranslateResult r = hotc::translate(o);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        std::string error;
        ASSERT_TRUE(c.store(o, r, started, error)) << error;
        ASSERT_TRUE(c.lookup(o).has_value());
    }
};

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
        out = it->second;
        return true;
    }
    hot::VfsStat stat(const std::string& path) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {files_.count(path) != 0, 1};
    }
    void set(const std::string& path, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = text;
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> files_;
};

} // namespace

// A new header earlier in the include path hides the header that the parse used: a miss.
TEST(TranslateCache, NewShadowHeaderInAnEarlierIncludeDirMisses)
{
    ShadowFixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    write_file(f.a + "/Foo.h", kOtherFoo);
    make_old(f.a + "/Foo.h");
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

// The directory of the script comes first in the include path.
TEST(TranslateCache, NewShadowHeaderInTheScriptDirMisses)
{
    ShadowFixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    write_file(f.src + "/Foo.h", kOtherFoo);
    make_old(f.src + "/Foo.h");
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

// A new file of the host file system also hides the disk header.
TEST(TranslateCache, NewShadowHeaderInTheHostFileSystemMisses)
{
    ShadowFixture f;
    auto memory = std::make_shared<MemoryFs>();
    hotc::TranslateOptions o = f.options;
    o.fs = memory;
    hot::TranslateCache c(f.cache);
    f.fill(c, o);
    memory->set(f.a + "/Foo.h", kOtherFoo);
    EXPECT_FALSE(c.lookup(o).has_value());
}

// A header that a header includes has shadow paths too. "Foo.h" in "b" includes "Bar.h" in
// "c". A search for "Bar.h" tries "b", then the include path: "src", "a", "c".
TEST(TranslateCache, NewShadowOfANestedHeaderMisses)
{
    ShadowFixture f;
    const std::string c_dir = f.scratch.file("c");
    fs::create_directories(c_dir);
    write_file(f.b + "/Foo.h", "#pragma once\n#include \"Bar.h\"\nstruct Foo : Bar\n{\n};\n");
    write_file(c_dir + "/Bar.h", "#pragma once\nstruct Bar\n{\n    virtual ~Bar() = default;\n"
                                 "    virtual int Power() { return 0; }\n};\n");
    make_old(f.b + "/Foo.h");
    make_old(c_dir + "/Bar.h");
    hotc::TranslateOptions o = f.options;
    o.include_dirs = {f.a, f.b, c_dir};
    hot::TranslateCache c(f.cache);
    f.fill(c, o);
    write_file(f.a + "/Bar.h", "#pragma once\nstruct Bar\n{\n    virtual ~Bar() = default;\n"
                               "    virtual float Power() { return 0.0f; }\n};\n");
    make_old(f.a + "/Bar.h");
    EXPECT_FALSE(c.lookup(o).has_value());
}

// A new script earlier in the search path hides the imported script: a miss.
TEST(TranslateCache, NewShadowImportMisses)
{
    ShadowFixture f;
    write_file(f.b + "/base.hot", "using \"Foo.h\";\nclass Base : Foo {\n    override int Power() { return 1; }\n}\n");
    hotc::TranslateOptions o = f.options;
    o.script_text = "using \"Foo.h\";\nusing \"base.hot\";\nclass Kid : Base {\n    override int Power() { return 2; }\n}\n";
    hot::TranslateCache c(f.cache);
    f.fill(c, o);
    write_file(f.a + "/base.hot", "using \"Foo.h\";\nclass Base : Foo {\n    override int Power() { return 3; }\n}\n");
    EXPECT_FALSE(c.lookup(o).has_value());
}

// The script imports "b1.hot", "b1.hot" imports "c1.hot". An edit of "c1.hot" is a miss.
TEST(TranslateCache, EditedTransitiveImportMisses)
{
    ShadowFixture f;
    const std::string c1 = f.b + "/c1.hot";
    write_file(c1, "using \"Foo.h\";\nclass C1 : Foo {\n    override int Power() { return 1; }\n}\n");
    write_file(f.b + "/b1.hot", "using \"Foo.h\";\nusing \"c1.hot\";\nclass B1 : C1 {\n"
                                "    override int Power() { return 2; }\n}\n");
    hotc::TranslateOptions o = f.options;
    o.script_text = "using \"Foo.h\";\nusing \"b1.hot\";\nclass Kid : B1 {\n    override int Power() { return 3; }\n}\n";
    hot::TranslateCache c(f.cache);
    f.fill(c, o);
    write_file(c1, "using \"Foo.h\";\nclass C1 : Foo {\n    int extra = 4;\n    override int Power() { return 1; }\n}\n");
    EXPECT_FALSE(c.lookup(o).has_value());
}

// Translate 1 starts at T. The header changes during the libclang parse, so the session entry
// has the new stamp and maybe the old facts. Store 1 refuses. Translate 2 starts at T + 10 s
// and uses the same session entry. Store 2 must refuse too: the facts come from the old parse.
TEST(TranslateCache, ReusedSessionParseOfAYoungHeaderIsNotStored)
{
    ShadowFixture f;
    const std::string header = f.b + "/Foo.h";
    hotc::TranslatorSession session;
    hot::TranslateCache c(f.cache);
    const auto t = fs::file_time_type::clock::now();
    fs::last_write_time(header, t + std::chrono::milliseconds(200));
    const hotc::TranslateResult r1 = hotc::translate(f.options, session);
    ASSERT_TRUE(r1.ok) << r1.diagnostics;
    std::string error;
    EXPECT_FALSE(c.store(f.options, r1, t, error));
    ASSERT_EQ(session.parse_count(), 1);

    const hotc::TranslateResult r2 = hotc::translate(f.options, session);
    ASSERT_TRUE(r2.ok) << r2.diagnostics;
    ASSERT_EQ(session.parse_count(), 1); // the same session entry
    EXPECT_FALSE(c.store(f.options, r2, t + std::chrono::seconds(10), error));
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

namespace
{

// Include dirs like the dirs of a game: many dirs before the dir of the engine headers. Some
// dirs exist, some do not. "eng/Eng/All.h" includes 30 headers in "eng/Eng/Core".
struct ManyDirsFixture : ShadowFixture
{
    std::string eng;
    std::vector<std::string> early;

    ManyDirsFixture()
    {
        for (int i = 0; i < 24; ++i)
        {
            early.push_back(scratch.file("inc" + std::to_string(i)));
            if (i % 2 == 0)
                fs::create_directories(early.back());
        }
        eng = scratch.file("eng");
        std::string all = "#pragma once\n";
        for (int i = 0; i < 30; ++i)
        {
            const std::string h = eng + "/Eng/Core/Header" + std::to_string(i) + ".h";
            fs::create_directories(fs::path(h).parent_path());
            write_file(h, "#pragma once\nstruct Part" + std::to_string(i) + " {};\n");
            make_old(h);
            all += "#include \"Eng/Core/Header" + std::to_string(i) + ".h\"\n";
        }
        write_file(eng + "/Eng/All.h", all + kFooHeader);
        make_old(eng + "/Eng/All.h");
        options.script_text = "using \"Eng/All.h\";\nclass Kid : Foo {\n    override int Power() { return 2; }\n}\n";
        options.include_dirs = early;
        options.include_dirs.push_back(eng);
    }
};

} // namespace

// The manifest keeps the shadow paths in a short form. Each lookup reads the manifest, so its
// size is a cost of each warm start.
TEST(TranslateCache, ShadowPathsOfManyIncludeDirsHaveAShortForm)
{
    ManyDirsFixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    uintmax_t manifest_size = 0;
    for (const auto& e : fs::directory_iterator(f.cache + "/tr"))
        if (e.path().extension() == ".json")
            manifest_size += e.file_size();
    EXPECT_GT(manifest_size, 0u);
    EXPECT_LT(manifest_size, 16u * 1024u);
}

// A new header in a dir that did not exist at the store hides an engine header: a miss.
TEST(TranslateCache, NewShadowHeaderInANewDirMisses)
{
    ManyDirsFixture f;
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    const std::string h = f.early[5] + "/Eng/Core/Header7.h";
    fs::create_directories(fs::path(h).parent_path());
    write_file(h, "#pragma once\nstruct Part7 { int x; };\n");
    make_old(h);
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

// A new header in a dir that existed at the store hides an engine header: a miss.
TEST(TranslateCache, NewShadowHeaderInAnExistingDirMisses)
{
    ManyDirsFixture f;
    const std::string dir = f.early[4] + "/Eng";
    fs::create_directories(dir);
    hot::TranslateCache c(f.cache);
    f.fill(c, f.options);
    write_file(dir + "/All.h", std::string("#pragma once\n") + kOtherFoo);
    make_old(dir + "/All.h");
    EXPECT_FALSE(c.lookup(f.options).has_value());
}

namespace
{

const char* kActorBase = "using \"Actor.h\";\nclass Base : Actor {\n    float v = 0.0;\n"
                         "    override void Tick(float dt) { this.v = 1.0; }\n}\n";
// An interface edit: the field initializer of a class is in the importer output.
const char* kActorBaseEdited = "using \"Actor.h\";\nclass Base : Actor {\n    float v = 0.5;\n"
                               "    override void Tick(float dt) { this.v = 1.0; }\n}\n";
// A body edit: the importer output stays the same.
const char* kActorBaseBodyEdited = "using \"Actor.h\";\nclass Base : Actor {\n    float v = 0.0;\n"
                                   "    override void Tick(float dt)\n    {\n        this.v = 2.0;\n    }\n}\n";
const char* kActorMain = "using \"Actor.h\";\nusing \"base.hot\";\nclass Calc : Base {\n"
                         "    float r = 0.0;\n    override void Tick(float dt) { this.r = 3.0; }\n}\n";

size_t manifest_count(const std::string& cache)
{
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(cache) / "tr", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json")
            ++n;
    return n;
}

} // namespace

// Script A imports script B. A body edit of B is a miss for A, and the next start is a hit. A revert
// of B finds the entry of the first text: an import edit does not remove the earlier entry of A.
TEST(TranslateCacheBackend, ImporterHitsAfterAnImportEditAndARevert)
{
    EnvGuard on("HOT_CACHE", "1");
    EnvGuard tr_on("HOT_TRANSLATE_CACHE", nullptr);
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string dir = scratch.file("src");
    fs::create_directories(dir);
    const std::string base = dir + "/base.hot";
    const hot::ScriptSource source = {dir + "/main.hot", kActorMain};
    int n = 0;
    const auto load = [&](uint64_t& hits, uint64_t& misses)
    {
        auto backend = make_backend(scratch, "w" + std::to_string(n++), cache);
        hot::LoadResult r = backend->load(source, 1);
        ASSERT_TRUE(r) << r.error;
        hits = backend->cache_stats().translate_hits;
        misses = backend->cache_stats().translate_misses;
    };
    uint64_t hits = 0, misses = 0;
    write_file(base, kActorBase);
    load(hits, misses);
    EXPECT_EQ(misses, 1u) << "first text";
    write_file(base, kActorBaseEdited);
    load(hits, misses);
    EXPECT_EQ(misses, 1u) << "edited import";
    load(hits, misses);
    EXPECT_EQ(hits, 1u) << "the start after the edit";
    write_file(base, kActorBase);
    load(hits, misses);
    EXPECT_EQ(hits, 1u) << "reverted import";
    write_file(base, kActorBaseEdited);
    load(hits, misses);
    EXPECT_EQ(hits, 1u) << "edited import again";
}

// The backend translates the importer of a body-edited import from the cache. The check of the
// view hit runs (HOT_TRANSLATE_VERIFY), so a wrong hit fails the load.
TEST(TranslateCacheBackend, ImporterHitsAfterABodyEditOfAnImport)
{
    EnvGuard on("HOT_CACHE", "1");
    EnvGuard tr_on("HOT_TRANSLATE_CACHE", nullptr);
    EnvGuard verify("HOT_TRANSLATE_VERIFY", "1");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    const std::string dir = scratch.file("src");
    fs::create_directories(dir);
    const std::string base = dir + "/base.hot";
    const hot::ScriptSource source = {dir + "/main.hot", kActorMain};
    int n = 0;
    const auto load = [&](uint64_t& hits, uint64_t& misses)
    {
        auto backend = make_backend(scratch, "b" + std::to_string(n++), cache);
        hot::LoadResult r = backend->load(source, 1);
        ASSERT_TRUE(r) << r.error;
        hits = backend->cache_stats().translate_hits;
        misses = backend->cache_stats().translate_misses;
    };
    uint64_t hits = 0, misses = 0;
    write_file(base, kActorBase);
    load(hits, misses);
    EXPECT_EQ(misses, 1u) << "first text";
    write_file(base, kActorBaseBodyEdited);
    load(hits, misses);
    EXPECT_EQ(hits, 1u) << "body edit of the import";
    EXPECT_EQ(misses, 0u);
}

// A key keeps a small number of entries. A store after the last one removes the oldest entry. A
// store of a known import text replaces its old entry, and the number of entries stays the same.
TEST(TranslateCache, KeyKeepsTheNewestEntries)
{
    Fixture f;
    hot::TranslateCache c(f.cache);
    const auto base_text = [](int i)
    {
        return "using \"Widget.h\";\nclass Base : Widget {\n    int n = " + std::to_string(i)
            + ";\n    override int Power() { return 1; }\n}\n";
    };
    for (int i = 0; i < 5; ++i)
    {
        write_file(f.base, base_text(i));
        f.fill(c, f.options);
    }
    EXPECT_EQ(manifest_count(f.cache), 4u);
    write_file(f.base, base_text(0));
    EXPECT_FALSE(c.lookup(f.options).has_value()) << "the oldest entry is gone";
    for (int i = 1; i < 5; ++i)
    {
        write_file(f.base, base_text(i));
        EXPECT_TRUE(c.lookup(f.options).has_value()) << i;
    }
    // A store of a stale entry of a known text (a touched header) replaces that entry.
    write_file(f.base, base_text(2));
    f.fill(c, f.options);
    EXPECT_EQ(manifest_count(f.cache), 4u);
    for (int i = 1; i < 5; ++i)
    {
        write_file(f.base, base_text(i));
        EXPECT_TRUE(c.lookup(f.options).has_value()) << i;
    }
}

// T9: a parallel wave stores one entry for each unit. A second wave hits all and parses none.
TEST(TranslateCacheBackend, ParallelWaveStoresAndHits)
{
    EnvGuard on("HOT_CACHE", "1");
    EnvGuard tr_on("HOT_TRANSLATE_CACHE", nullptr);
    EnvGuard jobs("HOT_JOBS", "4");
    TestScratch scratch;
    const std::string cache = scratch.file("cache");
    constexpr size_t kUnits = 6;
    std::vector<hot::ScriptSource> sources;
    for (size_t i = 0; i < kUnits; ++i)
    {
        const std::string cls = "Calc" + std::to_string(i);
        sources.push_back({"calc" + std::to_string(i) + ".hot",
            "using \"Actor.h\";\nclass " + cls + " : Actor {\n    float result = 0.0;\n"
            "    override void Tick(float dt) { this.result = " + std::to_string(i) + ".0; }\n}\n",
            {}, cls});
    }
    for (int wave = 0; wave < 2; ++wave)
    {
        auto backend = make_backend(scratch, "w" + std::to_string(wave), cache);
        auto pending = backend->begin_load_many(sources, 1);
        ASSERT_EQ(pending.size(), kUnits);
        for (auto& p : pending)
            ASSERT_TRUE(p->ok) << p->head.error;
        EXPECT_EQ(backend->cache_stats().translate_hits, wave == 0 ? 0u : kUnits);
        EXPECT_EQ(backend->cache_stats().translate_misses, wave == 0 ? kUnits : 0u);
    }
}
