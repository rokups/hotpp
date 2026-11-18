// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of the name resolver of hot-ls (ref_resolver.h). They need no clangd and no files.
#include "ref_resolver.h"
#include "script_text.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

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

namespace
{

// The table of a small script for rename_conflict.
struct ConflictScript
{
    std::string text = "class A { int hp; int mp; void f(int x) { int y = x; return hp + G; } }\n"
                       "class B : A { int zz; }\n"
                       "int G;\n";
    FileRefs table;

    // The byte offset of the "n"-th whole word "w".
    uint32_t at(const std::string& w, int n = 0) const
    {
        for (size_t p = text.find(w); p != std::string::npos; p = text.find(w, p + 1))
        {
            const bool left = p == 0 || !std::isalnum(static_cast<unsigned char>(text[p - 1]));
            const bool right = p + w.size() >= text.size() || !std::isalnum(static_cast<unsigned char>(text[p + w.size()]));
            if (left && right && n-- == 0)
                return static_cast<uint32_t>(p);
        }
        return 0;
    }
    void add(const std::string& w, int n, const std::string& decl, bool is_decl, RefKind kind)
    {
        table.refs.push_back(ref(at(w, n), static_cast<uint32_t>(w.size()), decl, is_decl, kind));
        table.refs.back().pos = {1, 1};
    }
    ConflictScript()
    {
        table.file_id = "/a.hot";
        const std::string x = "@" + std::to_string(at("x")), y = "@" + std::to_string(at("y"));
        add("A", 0, "A", true, RefKind::Class);
        add("hp", 0, "A.hp", true, RefKind::Field);
        add("mp", 0, "A.mp", true, RefKind::Field);
        add("f", 0, "A.f", true, RefKind::Method);
        add("x", 0, x, true, RefKind::Param);
        add("y", 0, y, true, RefKind::Local);
        add("x", 1, x, false, RefKind::Param);
        add("hp", 1, "A.hp", false, RefKind::Field);
        add("G", 0, "G", false, RefKind::ModuleVar);
        add("B", 0, "B", true, RefKind::Class);
        add("A", 1, "A", false, RefKind::Class);
        add("zz", 0, "B.zz", true, RefKind::Field);
        add("G", 1, "G", true, RefKind::ModuleVar);
        table.bases = {{"A", ""}, {"B", "A"}};
    }
    std::string conflict(const std::string& w, int n, const std::string& new_name) const
    {
        const uint32_t off = at(w, n);
        for (const ResolvedRef& r : table.refs)
            if (r.offset == off)
                return hot::ls::rename_conflict({{&table, text}}, r, new_name);
        return "no ref";
    }
};

} // namespace

TEST(RefResolver, RenameConflictOfAMemberLooksAtTheClassFamily)
{
    const ConflictScript s;
    EXPECT_NE(s.conflict("hp", 0, "mp").find("/a.hot:1"), std::string::npos);
    EXPECT_NE(s.conflict("hp", 0, "zz").find("/a.hot:2"), std::string::npos); // a derived class
    EXPECT_EQ(s.conflict("hp", 0, "qq"), "");
    // The bare use of hp in A::f: the parameter x would hide the renamed member ("x = x").
    EXPECT_NE(s.conflict("hp", 0, "x").find("the local at /a.hot:1"), std::string::npos);
    // The bare use of the module variable G in A::f: the renamed member would hide it.
    EXPECT_NE(s.conflict("mp", 0, "G").find("the member would hide"), std::string::npos);
}

TEST(RefResolver, NativeBasesOfTheFamily)
{
    const ConflictScript s;
    auto bases = [&](const std::string& w, int n, FileRefs table)
    {
        const uint32_t off = s.at(w, n);
        for (const ResolvedRef& r : table.refs)
            if (r.offset == off)
                return hot::ls::native_bases_of_family({{&table, s.text}}, r);
        return std::vector<std::string>{"no ref"};
    };
    // No native base: A and B are script classes.
    EXPECT_TRUE(bases("hp", 0, s.table).empty());
    // A has the native base Node. B derives from A: the family has Node.
    FileRefs native = s.table;
    native.bases = {{"A", "Node"}, {"B", "A"}};
    EXPECT_EQ(bases("hp", 0, native), std::vector<std::string>{"Node"});
    EXPECT_EQ(bases("zz", 0, native), std::vector<std::string>{"Node"}); // a script chain ending in a native class
    // A module symbol and a local have no class family.
    EXPECT_TRUE(bases("G", 1, native).empty());
    EXPECT_TRUE(bases("y", 0, native).empty());
}

TEST(RefResolver, RenameConflictOfAModuleSymbolLooksAtTheModuleDeclarations)
{
    const ConflictScript s;
    EXPECT_NE(s.conflict("G", 1, "A").find("/a.hot:1"), std::string::npos);
    EXPECT_EQ(s.conflict("G", 1, "Q"), "");
    // The bare use of G in A::f: the member hp would hide the renamed G.
    EXPECT_NE(s.conflict("G", 1, "hp").find("the member at /a.hot:1"), std::string::npos);
    // The bare use of G in A::f: the parameter x would hide the renamed G.
    EXPECT_NE(s.conflict("G", 1, "x").find("the local at /a.hot:1"), std::string::npos);
}

TEST(RefResolver, RenameConflictOfALocalLooksAtTheFunction)
{
    const ConflictScript s;
    EXPECT_NE(s.conflict("y", 0, "x").find("declared"), std::string::npos); // the parameter
    EXPECT_NE(s.conflict("x", 0, "y").find("declared"), std::string::npos); // the local
    EXPECT_NE(s.conflict("y", 0, "hp").find("hide"), std::string::npos); // a bare use after the declaration
    EXPECT_EQ(s.conflict("y", 0, "mp"), "");
    EXPECT_EQ(s.conflict("y", 0, "zz"), ""); // after the end of the function
}

// A raw body: the translator does not resolve its words. A word "n" in a raw body can be a C++
// local, thus the rename of the module variable G to "n" refuses.
TEST(RefResolver, RenameConflictOfAModuleSymbolLooksAtTheRawBodies)
{
    FileRefs t;
    t.file_id = "/a.hot";
    const std::string text = "int G = 5;\nvoid f() raw { int n = 1; int x = G; }\n";
    ResolvedRef g = ref(static_cast<uint32_t>(text.find("G")), 1, "G", true, RefKind::ModuleVar);
    t.refs = {g, ref(static_cast<uint32_t>(text.find("f(")), 1, "f", true, RefKind::Function)};
    t.raw_ranges = {{static_cast<uint32_t>(text.find('{')), static_cast<uint32_t>(text.rfind('}') + 1)}};
    EXPECT_NE(hot::ls::rename_conflict({{&t, text}}, g, "n").find("/a.hot:2"), std::string::npos);
    EXPECT_EQ(hot::ls::rename_conflict({{&t, text}}, g, "q"), "");
}

// HOT-51: class_facts_hold is the rule that lets a completion use the facts of an older text. Each edit that the
// rule accepts must keep the classes, the bases and the member names. The test tries many edits at each place.
namespace
{

std::string class_signature(const hotc::AnalyzeResult& r)
{
    std::ostringstream o;
    for (const hotc::AnalyzedClass& c : r.classes)
    {
        o << c.name << ':' << c.base << ':' << c.is_interface << ':' << c.attributes.size() << '{';
        for (const hotc::AnalyzedMember& m : c.members)
            o << m.kind << ' ' << m.name << ';';
        o << '|';
        for (const hotc::AnalyzedMember& m : c.statics)
            o << m.kind << ' ' << m.name << ';';
        o << '}';
    }
    return o.str();
}

size_t check_class_facts_rule(const std::string& base, size_t* total, size_t stride)
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.generation = 1;
    const hotc::AnalyzeResult before = hotc::analyze(base, o);
    const hot::ls::Text analyzed(base);
    const std::string want = class_signature(before);
    size_t held = 0;
    const std::vector<std::string> inserts = {"x", "1", " ", ".", "(", ")", "[", "]", "=", "+", "<", ">", "!", "?", "&", "|",
        ":", ",", "class Zed", " struct Zed ", " interface Zed ", "enum Zed", "public ", "override ", "int z", "void g()",
        "namespace N", "using w", "x.y(", "(((", "))", "a ? b : c", "operator", "get", "set"};
    for (size_t at = 0; at <= base.size(); at += stride)
    {
        std::vector<std::string> edits;
        for (const std::string& ins : inserts)
            edits.push_back(base.substr(0, at) + ins + base.substr(at));
        for (size_t len = 1; len <= 3 && at + len <= base.size(); ++len)
            edits.push_back(base.substr(0, at) + base.substr(at + len));
        for (const std::string& text : edits)
        {
            ++*total;
            if (!hot::ls::class_facts_hold(analyzed, text, before))
                continue;
            ++held;
            const hotc::AnalyzeResult after = hotc::analyze(text, o);
            EXPECT_EQ(class_signature(after), want) << "edit at " << at << " of the text:\n" << text;
            if (::testing::Test::HasFailure())
                return held;
        }
    }
    return held;
}

} // namespace

TEST(ClassFactsHold, AcceptedEditsKeepTheClassFacts)
{
    const std::string base =
        "class Foo\n"
        "{\n"
        "    int hits = 0;\n"
        "    string name;\n"
        "    void Update(float dt)\n"
        "    {\n"
        "        var a = hits + 1;\n"
        "        if (a > 2) { hits = 0; }\n"
        "        for (int i = 0; i < 3; i++) hits += i;\n"
        "        log(\"a { b } // c\");\n"
        "        /* block { */\n"
        "        a.\n"
        "    }\n"
        "    Foo() { hits = 1; }\n"
        "    int Get() { return hits; }\n"
        "    void Broken() { var q = (1 + ; }\n"
        "}\n"
        "class Bar : Foo { void Go() { this.x(); } }\n";
    size_t total = 0;
    // HOT_FUZZ_STRIDE=1 tries each place (slow). The default tries each 60th place.
    const char* env = std::getenv("HOT_FUZZ_STRIDE");
    const size_t held = check_class_facts_rule(base, &total, env ? static_cast<size_t>(std::max(1, std::atoi(env))) : 60);
    EXPECT_GT(held, 0u);
    EXPECT_LT(held, total); // the rule is not a blanket yes
    std::printf("class_facts_hold: %zu of %zu edits accepted\n", held, total);
}

TEST(ClassFactsHold, EditsOutsideABodyAreNotAccepted)
{
    const std::string base = "class Foo\n{\n    int hits = 0;\n    void Update() { hits = 1; }\n}\n";
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.generation = 1;
    const hotc::AnalyzeResult facts = hotc::analyze(base, o);
    const hot::ls::Text analyzed(base);
    const auto hold = [&](const std::string& text) { return hot::ls::class_facts_hold(analyzed, text, facts); };
    EXPECT_TRUE(hold(base));
    EXPECT_TRUE(hold("class Foo\n{\n    int hits = 0;\n    void Update() { hits = 1 + x; }\n}\n"));
    EXPECT_FALSE(hold("class Foo\n{\n    int hits = 0;\n    int more;\n    void Update() { hits = 1; }\n}\n")); // a new line
    EXPECT_FALSE(hold("class Foo\n{\n    int hitz = 0;\n    void Update() { hits = 1; }\n}\n")); // a field name
    EXPECT_FALSE(hold("class Foo\n{\n    int hits = 0;\n    void Updat() { hits = 1; }\n}\n")); // a method name
    EXPECT_FALSE(hold("class Fo\n{\n    int hits = 0;\n    void Update() { hits = 1; }\n}\n")); // a class name
    EXPECT_FALSE(hold("class Foo\n{\n    int hits = 0;\n    void Update() { hits = 1; } }\n}\n")); // a brace
    EXPECT_FALSE(hold("class Foo\n{\n    int hits = 0;\n    void Update() { hits = 1;; }\n}\n")); // a semicolon
    EXPECT_FALSE(hold("class Foo\n{\n    int hits = 0;\n    void Update() { hits = 1; // }\n}\n")); // a comment sign
}
