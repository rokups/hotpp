// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The positions of an import do not depend on the size of the main script. The first import starts at
// a fixed power of two, so an edit of the main script keeps the refs, the source map and the
// diagnostics of the import. The sizes are around the power of two (1 MiB).
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

using namespace translator_test;

constexpr size_t kMiB = 1u << 20;

// The main text with a trailing comment, so the whole text has exactly "size" bytes. A size smaller
// than the text plus the comment gives the text without padding.
std::string padded(const std::string& text, size_t size)
{
    if (size < text.size() + 3)
        return text;
    return text + "\n//" + std::string(size - text.size() - 3, 'x');
}

struct Case
{
    std::string main_text;
    std::vector<std::pair<std::string, std::string>> modules;
};

hotc::TranslateResult run(const TempDir& dir, const Case& c, size_t size)
{
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, padded(c.main_text, size));
    o.resolve_refs = true;
    for (const auto& m : c.modules)
        o.modules.emplace_back(m.first, m.second);
    return hotc::translate(o);
}

void expect_same(const hotc::TranslateResult& a, const hotc::TranslateResult& b, size_t size)
{
    EXPECT_EQ(a.ok, b.ok) << size;
    EXPECT_EQ(a.diagnostics, b.diagnostics) << size;
    EXPECT_EQ(a.warnings, b.warnings) << size;
    // The comment at the end is the only difference: it adds no line to the generated text.
    EXPECT_EQ(a.cpp, b.cpp) << size;
    ASSERT_EQ(a.refs.size(), b.refs.size()) << size;
    for (size_t i = 0; i < a.refs.size(); ++i)
    {
        EXPECT_EQ(a.refs[i].offset, b.refs[i].offset) << size << " ref " << i;
        EXPECT_EQ(a.refs[i].length, b.refs[i].length) << size << " ref " << i;
        EXPECT_EQ(a.refs[i].pos.line, b.refs[i].pos.line) << size << " ref " << i;
        EXPECT_EQ(a.refs[i].pos.col, b.refs[i].pos.col) << size << " ref " << i;
        EXPECT_EQ(a.refs[i].id, b.refs[i].id) << size << " ref " << i;
        EXPECT_EQ(a.refs[i].decl_file, b.refs[i].decl_file) << size << " ref " << i;
    }
    const auto& sa = a.source_map.segments();
    const auto& sb = b.source_map.segments();
    ASSERT_EQ(sa.size(), sb.size()) << size;
    for (size_t i = 0; i < sa.size(); ++i)
    {
        EXPECT_EQ(sa[i].file, sb[i].file) << size << " segment " << i;
        EXPECT_EQ(sa[i].script_begin, sb[i].script_begin) << size << " segment " << i;
        EXPECT_EQ(sa[i].script_len, sb[i].script_len) << size << " segment " << i;
        EXPECT_EQ(sa[i].gen_begin, sb[i].gen_begin) << size << " segment " << i;
        EXPECT_EQ(sa[i].gen_len, sb[i].gen_len) << size << " segment " << i;
    }
    ASSERT_EQ(a.source_map.files().size(), b.source_map.files().size()) << size;
    for (size_t i = 0; i < a.source_map.files().size(); ++i)
        EXPECT_EQ(a.source_map.files()[i].name, b.source_map.files()[i].name) << size;
}

void expect_stable(const Case& c)
{
    TempDir dir;
    const hotc::TranslateResult base = run(dir, c, 0);
    // The imports have segments: the comparison is not empty.
    EXPECT_GT(base.source_map.files().size(), 0u) << base.diagnostics;
    for (const size_t size : {kMiB - 1, kMiB, kMiB + 1, 2 * kMiB + 7})
        expect_same(base, run(dir, c, size), size);
}

const char* kLib = "using \"w.h\";\n"
                   "class Box<T> { T v; T Get() => v; }\n"
                   "int Helper() { return 1; }\n"
                   "class Plain : Widget { int z = 2; int Z() { Box<int> b; return z + b.Get(); } }\n";

TEST(ImportOrigin, DirectImportWithAGenericClass)
{
    expect_stable({"using \"w.h\";\nusing \"lib.hot\";\n"
                   "class Kid : Plain { int Use() { return Z() + z; } }\n",
        {{"lib.hot", kLib}}});
}

TEST(ImportOrigin, IndirectImport)
{
    expect_stable({"using \"w.h\";\nusing \"mid.hot\";\n"
                   "class Kid : Mid { int Use() { return Get() + n; } }\n",
        {{"mid.hot",
             "using \"w.h\";\nusing \"lib.hot\";\n"
             "class Mid : Widget { int n = 0; int Get() { Box<int> b; return b.Get() + Helper(); } }\n"},
            {"lib.hot", kLib}}});
}

TEST(ImportOrigin, ErrorInAnImportKeepsItsPosition)
{
    expect_stable({"using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Plain { }\n",
        {{"lib.hot", "using \"w.h\";\nclass Plain : Widget {\n    Missing m;\n}\n"}}});
}

TEST(ImportOrigin, ErrorInTheMainScriptKeepsItsPosition)
{
    expect_stable({"using \"w.h\";\nusing \"lib.hot\";\nclass Kid : Plain { int Use() { return nope; } }\n",
        {{"lib.hot", kLib}}});
}

} // namespace
