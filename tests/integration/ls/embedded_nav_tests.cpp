// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// HOT-65 step S4: hover and definition in the script blocks of a markup document (".rml").
// The host JSON has the rule "embedded". A test of a script field needs clangd and skips without it.
#include "integration/ls/ls_client.h"
#include "long_path.h"
#include "support/translator_test_support.h"

#include <thread>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
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

struct Nav
{
    TempDir dir;
    std::string path, uri;
    std::unique_ptr<LsClient> client;

    explicit Nav(const std::string& prefix = "using \"Actor.h\";\n")
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
    void open(const std::string& text)
    {
        write_bytes(path, text);
        client->open(uri, text);
        client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == uri; }, 120000);
    }
    json ask(const char* method, int line, int character)
    {
        return client->request_answered(method, {{"textDocument", {{"uri", uri}}}, {"position", {{"line", line}, {"character", character}}}})["result"];
    }
};

// The UTF-16 column of the n-th "count" in the line, counting from 0.
int col_of(const std::string& line, const std::string& word, int nth = 0)
{
    size_t at = 0;
    for (int i = 0; i <= nth; ++i)
        at = line.find(word, i == 0 ? 0 : at + 1);
    return static_cast<int>(at);
}

void expect_hover(const json& r, int line, int col, int len)
{
    ASSERT_FALSE(r.is_null());
    EXPECT_NE(r["contents"]["value"].get<std::string>().find("count"), std::string::npos) << r.dump();
    EXPECT_EQ(r["range"]["start"]["line"], line) << r.dump();
    EXPECT_EQ(r["range"]["start"]["character"], col) << r.dump();
    EXPECT_EQ(r["range"]["end"]["character"], col + len) << r.dump();
}

#define REQUIRE_CLANGD() \
    do \
    { \
        if (std::string(HOT_CLANGD_PATH).empty()) \
            GTEST_SKIP() << "clangd is not installed"; \
    } while (0)

const std::string kBody = "class Foo { int count; void f() { this.count = this.count + 1; } }";

} // namespace

TEST(EmbeddedNav, HoverGivesRawRange)
{
    REQUIRE_CLANGD();
    Nav n;
    const std::string line = "    int count; void f() { this.count = this.count + 1; }";
    n.open("<rml>\n<head>\n<script>\nclass Foo {\n" + line + "\n}\n</script>\n</head>\n");
    const int c = col_of(line, "count", 1);
    expect_hover(n.ask("textDocument/hover", 4, c + 2), 4, c, 5);
}

TEST(EmbeddedNav, HoverAfterMultiByteTextOnTheLine)
{
    REQUIRE_CLANGD();
    Nav n;
    const std::string head = "<p>\xC3\xA4\xC3\xB6 \xF0\x9F\x98\x80</p>"; // 3 + 2 + 1 + 2 + 4 UTF-16 units
    n.open("<rml>\n" + head + "<script>" + kBody + "</script>\n</rml>\n");
    const int base = 3 + 2 + 1 + 2 + 4 + 8;
    const int c = base + col_of(kBody, "count", 1);
    expect_hover(n.ask("textDocument/hover", 1, c + 1), 1, c, 5);
}

TEST(EmbeddedNav, HoverOnLineZero)
{
    REQUIRE_CLANGD();
    Nav n;
    n.open("<script>" + kBody + "</script>\n</head>\n");
    const int c = 8 + col_of(kBody, "count", 1);
    expect_hover(n.ask("textDocument/hover", 0, c + 1), 0, c, 5);
}

TEST(EmbeddedNav, DefinitionInsideTheBlock)
{
    REQUIRE_CLANGD();
    Nav n;
    const std::string head = "<p>\xC3\xA4</p>";
    n.open("<rml>\n" + head + "<script>" + kBody + "</script>\n</rml>\n");
    const int base = 3 + 1 + 4 + 8;
    const json r = n.ask("textDocument/definition", 1, base + col_of(kBody, "count", 1) + 1);
    ASSERT_FALSE(r.is_null());
    const json& l = r.is_array() ? r[0] : r;
    EXPECT_EQ(l["uri"], n.uri) << r.dump();
    EXPECT_EQ(l["range"]["start"]["line"], 1) << r.dump();
    EXPECT_EQ(l["range"]["start"]["character"], base + col_of(kBody, "count", 0)) << r.dump();
}

TEST(EmbeddedNav, DefinitionOnLineZero)
{
    REQUIRE_CLANGD();
    Nav n;
    n.open("<script>" + kBody + "</script>\n</head>\n");
    const json r = n.ask("textDocument/definition", 0, 8 + col_of(kBody, "count", 1) + 1);
    ASSERT_FALSE(r.is_null());
    const json& l = r.is_array() ? r[0] : r;
    EXPECT_EQ(l["range"]["start"]["line"], 0) << r.dump();
    EXPECT_EQ(l["range"]["start"]["character"], 8 + col_of(kBody, "count", 0)) << r.dump();
}

TEST(EmbeddedNav, MarkupAnswersNull)
{
    Nav n;
    n.open("<rml>\n<head><title>count Foo</title>\n<script>\n" + kBody + "\n</script>\n</head>\n<body>count</body>\n");
    for (const char* m : {"textDocument/hover", "textDocument/definition"})
    {
        EXPECT_TRUE(n.ask(m, 0, 2).is_null()) << m;
        EXPECT_TRUE(n.ask(m, 1, 14).is_null()) << m;  // in the title
        EXPECT_TRUE(n.ask(m, 2, 3).is_null()) << m;   // in the tag
        EXPECT_TRUE(n.ask(m, 6, 8).is_null()) << m;   // in the body
        EXPECT_TRUE(n.ask(m, 40, 0).is_null()) << m;  // after the end of the text
    }
}

// A name that the prefix declares has no raw position: the definition answers null.
TEST(EmbeddedNav, DefinitionIntoThePrefixAnswersNull)
{
    Nav n("class Pre { int v; };\n");
    const std::string body = "class Foo { void f() { Pre p; } }";
    n.open("<rml>\n<script>\n" + body + "\n</script>\n</rml>\n");
    const json r = n.ask("textDocument/definition", 2, col_of(body, "Pre") + 1);
    EXPECT_TRUE(r.is_null()) << r.dump();
}

TEST(EmbeddedNav, DefinitionIntoAHotFile)
{
    Nav n;
    write_bytes(n.dir.path() / "y.hot", "class Y\n{\n    int v;\n}\n");
    const std::string body = "using \"y.hot\";\nclass Foo { void f() { Y y; } }";
    n.open("<rml>\n<script>\n" + body + "\n</script>\n</rml>\n");
    const std::string line = "class Foo { void f() { Y y; } }";
    const json r = n.ask("textDocument/definition", 3, col_of(line, "Y y") + 0);
    ASSERT_FALSE(r.is_null());
    const json& l = r.is_array() ? r[0] : r;
    EXPECT_NE(l["uri"].get<std::string>().find("y.hot"), std::string::npos) << r.dump();
    EXPECT_EQ(l["range"]["start"]["line"], 0) << r.dump();
    EXPECT_EQ(l["range"]["start"]["character"], 6) << r.dump();
}

// Hover and definition on a 300-line document: the time of 20 requests.
TEST(EmbeddedNav, TimingOn300Lines)
{
    REQUIRE_CLANGD();
    Nav n;
    std::string text = "<rml>\n<head>\n<script>\nclass Foo {\n    int count;\n";
    std::vector<std::string> rows;
    for (int i = 0; i < 280; ++i)
    {
        rows.push_back("    int m" + std::to_string(i) + "(int a) { return this.count + a + " + std::to_string(i) + "; }");
        text += rows.back() + "\n";
    }
    text += "}\n</script>\n</head>\n<body>\n</body>\n";
    n.open(text);
    for (const char* m : {"textDocument/hover", "textDocument/definition"})
    {
        std::vector<double> ms;
        for (int i = 0; i < 20; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const int row = 5 + i * 10;
            const json r = n.ask(m, row, col_of(rows[row - 5], "count") + 1);
            EXPECT_FALSE(r.is_null()) << m << " " << i;
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        double sum = 0;
        for (double v : ms)
            sum += v;
        std::printf("TIMING %s mean %.1f ms p95 %.1f ms\n", m, sum / ms.size(), ms[18]);
#ifdef _WIN32
        EXPECT_LT(ms[18], 5000.0) << m; // a loaded Windows runner is slow
#else
        EXPECT_LT(ms[18], 1000.0) << m;
#endif
    }
}

// A symbol that the same block declares: the answer comes from the editor text of the document.
TEST(EmbeddedNav, SameBlockMethodDefinitionAndHover)
{
    Nav n;
    const std::string head = "<p>\xC3\xA4</p>"; // 3 + 1 + 4 UTF-16 units
    const std::string body = "class Foo { void g() {} void f() { g(); } }";
    const int base = 3 + 1 + 4 + 8;
    n.open("<rml>\n<script>\nclass Pad { }\n</script>\n" + head + "<script>" + body + "</script>\n</rml>\n");
    const json d = n.ask("textDocument/definition", 4, base + col_of(body, "g();") + 0);
    ASSERT_FALSE(d.is_null());
    const json& l = d.is_array() ? d[0] : d;
    EXPECT_EQ(l["uri"], n.uri) << d.dump();
    EXPECT_EQ(l["range"]["start"]["line"], 4) << d.dump();
    EXPECT_EQ(l["range"]["start"]["character"], base + col_of(body, "g()")) << d.dump();
    json h;
    for (int i = 0; i < 40; ++i) // a cold hover gives the class text at first (HOT-94)
    {
        h = n.ask("textDocument/hover", 4, base + col_of(body, "g();"));
        if (h.is_object() && h["contents"]["value"].get<std::string>().find("g") != std::string::npos)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ASSERT_FALSE(h.is_null());
    EXPECT_NE(h["contents"]["value"].get<std::string>().find("g"), std::string::npos) << h.dump();
}

// A static method of the same block: the hover text comes from the declaration line of the unit text.
TEST(EmbeddedNav, SameBlockStaticMethodHover)
{
    Nav n;
    const std::string line = "    void f() { Foo.F(1); }";
    n.open("<rml>\n<script>\nclass Foo {\n    static int F(int v) { return v; }\n" + line + "\n}\n</script>\n</rml>\n");
    const json h = n.ask("textDocument/hover", 4, col_of(line, "F(1)"));
    ASSERT_FALSE(h.is_null());
    EXPECT_EQ(h["contents"]["value"], "```hot\nstatic int F(int v)\n```") << h.dump();
    EXPECT_EQ(h["range"]["start"]["character"], col_of(line, "F(1)")) << h.dump();
}

// The answer follows an unsaved edit, not the file on the disk.
TEST(EmbeddedNav, SameBlockAnswerFollowsAnUnsavedEdit)
{
    Nav n;
    const std::string body = "class Foo { void g() {} void f() { g(); } }";
    n.open("<rml>\n<script>\n" + body + "\n</script>\n</rml>\n");
    const std::string edited = "class Foo { int pad; void g() {} void f() { g(); } }";
    n.client->change(n.uri, 2, "<rml>\n<script>\n" + edited + "\n</script>\n</rml>\n");
    n.client->wait_notification("textDocument/publishDiagnostics", [&](const json& p) { return p.value("uri", std::string()) == n.uri; }, 120000);
    const json d = n.ask("textDocument/definition", 2, col_of(edited, "g();"));
    ASSERT_FALSE(d.is_null());
    const json& l = d.is_array() ? d[0] : d;
    EXPECT_EQ(l["range"]["start"]["line"], 2) << d.dump();
    EXPECT_EQ(l["range"]["start"]["character"], col_of(edited, "g()")) << d.dump();
    const json h = n.ask("textDocument/hover", 2, col_of(edited, "g();"));
    ASSERT_FALSE(h.is_null());
}
