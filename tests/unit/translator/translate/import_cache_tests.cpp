// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the import cache of the translator session (TranslateOptions::import_cache). The output
// must not change with the flag. A second translate with the flag skips the parse of an
// unchanged import.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace translator_test;

const char* kA =
    "using \"w.h\";\n"
    "class Base : Widget {\n"
    "    int hp = 1;\n"
    "    virtual int Power() { return 1; }\n"
    "    int Twice() { return this.hp * 2; }\n"
    "}\n"
    "struct Pt {\n    int v = 1;\n    int Get() { return this.v; }\n}\n"
    "const int K = 3;\n"
    "int Helper(int a = 1) { return a; }\n"
    "T Twice2<T>(T x) => x + x;\n"
    "int UseGeneric() { return Twice2<int>(2); }\n";

const char* kB =
    "using \"w.h\";\n"
    "class Other : Widget {\n    int z = 2;\n    int Get() { return this.z; }\n}\n";

const char* kImporter =
    "using \"w.h\";\n"
    "using \"a.hot\";\n"
    "using \"b.hot\";\n"
    "class Kid : Base {\n"
    "    override int Power() { return Helper() + K + this.Twice(); }\n"
    "    int Use() { Pt p; $Other o = new $Other(); return p.Get() + o.Get(); }\n"
    "}\n";

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

hotc::TranslateResult run(hotc::TranslatorSession& s, const TempDir& dir, const std::string& a, const std::string& b,
    const std::string& importer, bool cache, bool write_header = true)
{
    if (write_header) // threads must not rewrite a header that other threads read
        write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, importer);
    o.modules.emplace_back("a.hot", a);
    o.modules.emplace_back("b.hot", b);
    o.import_cache = cache;
    return hotc::translate(o, s);
}

void expect_same_output(const hotc::TranslateResult& x, const hotc::TranslateResult& y)
{
    EXPECT_EQ(x.ok, y.ok);
    EXPECT_EQ(x.cpp, y.cpp);
    EXPECT_EQ(x.iface_hash, y.iface_hash);
    EXPECT_EQ(x.import_hashes, y.import_hashes);
    EXPECT_EQ(x.diagnostics, y.diagnostics);
    EXPECT_EQ(x.warnings, y.warnings);
    EXPECT_EQ(x.source_map.generic_names(), y.source_map.generic_names());
}

TEST(ImportCache, FlagOffAndOnGiveTheSameOutput)
{
    TempDir dir;
    hotc::TranslatorSession s_off, s_on;
    const auto off = run(s_off, dir, kA, kB, kImporter, false);
    ASSERT_TRUE(off.ok) << off.diagnostics;
    const auto miss = run(s_on, dir, kA, kB, kImporter, true);
    const auto hit = run(s_on, dir, kA, kB, kImporter, true);
    expect_same_output(off, miss);
    expect_same_output(off, hit);
    EXPECT_EQ(off.timing.import_cache_hits + off.timing.import_cache_misses, 0);
    EXPECT_EQ(miss.timing.import_cache_hits, 0);
    EXPECT_EQ(miss.timing.import_cache_misses, 2);
    EXPECT_GT(miss.timing.import_parse_ns, 0);
    EXPECT_EQ(hit.timing.import_cache_hits, 2);
    EXPECT_EQ(hit.timing.import_cache_misses, 0);
    EXPECT_EQ(hit.timing.import_parse_ns, 0);
    EXPECT_FALSE(hit.source_map.generic_names().empty()) << "the test needs a generic display name";
    EXPECT_EQ(s_on.import_cache_size(), 2u);
    s_on.clear();
    EXPECT_EQ(s_on.import_cache_size(), 0u);
}

TEST(ImportCache, BodyEditMissesOnlyTheEditedUnit)
{
    TempDir dir;
    hotc::TranslatorSession s;
    run(s, dir, kA, kB, kImporter, true);
    const std::string edited = replaced(kA, "return this.hp * 2;", "return this.hp * 3;");
    const auto r = run(s, dir, edited, kB, kImporter, true);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.timing.import_cache_misses, 1);
    EXPECT_EQ(r.timing.import_cache_hits, 1);
    hotc::TranslatorSession fresh;
    expect_same_output(run(fresh, dir, edited, kB, kImporter, false), r);
}

TEST(ImportCache, InterfaceEditMissesAndChangesTheHash)
{
    TempDir dir;
    hotc::TranslatorSession s;
    const auto first = run(s, dir, kA, kB, kImporter, true);
    const std::string edited = replaced(kA, "int hp = 1;", "int hp = 1;\n    int more = 2;");
    const auto r = run(s, dir, edited, kB, kImporter, true);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_GE(r.timing.import_cache_misses, 1);
    EXPECT_NE(first.import_hashes, r.import_hashes);
    hotc::TranslatorSession fresh;
    expect_same_output(run(fresh, dir, edited, kB, kImporter, false), r);
}

TEST(ImportCache, ImporterEditHits)
{
    TempDir dir;
    hotc::TranslatorSession s;
    run(s, dir, kA, kB, kImporter, true);
    const std::string edited = replaced(kImporter, "this.Twice()", "this.Twice() + 1");
    const auto r = run(s, dir, kA, kB, edited, true);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.timing.import_cache_hits, 2);
    EXPECT_EQ(r.timing.import_parse_ns, 0);
    hotc::TranslatorSession fresh;
    expect_same_output(run(fresh, dir, kA, kB, edited, false), r);
}

TEST(ImportCache, ImportersWithTheSameBaseShareEntries)
{
    TempDir dir;
    hotc::TranslatorSession s;
    run(s, dir, kA, kB, kImporter, true);
    const std::string other =
        "using \"w.h\";\nusing \"a.hot\";\nusing \"b.hot\";\nclass Kid2 : Base {\n    int Use2() { return Helper(); }\n}\n";
    const auto r = run(s, dir, kA, kB, other, true);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.timing.import_cache_hits, 2);
    EXPECT_EQ(s.import_cache_size(), 2u);
}

TEST(ImportCache, ParseErrorLeavesNoEntryAndTheSameDiagnostics)
{
    TempDir dir;
    hotc::TranslatorSession s, s_off;
    const std::string bad = std::string(kB) + "class Broken { int x = ; }\n";
    const auto on = run(s, dir, kA, bad, kImporter, true);
    const auto off = run(s_off, dir, kA, bad, kImporter, false);
    EXPECT_FALSE(on.ok);
    expect_same_output(off, on);
    // The good import before the error has an entry. The bad import has none.
    EXPECT_EQ(s.import_cache_size(), 1u);
    const auto again = run(s, dir, kA, bad, kImporter, true);
    expect_same_output(off, again);
}

std::string read_all(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST(ImportCache, RepositoryScriptsGiveTheSameOutput)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::path(HOT_SOURCE_DIR) / "tests" / "scripts";
    int n = 0, with_imports = 0;
    for (const fs::path& dir : {root, root / "aot_units"})
        for (const auto& e : fs::directory_iterator(dir))
        {
            // The xmod scripts and the aot_units scripts have imports. Other scripts take long to parse.
            const std::string name = e.path().filename().string();
            if (e.path().extension() != ".hot" || (dir == root && name.rfind("xmod_", 0) != 0))
                continue;
            auto make = [&](bool cache)
            {
                hotc::TranslateOptions o;
                o.script_text = read_all(e.path());
                o.script_name = e.path().string();
                o.generation = 1;
                o.include_dirs = {(fs::path(HOT_SOURCE_DIR) / "tests" / "engine").string(),
                    (fs::path(HOT_SOURCE_DIR) / "include").string(), dir.string()};
                o.import_cache = cache;
                return o;
            };
            hotc::TranslatorSession s;
            const auto off = hotc::translate(make(false), s);
            const auto first = hotc::translate(make(true), s);
            const auto second = hotc::translate(make(true), s);
            SCOPED_TRACE(e.path().string());
            expect_same_output(off, first);
            expect_same_output(off, second);
            EXPECT_EQ(first.timing.import_cache_misses, second.timing.import_cache_hits);
            EXPECT_EQ(second.timing.import_cache_misses, 0);
            ++n;
            if (first.timing.import_cache_misses > 0)
                ++with_imports;
        }
    EXPECT_GT(n, 10);
    EXPECT_GT(with_imports, 5);
}

// A default argument is lowered in the context of each importer. The cached script must not share it.
const char* kDefLib =
    "using \"w.h\";\n"
    "enum E { X, Y }\n"
    "const int K = 3;\n"
    "using Num = int;\n"
    "class D : Widget {\n"
    "    int F(int a = K) { return a; }\n"
    "    int G(E e = E.X) { return 0; }\n"
    "    int H(Num n = 2 * K) { return n; }\n"
    "}\n";
const char* kDefImporter1 = "using \"w.h\";\nusing \"a.hot\";\nclass U1 : D { int R() { return this.F() + this.H(); } }\n";
const char* kDefImporter2 = "using \"w.h\";\nusing \"a.hot\";\nclass U2 : D { int R() { return this.F(1); } }\n"
                            "class U3 : D { override int G(E e = E.Y) { return 1; } }\n";
// A weak default with "new": check_weak_new reports it where the default is lowered.
const char* kBadDefLib = "using \"w.h\";\nclass D : Widget {\n    int F([weak] $D o = new $D()) { return 1; }\n}\n";

TEST(ImportCache, DefaultArgumentsGiveTheSameOutputForTwoImporters)
{
    TempDir dir;
    hotc::TranslatorSession s, s_off;
    for (const char* lib : {kDefLib, kBadDefLib})
        for (const char* importer : {kDefImporter1, kDefImporter2, kDefImporter1})
        {
            const auto off = run(s_off, dir, lib, kB, importer, false);
            const auto on = run(s, dir, lib, kB, importer, true);
            SCOPED_TRACE(std::string(lib) + importer);
            expect_same_output(off, on);
        }
    EXPECT_TRUE(run(s_off, dir, kDefLib, kB, kDefImporter1, false).ok);
    EXPECT_FALSE(run(s_off, dir, kBadDefLib, kB, kDefImporter1, false).ok);
}

TEST(ImportCache, TwoThreadsHitOneEntry)
{
    TempDir dir;
    hotc::TranslatorSession s, s_off;
    write_file(dir.path() / "w.h", kHeaderV1);
    const auto want1 = run(s_off, dir, kDefLib, kB, kDefImporter1, false);
    const auto want2 = run(s_off, dir, kDefLib, kB, kDefImporter2, false);
    run(s, dir, kDefLib, kB, kDefImporter1, true);
    std::vector<hotc::TranslateResult> got(8);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < got.size(); ++i)
        threads.emplace_back([&, i] { got[i] = run(s, dir, kDefLib, kB, i % 2 ? kDefImporter2 : kDefImporter1, true, false); });
    for (std::thread& t : threads)
        t.join();
    for (size_t i = 0; i < got.size(); ++i)
        expect_same_output(i % 2 ? want2 : want1, got[i]);
}

} // namespace
