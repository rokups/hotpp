// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of the name resolver of hot-ls (ref_resolver.h). They need no clangd and no files.
#include "ref_resolver.h"

#include <gtest/gtest.h>

using hot::ls::FileRefs;
using hot::ls::ImportGraph;
using hotc::RefKind;
using hotc::ResolvedRef;
using json = nlohmann::json;

namespace
{

ResolvedRef ref(uint32_t offset, uint32_t length, const std::string& decl, bool is_decl = false,
    RefKind kind = RefKind::Field)
{
    ResolvedRef r;
    r.offset = offset;
    r.length = length;
    r.kind = kind;
    r.is_decl = is_decl;
    r.decl = decl;
    r.decl_file = "/a.hot";
    r.id = r.decl_file + "#" + decl;
    return r;
}

FileRefs sample()
{
    FileRefs f;
    f.file_id = "/a.hot";
    f.refs = {ref(10, 2, "A.hp", true), ref(20, 4, "A.heal", true, RefKind::Method), ref(30, 2, "A.hp"),
        ref(40, 2, "A.hp")};
    return f;
}

json edit(int line, int ch, int len, const std::string& text)
{
    return {{"range", {{"start", {{"line", line}, {"character", ch}}}, {"end", {{"line", line}, {"character", ch + len}}}}},
        {"newText", text}};
}

} // namespace

TEST(RefResolver, RefAtFindsTheNameThatHoldsTheOffset)
{
    const FileRefs f = sample();
    EXPECT_EQ(hot::ls::ref_at(f, 9), nullptr);
    ASSERT_NE(hot::ls::ref_at(f, 10), nullptr);
    EXPECT_EQ(hot::ls::ref_at(f, 10)->decl, "A.hp");
    EXPECT_EQ(hot::ls::ref_at(f, 12)->decl, "A.hp"); // directly after the name
    EXPECT_EQ(hot::ls::ref_at(f, 13), nullptr);
    EXPECT_EQ(hot::ls::ref_at(f, 22)->decl, "A.heal");
    EXPECT_EQ(hot::ls::ref_at(f, 41)->offset, 40u);
    EXPECT_EQ(hot::ls::ref_at(f, 100), nullptr);
    EXPECT_EQ(hot::ls::ref_at(FileRefs{}, 0), nullptr);
}

TEST(RefResolver, SortRefsOrdersAnUnsortedTable)
{
    FileRefs f = sample();
    std::reverse(f.refs.begin(), f.refs.end());
    hot::ls::sort_refs(f);
    ASSERT_EQ(f.refs.size(), 4u);
    EXPECT_EQ(f.refs[0].offset, 10u);
    EXPECT_EQ(f.refs[3].offset, 40u);
    EXPECT_EQ(hot::ls::ref_at(f, 31)->offset, 30u);
}

TEST(RefResolver, PlacesAndDeclOfUseTheId)
{
    const FileRefs f = sample();
    const auto p = hot::ls::places(f, "/a.hot#A.hp");
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0]->offset, 10u);
    EXPECT_EQ(p[2]->offset, 40u);
    EXPECT_TRUE(hot::ls::places(f, "/b.hot#A.hp").empty());
    ASSERT_NE(hot::ls::decl_of(f, "/a.hot#A.heal"), nullptr);
    EXPECT_EQ(hot::ls::decl_of(f, "/a.hot#A.heal")->offset, 20u);
    EXPECT_EQ(hot::ls::decl_of(f, "/a.hot#B.x"), nullptr);
}

TEST(RefResolver, FileLocalKindsAreLocalAndParam)
{
    EXPECT_TRUE(hot::ls::file_local(RefKind::Local));
    EXPECT_TRUE(hot::ls::file_local(RefKind::Param));
    for (RefKind k : {RefKind::Field, RefKind::Property, RefKind::Method, RefKind::StaticMethod, RefKind::StaticField,
             RefKind::Const, RefKind::ModuleVar, RefKind::Function, RefKind::Class, RefKind::Interface, RefKind::Enum,
             RefKind::EnumValue})
        EXPECT_FALSE(hot::ls::file_local(k)) << static_cast<int>(k);
}

TEST(RefResolver, ImportersOfAnImportCycleEnds)
{
    ImportGraph g;
    g.set("a", {"b"});
    g.set("b", {"c"});
    g.set("c", {"a"});
    EXPECT_EQ(g.importers_of("a"), (std::vector<std::string>{"b", "c"}));
    EXPECT_EQ(g.importers_of("c"), (std::vector<std::string>{"a", "b"}));
    g.set("self", {"self"});
    EXPECT_TRUE(g.importers_of("self").empty());
}

TEST(RefResolver, ImportersOfADiamondGivesEachImporterOnce)
{
    ImportGraph g;
    g.set("top", {"left", "right"});
    g.set("left", {"base"});
    g.set("right", {"base"});
    g.set("other", {"x"});
    EXPECT_EQ(g.importers_of("base"), (std::vector<std::string>{"left", "right", "top"}));
    EXPECT_TRUE(g.importers_of("top").empty());
}

TEST(RefResolver, SetReplacesAndEraseRemovesTheImports)
{
    ImportGraph g;
    g.set("a", {"base"});
    g.set("b", {"a"});
    EXPECT_EQ(g.importers_of("base"), (std::vector<std::string>{"a", "b"}));
    g.set("a", {"other"});
    EXPECT_TRUE(g.importers_of("base").empty());
    EXPECT_EQ(g.importers_of("other"), (std::vector<std::string>{"a", "b"}));
    g.erase("b");
    EXPECT_EQ(g.importers_of("other"), (std::vector<std::string>{"a"}));
    g.erase("missing");
}

TEST(RefResolver, MergeEditsKeepsTheFirstEditOfARangeAndCountsTheConflicts)
{
    json changes = json::object();
    EXPECT_EQ(hot::ls::merge_edits(changes, "file:///a.hot", json::array({edit(1, 0, 2, "x")})), 0);
    EXPECT_EQ(hot::ls::merge_edits(changes, "file:///a.hot", json::array({edit(1, 0, 2, "y"), edit(1, 0, 2, "x")})), 1);
    const json& a = changes["file:///a.hot"];
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], edit(1, 0, 2, "x"));
}

TEST(RefResolver, MergeEditsSortsAndRemovesEqualEdits)
{
    json changes = json::object();
    hot::ls::merge_edits(changes, "file:///a.hot", json::array({edit(3, 4, 2, "x"), edit(1, 0, 2, "x")}));
    hot::ls::merge_edits(changes, "file:///a.hot", json::array({edit(1, 0, 2, "x"), edit(1, 8, 2, "x")}));
    hot::ls::merge_edits(changes, "file:///b.hot", json::array({edit(0, 0, 1, "x")}));
    const json& a = changes["file:///a.hot"];
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[0], edit(1, 0, 2, "x"));
    EXPECT_EQ(a[1], edit(1, 8, 2, "x"));
    EXPECT_EQ(a[2], edit(3, 4, 2, "x"));
    EXPECT_EQ(changes["file:///b.hot"].size(), 1u);
    hot::ls::merge_edits(changes, "file:///c.hot", json::array());
    EXPECT_TRUE(changes["file:///c.hot"].is_array());
}
