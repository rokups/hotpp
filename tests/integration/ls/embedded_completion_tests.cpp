// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// HOT-65 step S5: completion in the script blocks of a markup document (".rml").
// A test of a script field needs clangd and skips without it.
#include "integration/ls/ls_client.h"
#include "long_path.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using ls_test::json;
using ls_test::LsClient;
using translator_test::TempDir;

void write_bytes(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary) << text;
}

std::string uri_of(const std::string& path)
{
    return "file://" + hot::ls::long_path(path);
}

struct Comp
{
    TempDir dir;
    std::string path, uri;
    std::unique_ptr<LsClient> client;
    int version = 1;

    explicit Comp(const std::string& prefix = "using \"Actor.h\";\n")
    {
        const fs::path root = dir.path();
        fs::create_directories(root / "build");
        write_bytes(root / "build" / "CMakeCache.txt", "# cache\n");
        write_bytes(root / "prefix.txt", prefix);
        fs::create_directories(root / "hosts");
        json host = {{"version", 1}, {"name", "t"}, {"binaryDir", (root / "build").generic_string()},
            {"compiler", "/usr/bin/c++"}, {"std", "c++17"}, {"resourceDir", HOT_RESOURCE_DIR},
            {"sourceDir", root.generic_string()}, {"scriptDirs", json::array({root.generic_string()})},
            {"includeDirs", json::array({std::string(HOT_SOURCE_DIR) + "/tests/engine", std::string(HOT_SOURCE_DIR) + "/include"})}};
        host["embedded"] = json::array({{{"suffix", ".rml"}, {"tag", "script"}, {"prefixFile", "prefix.txt"}}});
        write_bytes(root / "hosts" / "t.json", host.dump());
        hot::ls::ServerOptions o;
        o.clangd = HOT_CLANGD_PATH;
        o.clangd_log = (root / "clangd.log").string();
        o.hosts_dir = (root / "hosts").string();
        o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/include", std::string(HOT_SOURCE_DIR) + "/tests/engine"};
        o.background_index = false;
        path = (root / "page.rml").string();
        uri = uri_of(path);
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(root.string()));
    }
    void wait_diag()
    {
        client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == uri; }, 120000);
    }
    void open(const std::string& text)
    {
        write_bytes(path, text);
        client->open(uri, text);
        wait_diag();
    }
    void edit(const std::string& text)
    {
        client->change(uri, ++version, text);
        wait_diag();
    }
    json ask(int line, int character)
    {
        return client->request("textDocument/completion",
            {{"textDocument", {{"uri", uri}}}, {"position", {{"line", line}, {"character", character}}}})["result"];
    }
};

bool has_label(const json& r, const std::string& label)
{
    const json& items = r.is_object() ? r["items"] : r;
    if (!items.is_array())
        return false;
    for (const json& it : items)
        if (it.value("label", std::string()) == label)
            return true;
    return false;
}

// Each edit of an item starts after line 0 column 0 (the clamp of the prefix).
void expect_no_prefix_edit(const json& r)
{
    if (r.is_null())
        return;
    for (const json& it : r.is_object() ? r["items"] : r)
    {
        if (it.contains("textEdit"))
        {
            const json& s = it["textEdit"]["range"]["start"];
            EXPECT_FALSE(s["line"] == 0 && s["character"] == 0) << it.dump();
        }
        for (const json& ed : it.value("additionalTextEdits", json::array()))
            EXPECT_FALSE(ed["range"]["start"]["line"] == 0 && ed["range"]["start"]["character"] == 0) << it.dump();
    }
}

#define REQUIRE_CLANGD() \
    do \
    { \
        if (std::string(HOT_CLANGD_PATH).empty()) \
            GTEST_SKIP() << "clangd is not installed"; \
    } while (0)

} // namespace

TEST(EmbeddedCompletion, IdentifierOfAClassDeclaredEarlier)
{
    REQUIRE_CLANGD();
    Comp c;
    const std::string line = "class Bar { void g() { Fo";
    c.open("<rml>\n<p>\xC3\xA4\xF0\x9F\x98\x80</p><script>class Foobar { int v; }\n" + line + " } }</script>\n</rml>\n");
    // Line 2 has no multi-byte text: the column counts bytes.
    const json r = c.ask(2, static_cast<int>(line.size()));
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "Foobar")) << r.dump().substr(0, 400);
    expect_no_prefix_edit(r);
}

TEST(EmbeddedCompletion, MemberAfterMultiByteTextOnTheLine)
{
    REQUIRE_CLANGD();
    Comp c;
    const std::string head = "<p>\xC3\xA4\xC3\xB6 \xF0\x9F\x98\x80</p><script>"; // 3 + 2 + 1 + 2 + 4 + 8 UTF-16 units
    const std::string body = "class Foo { int count; void f() { this.";
    c.open("<rml>\n" + head + body + " } }</script>\n</rml>\n");
    const json r = c.ask(1, 3 + 2 + 1 + 2 + 4 + 8 + static_cast<int>(body.size()));
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "count")) << r.dump().substr(0, 400);
    expect_no_prefix_edit(r);
}

TEST(EmbeddedCompletion, OnLineZeroRightAfterTheTag)
{
    REQUIRE_CLANGD();
    Comp c;
    const std::string body = "class Foobar { int v; } class Bar { void g() { Fo";
    c.open("<script>" + body + " } }</script>\n</head>\n");
    const json r = c.ask(0, 8 + static_cast<int>(body.size()));
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "Foobar")) << r.dump().substr(0, 400);
    expect_no_prefix_edit(r);
}

TEST(EmbeddedCompletion, NamesOfThePrefixComplete)
{
    REQUIRE_CLANGD();
    Comp c("class PreThing { int v; };\n");
    const std::string body = "class Bar { void g() { PreT";
    c.open("<rml>\n<script>\n" + body + " } }\n</script>\n</rml>\n");
    const json r = c.ask(2, static_cast<int>(body.size()));
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "PreThing")) << r.dump().substr(0, 400);
}

TEST(EmbeddedCompletion, MarkupAnswersNull)
{
    Comp c;
    c.open("<rml>\n<head><title>Foo</title>\n<script>\nclass Foo { }\n</script>\n</head>\n<body>Foo</body>\n");
    EXPECT_TRUE(c.ask(0, 2).is_null());
    EXPECT_TRUE(c.ask(1, 14).is_null());
    EXPECT_TRUE(c.ask(2, 3).is_null());
    EXPECT_TRUE(c.ask(6, 8).is_null());
    EXPECT_TRUE(c.ask(40, 0).is_null());
}

TEST(EmbeddedCompletion, UnsavedEditChangesTheAnswer)
{
    REQUIRE_CLANGD();
    Comp c;
    const std::string line = "class Bar { void g() { Fo";
    const std::string pre = "<rml>\n<script>\n";
    c.open(pre + "class Foobar { }\n" + line + " } }\n</script>\n</rml>\n");
    json r = c.ask(3, static_cast<int>(line.size()));
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "Foobar"));
    c.edit(pre + "class Fooqux { }\n" + line + " } }\n</script>\n</rml>\n");
    r = c.ask(3, static_cast<int>(line.size()));
    ASSERT_FALSE(r.is_null());
    EXPECT_FALSE(has_label(r, "Foobar")) << r.dump().substr(0, 400);
    EXPECT_TRUE(has_label(r, "Fooqux"));
}

// The "using" path completion has a textEdit range: it is in raw coordinates and never in the prefix.
TEST(EmbeddedCompletion, UsingPathRangeIsRaw)
{
    REQUIRE_CLANGD();
    Comp c;
    write_bytes(c.dir.path() / "yy.hot", "class Y { }\n");
    const std::string head = "<p>\xC3\xA4</p><script>"; // 3 + 1 + 4 + 8 UTF-16 units
    const std::string body = "using \"y";
    c.open("<rml>\n" + head + body + "\"; class Foo { }</script>\n</rml>\n");
    const int base = 3 + 1 + 4 + 8;
    const json r = c.ask(1, base + static_cast<int>(body.size()));
    ASSERT_FALSE(r.is_null());
    ASSERT_TRUE(has_label(r, "yy.hot")) << r.dump().substr(0, 400);
    for (const json& it : r["items"])
        if (it.contains("textEdit"))
        {
            EXPECT_EQ(it["textEdit"]["range"]["start"]["line"], 1) << it.dump();
            EXPECT_EQ(it["textEdit"]["range"]["start"]["character"], base + 7) << it.dump();
        }
    expect_no_prefix_edit(r);
}

// The first latency on a document of 300 lines: a member access and an identifier prefix, 20 requests each.
TEST(EmbeddedCompletion, TimingOn300Lines)
{
    REQUIRE_CLANGD();
    Comp c;
    std::string text = "<rml>\n<head>\n<script>\nclass Foo {\n    int count;\n";
    std::vector<std::string> rows;
    for (int i = 0; i < 280; ++i)
    {
        rows.push_back("    int m" + std::to_string(i) + "(int a) { return this.count + a + " + std::to_string(i) + "; }");
        text += rows.back() + "\n";
    }
    text += "}\n</script>\n</head>\n<body>\n</body>\n";
    c.open(text);
    for (const char* kind : {"member", "identifier"})
    {
        std::vector<double> ms;
        for (int i = 0; i < 20; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const int row = 5 + i * 10;
            const std::string& r = rows[static_cast<size_t>(row - 5)];
            const int col = std::string(kind) == "member" ? static_cast<int>(r.find("this.") + 5) : static_cast<int>(r.find("a +") + 1);
            const json res = c.ask(row, col);
            EXPECT_FALSE(res.is_null()) << kind << " " << i;
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        double sum = 0;
        for (double v : ms)
            sum += v;
        std::printf("TIMING completion %s mean %.1f ms p95 %.1f ms\n", kind, sum / ms.size(), ms[18]);
        EXPECT_LT(ms[18], 1000.0) << kind;
    }
}

// A whitespace position inside a block (an empty indented line, after "= ") completes.
TEST(EmbeddedCompletion, WhitespaceInsideABlockCompletes)
{
    REQUIRE_CLANGD();
    Comp c;
    c.open("<rml>\n<script>\nclass Foobar { int v; }\nclass Bar {\n    void g() {\n        \n        int x = \n    }\n}\n</script>\n</rml>\n");
    json r = c.ask(5, 8);
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "Foobar")) << r.dump().substr(0, 300);
    r = c.ask(6, 16);
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "Foobar")) << r.dump().substr(0, 300);
    expect_no_prefix_edit(r);
}

TEST(EmbeddedCompletion, BlockBoundaries)
{
    REQUIRE_CLANGD();
    Comp c;
    const std::string text = "<rml>\n<script>class Foobar { }\n</script>\n<p>Foo</p>\n<script>class Baz { void f() { Fo } }</script></rml>\n";
    c.open(text);
    EXPECT_TRUE(c.ask(3, 5).is_null());                 // markup between blocks
    EXPECT_TRUE(c.ask(1, 4).is_null());                 // inside the open tag
    EXPECT_TRUE(c.ask(1, 7).is_null());                 // before the ">" of the open tag
    EXPECT_TRUE(c.ask(0, 2).is_null());
    EXPECT_FALSE(c.ask(2, 0).is_null());                // right at "</script"
    EXPECT_TRUE(c.ask(2, 3).is_null());                 // inside the end tag
    const std::string l4 = "<script>class Baz { void f() { Fo } }</script></rml>";
    EXPECT_FALSE(c.ask(4, static_cast<int>(l4.find("</script"))).is_null());
    EXPECT_TRUE(c.ask(4, static_cast<int>(l4.find("</rml"))).is_null());
}

// An empty block (a "src" block) has no code: a request between its tags answers null and the server lives on.
TEST(EmbeddedCompletion, EmptyBlockAfterTheLastCodeBlockAnswersNull)
{
    REQUIRE_CLANGD();
    Comp c;
    c.open("<rml>\n<script>class Foobar { }</script>\n<p>a</p>\n<p>b</p>\n<script src=\"x\"></script>\n</rml>\n");
    const std::string l = "<script src=\"x\"></script>";
    EXPECT_TRUE(c.ask(4, static_cast<int>(l.find("</script"))).is_null());
    EXPECT_TRUE(c.client->request("textDocument/hover", {{"textDocument", {{"uri", c.uri}}},
        {"position", {{"line", 4}, {"character", static_cast<int>(l.find("</script"))}}}})["result"].is_null());
    c.edit("<rml>\n<script>class Foobaz { }</script>\n<p>a</p>\n<p>b</p>\n<script src=\"x\"></script>\n</rml>\n");
    const json r = c.ask(1, 8 + 10);
    EXPECT_FALSE(r.is_null());
}

TEST(EmbeddedCompletion, OnlyBlockEmptyAnswersNull)
{
    Comp c;
    c.open("<rml>\n<p>a</p>\n<p>b</p>\n<script src=\"x\"></script>\n</rml>\n");
    const std::string l = "<script src=\"x\"></script>";
    EXPECT_TRUE(c.ask(3, static_cast<int>(l.find("</script"))).is_null());
    c.edit("<rml>\n<p>a</p>\n<p>b</p>\n<script src=\"x\"></script>\n<p>c</p>\n</rml>\n");
    EXPECT_TRUE(c.ask(3, static_cast<int>(l.find("</script"))).is_null());
}

TEST(EmbeddedCompletion, CrLfLineEnds)
{
    REQUIRE_CLANGD();
    Comp c;
    c.open("<rml>\r\n<script>\r\nclass Foobar { int v; }\r\nclass Bar {\r\n    void g() {\r\n        \r\n    }\r\n}\r\n</script>\r\n</rml>\r\n");
    const json r = c.ask(5, 8);
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(has_label(r, "Foobar")) << r.dump().substr(0, 300);
    expect_no_prefix_edit(r);
    write_bytes(c.dir.path() / "yy.hot", "class Y { }\r\n");
    c.edit("<rml>\r\n<p>\xC3\xA4</p><script>using \"y\"; class Foo { }\r\n</script>\r\n</rml>\r\n");
    const json u = c.ask(1, 3 + 1 + 4 + 8 + 7);
    ASSERT_FALSE(u.is_null());
    for (const json& it : u["items"])
        if (it.contains("textEdit"))
        {
            EXPECT_EQ(it["textEdit"]["range"]["start"]["line"], 1) << it.dump();
            EXPECT_EQ(it["textEdit"]["range"]["start"]["character"], 3 + 1 + 4 + 8 + 7) << it.dump();
        }
}
