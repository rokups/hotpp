// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// HOT-65 step S1: hot-ls serves a markup document (".rml") with diagnostics only. The host JSON
// has the rule "embedded" and a prefix file. The tests use a temporary hosts dir.
#include "integration/ls/ls_client.h"
#include "long_path.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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

// A host with the rule for ".rml" (or without a rule), a prefix file and a server on a temporary dir.
struct Embedded
{
    TempDir dir;
    std::string path, uri;
    std::unique_ptr<LsClient> client;

    // "prefix" is the text of the prefix file. "rule" false: the host has no rule "embedded".
    explicit Embedded(const std::string& prefix = "using \"Actor.h\";\n", bool rule = true)
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
        if (rule)
            host["embedded"] = json::array({{{"suffix", ".rml"}, {"tag", "script"}, {"prefixFile", "prefix.txt"}}});
        write_bytes(root / "hosts" / "t.json", host.dump());
        hot::ls::ServerOptions o;
        o.clangd = "";
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
    }
    // Waits for a publish of the document that "match" accepts. Null on a timeout.
    json publish(const std::function<bool(const json&)>& match, int timeout_ms = 120000)
    {
        return client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == uri && match(p["diagnostics"]); }, timeout_ms);
    }
    json errors() { return publish([](const json& d) { return !d.empty(); }); }
    json any() { return publish([](const json&) { return true; }); }
};

const std::string kBad = "class Foo { void f() { int x = ; } }\n";

// The column of the first ";" after "int x =" in the line.
int semi_col(const std::string& line)
{
    return static_cast<int>(line.find("= ;") + 2);
}

} // namespace

// An error on line 5 has the raw line and the raw column.
TEST(EmbeddedDiagnostics, ErrorHasRawRange)
{
    Embedded e;
    const std::string code = "    void f() { int x = ; }";
    e.open("<rml>\n<head>\n<title>t</title>\n<script>\nclass Foo {\n" + code + "\n}\n</script>\n</head>\n");
    const json n = e.errors();
    ASSERT_FALSE(n.is_null());
    const json& d = n["params"]["diagnostics"][0];
    EXPECT_EQ(d["range"]["start"]["line"], 5) << n.dump();
    EXPECT_EQ(d["range"]["start"]["character"], semi_col(code)) << n.dump();
    EXPECT_EQ(d["severity"], 1);
}

// Line 0: the prefix shift is undone.
TEST(EmbeddedDiagnostics, ErrorOnLineZeroHasRawColumn)
{
    Embedded e;
    const std::string line = "<script>" + kBad.substr(0, kBad.size() - 1) + "</script>";
    e.open(line + "\n</head>\n");
    const json n = e.errors();
    ASSERT_FALSE(n.is_null());
    const json& d = n["params"]["diagnostics"][0];
    EXPECT_EQ(d["range"]["start"]["line"], 0) << n.dump();
    EXPECT_EQ(d["range"]["start"]["character"], semi_col(line)) << n.dump();
}

// Non-ASCII markup before the code: the column counts UTF-16 units of the raw text.
TEST(EmbeddedDiagnostics, NonAsciiMarkupGivesUtf16Column)
{
    Embedded e;
    const std::string head = "<p>\xC3\xA4\xC3\xB6\xC3\xBC \xF0\x9F\x98\x80</p>"; // 3 chars + space + a surrogate pair
    const std::string code = "class Foo { void f() { int x = ; } }";
    e.open("<rml>\n" + head + "<script>" + code + "</script>\n</rml>\n");
    const json n = e.errors();
    ASSERT_FALSE(n.is_null());
    const json& d = n["params"]["diagnostics"][0];
    // UTF-16 units of the head: "<p>" 3, 3 letters, space, 2 units, "</p>" 4, "<script>" 8.
    const int expected = 3 + 3 + 1 + 2 + 4 + 8 + semi_col(code);
    EXPECT_EQ(d["range"]["start"]["line"], 1) << n.dump();
    EXPECT_EQ(d["range"]["start"]["character"], expected) << n.dump();
}

TEST(EmbeddedDiagnostics, NoScriptBlockGivesAnEmptyList)
{
    Embedded e;
    e.open("<rml>\n<body>this ; is { markup</body>\n</rml>\n");
    const json n = e.any();
    ASSERT_FALSE(n.is_null());
    EXPECT_TRUE(n["params"]["diagnostics"].empty()) << n.dump();
}

TEST(EmbeddedDiagnostics, NoRuleGivesNoPublish)
{
    Embedded e("", false);
    e.open("<rml>\n<script>" + kBad + "</script>\n</rml>\n");
    EXPECT_TRUE(e.publish([](const json&) { return true; }, 1500).is_null()) << "unexpected publish";
    // A request for the ignored document gets null.
    const json r = e.client->request("textDocument/hover",
        {{"textDocument", {{"uri", e.uri}}}, {"position", {{"line", 1}, {"character", 1}}}});
    EXPECT_TRUE(r["result"].is_null()) << r.dump();
}

TEST(EmbeddedDiagnostics, FixThroughDidChangeClearsTheList)
{
    Embedded e;
    e.open("<rml>\n<script>\n" + kBad + "</script>\n</rml>\n");
    ASSERT_FALSE(e.errors().is_null());
    e.client->change(e.uri, 2, "<rml>\n<script>\nclass Foo { void f() { } }\n</script>\n</rml>\n");
    const json n = e.publish([](const json& d) { return d.empty(); });
    ASSERT_FALSE(n.is_null());
}

TEST(EmbeddedDiagnostics, MarkupPartsGiveNoDiagnostics)
{
    Embedded e;
    e.open("<rml>\n<head><title>int x = ; { class</title>\n<script>\nclass Foo { void f() { } }\n</script>\n"
           "</head>\n<body>void g( { ;; </body>\n</rml>\n");
    const json n = e.any();
    ASSERT_FALSE(n.is_null());
    EXPECT_TRUE(n["params"]["diagnostics"].empty()) << n.dump();
}

// An error in the prefix is not before column 0 of line 0.
TEST(EmbeddedDiagnostics, PrefixErrorIsClamped)
{
    Embedded e("using \"NoSuchHeaderHere.h\";\n");
    e.open("<rml>\n<script>\nclass Foo { }\n</script>\n</rml>\n");
    const json n = e.errors();
    ASSERT_FALSE(n.is_null());
    for (const json& d : n["params"]["diagnostics"])
    {
        EXPECT_GE(d["range"]["start"]["character"].get<int>(), 0) << n.dump();
        EXPECT_GE(d["range"]["end"]["character"].get<int>(), 0) << n.dump();
        EXPECT_LT(d["range"]["start"]["line"].get<int>(), 5) << n.dump();
    }
}

TEST(EmbeddedDiagnostics, PrefixWithALineEndIsNotServed)
{
    Embedded e("using \"Actor.h\";\nusing <vector>;\n");
    e.open("<rml>\n<script>\n" + kBad + "</script>\n</rml>\n");
    EXPECT_TRUE(e.publish([](const json&) { return true; }, 1500).is_null());
}

TEST(EmbeddedDiagnostics, OtherRequestsAnswerNull)
{
    Embedded e;
    e.open("<rml>\n<script>\nclass Foo { void f() { } }\n</script>\n</rml>\n");
    ASSERT_FALSE(e.any().is_null());
    const json id = {{"textDocument", {{"uri", e.uri}}}};
    json at = id;
    at["position"] = {{"line", 2}, {"character", 8}};
    for (const char* m : {"textDocument/hover", "textDocument/completion", "textDocument/definition",
             "textDocument/signatureHelp", "textDocument/prepareRename", "textDocument/prepareCallHierarchy"})
    {
        const json r = e.client->request(m, at);
        EXPECT_TRUE(r["result"].is_null()) << m << " " << r.dump();
    }
    json refs = at;
    refs["context"] = {{"includeDeclaration", true}};
    EXPECT_TRUE(e.client->request("textDocument/references", refs)["result"].is_null());
    json ren = at;
    ren["newName"] = "Bar";
    EXPECT_TRUE(e.client->request("textDocument/rename", ren)["result"].is_null());
    json fmt = id;
    fmt["options"] = {{"tabSize", 4}, {"insertSpaces", true}};
    EXPECT_TRUE(e.client->request("textDocument/formatting", fmt)["result"].is_null());
    for (const char* m : {"textDocument/semanticTokens/full", "textDocument/documentLink", "textDocument/codeLens",
             "textDocument/documentSymbol", "textDocument/inlayHint", "textDocument/codeAction"})
    {
        json params = id;
        if (std::string(m) == "textDocument/inlayHint" || std::string(m) == "textDocument/codeAction")
            params["range"] = {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 4}, {"character", 0}}}};
        if (std::string(m) == "textDocument/codeAction")
            params["context"] = {{"diagnostics", json::array()}};
        const json r = e.client->request(m, params);
        EXPECT_TRUE(r["result"].is_null()) << m << " " << r.dump();
    }
    // The server still works.
    e.client->change(e.uri, 2, "<script>class A { void f() { int x = ; } }</script>\n");
    EXPECT_FALSE(e.errors().is_null());
}

// The time from a didChange to the publish for a 300 line document.
TEST(EmbeddedDiagnostics, ChangeToPublishTime)
{
    Embedded e;
    std::string body = "<rml>\n<head>\n<script>\nclass Foo {\n";
    for (int i = 0; i < 280; ++i)
        body += "    int f" + std::to_string(i) + "(int a) { return a + " + std::to_string(i) + "; }\n";
    body += "}\n</script>\n</head>\n<body>\n";
    for (int i = 0; i < 10; ++i)
        body += "<p>line " + std::to_string(i) + "</p>\n";
    body += "</body>\n</rml>\n";
    e.open(body);
    ASSERT_FALSE(e.any().is_null());
    long best = 1 << 30, total = 0;
    const int runs = 8;
    for (int i = 0; i < runs; ++i)
    {
        std::string text = body;
        text.replace(text.find("a + 7;"), 6, "a + " + std::to_string(1000 + i) + ";");
        const auto t0 = std::chrono::steady_clock::now();
        e.client->change(e.uri, 2 + i, text);
        ASSERT_FALSE(e.any().is_null());
        const long ms = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        best = std::min(best, ms);
        total += ms;
    }
    std::printf("[embedded timing] 300-line rml didChange to publish: best %ld ms, mean %ld ms\n", best, total / runs);
    EXPECT_LT(best, 5000);
}
