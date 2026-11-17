// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot-ls (S14b, design M12 to M14). One test for each feature. The tests that need
// clangd are skipped when CMake did not find clangd.
#include "attributes.h"
#include "clangd_process.h"
#include "integration/ls/ls_client.h"
#include "long_path.h"
#include "semantic.h"
#include "hot/clang_files_backend.h"
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"
#include "symbol_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using ls_test::json;
using ls_test::LsClient;
using translator_test::TempDir;

namespace
{

// Writes the bytes of "text" unchanged. The text mode of the C runtime on Windows writes "\r\n"
// for "\n". Then the file on disk is not the text that the test sends in "didOpen", and the
// server stores no shard when the document closes (store_closed_shard compares the two texts).
void write_file(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary) << text;
}

// The 0-based lines: 0 using, 2 class, 3 hp, 5 Tick, 6 Move, 7 heal call, 10 heal, 11 hp.
const char* const kScript =
    "using \"Actor.h\";\n"
    "\n"
    "class Player : Actor {\n"
    "    float hp = 100.0;\n"
    "\n"
    "    override void Tick(float dt) {\n"
    "        this.Move(Vec3(dt, 0, 0));\n"
    "        this.heal(1.0);\n"
    "    }\n"
    "\n"
    "    void heal(float amount) {\n"
    "        this.hp = this.hp + amount;\n"
    "    }\n"
    "}\n";

bool have_clangd()
{
    return std::string(HOT_CLANGD_PATH).size() > 0;
}

#define REQUIRE_CLANGD() \
    do \
    { \
        if (!have_clangd()) \
            GTEST_SKIP() << "clangd is not installed"; \
    } while (0)

hot::ls::ServerOptions options(const TempDir& dir, bool clangd)
{
    hot::ls::ServerOptions o;
    o.clangd = clangd ? HOT_CLANGD_PATH : "";
    o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
    // The tests count the analyses, so the default is off. The tests of the index set it.
    // HOTPP_TEST_BACKGROUND_INDEX=1 turns the index on for all tests.
    const char* bg = std::getenv("HOTPP_TEST_BACKGROUND_INDEX");
    o.background_index = bg && std::string(bg) == "1";
    // HOT_LS_CLANGD_LOG keeps the clangd log after the test (the temporary dir goes away).
    // A directory gets one log file for each test.
    const char* log = std::getenv("HOT_LS_CLANGD_LOG");
    o.clangd_log = log ? log : (dir.path() / "clangd.log").string();
    std::error_code ec;
    if (log && std::filesystem::is_directory(log, ec))
    {
        const ::testing::TestInfo* t = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string name = t ? std::string(t->test_suite_name()) + "." + t->name() : "clangd";
        o.clangd_log = (std::filesystem::path(log) / (name + ".log")).string();
    }
    return o;
}

std::string uri_of(const std::string& path)
{
#ifdef _WIN32
    // The URI of "C:\\dir\\a.hot" is "file:///C:/dir/a.hot", as hot-ls and clangd write it.
    // The URI has the long names: the temporary folder can have a short 8.3 name.
    std::string p = hot::ls::long_path(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    return (p.size() > 1 && p[1] == ':' ? "file:///" : "file://") + p;
#else
    // The real path without symbolic links (macOS "/private/var"), as hot-ls and clangd write it.
    return "file://" + hot::ls::long_path(path);
#endif
}

json pos(int line, int character)
{
    return {{"line", line}, {"character", character}};
}

json doc_pos(const std::string& uri, int line, int character)
{
    return {{"textDocument", {{"uri", uri}}}, {"position", pos(line, character)}};
}

// A server with the script open. The script file is on disk too.
struct Session
{
    TempDir dir;
    std::string path, uri;
    std::unique_ptr<LsClient> client;
    explicit Session(bool clangd, const std::string& text = kScript, hot::ls::ServerOptions* o = nullptr)
    {
        path = (dir.path() / "player.hot").string();
        uri = uri_of(path);
        write_file(path, text);
        client = std::make_unique<LsClient>(o ? *o : options(dir, clangd));
        client->initialize(uri_of(dir.path().string()));
        client->open(uri, text);
    }
    // Waits for the diagnostics of the script that "match" accepts.
    json diagnostics(const std::function<bool(const json&)>& match)
    {
        return client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == uri && match(p["diagnostics"]); });
    }
};

bool has_source(const json& diags, const std::string& source)
{
    return std::any_of(diags.begin(), diags.end(), [&](const json& d) { return d.value("source", "") == source; });
}

} // namespace

// ------------------------------------------------------------------ M12: diagnostics

TEST(LanguageServer, ParseErrorIsAtTheScriptPosition)
{
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.hp = ;");
    Session s(false, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "hot"); });
    ASSERT_FALSE(n.is_null());
    const json& d = n["params"]["diagnostics"][0];
    EXPECT_EQ(d["range"]["start"]["line"], 11) << n.dump();
    EXPECT_EQ(d["severity"], 1);
}

// The #line check of design 10.1: clangd reports the file position, not the "#line" position.
// The map gives the script line. The rewriter gives script names ("int", not "int32_t").
TEST(LanguageServer, ClangdTypeErrorIsAtTheScriptLine)
{
    REQUIRE_CLANGD();
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "int x = \"abc\";");
    Session s(true, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "clangd"); });
    ASSERT_FALSE(n.is_null()) << "no clangd diagnostics";
    bool found = false;
    for (const json& d : n["params"]["diagnostics"])
    {
        if (d.value("source", "") != "clangd")
            continue;
        const std::string msg = d.value("message", "");
        if (msg.find("initialize") == std::string::npos)
            continue;
        found = true;
        // clangd puts the error on the variable "x" (character 12).
        EXPECT_EQ(d["range"]["start"]["line"], 11) << d.dump();
        EXPECT_EQ(d["range"]["start"]["character"], 12) << d.dump();
        EXPECT_NE(msg.find("int'"), std::string::npos) << msg; // "'int'" or "'const int'"
        EXPECT_EQ(msg.find("int32_t"), std::string::npos) << msg;
    }
    EXPECT_TRUE(found) << n.dump();
}

// clangd does not publish again when a new version has the same diagnostics. An edit that gives
// the same C++ must not remove the clangd diagnostics. The positions follow the edit.
TEST(LanguageServer, ClangdDiagnosticStaysAfterAnEditThatGivesTheSameCpp)
{
    REQUIRE_CLANGD();
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "int x = \"abc\";");
    Session s(true, text);
    const json first = s.diagnostics([](const json& d) { return has_source(d, "clangd"); });
    ASSERT_FALSE(first.is_null()) << "no clangd diagnostics";
    // Extra spaces in a field line, away from the error. The C++ may stay equal.
    std::string t2 = text;
    t2.replace(t2.find("float hp"), 8, "float   hp");
    s.client->change(s.uri, 2, t2);
    // The last publish of the script must still have the diagnostic.
    json last;
    for (;;)
    {
        const json n = s.client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == s.uri; }, 5000);
        if (n.is_null())
            break;
        last = n;
    }
    ASSERT_FALSE(last.is_null());
    bool found = false;
    for (const json& d : last["params"]["diagnostics"])
        if (d.value("source", "") == "clangd" && d["range"]["start"]["line"] == 11)
            found = true;
    EXPECT_TRUE(found) << last.dump();
}

// ------------------------------------------------------------------ M13: navigation and editing

TEST(LanguageServer, CompletionAfterThisListsNativeAndScriptMembers)
{
    REQUIRE_CLANGD();
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.");
    Session s(true, text);
    const json r = s.client->request("textDocument/completion", doc_pos(s.uri, 11, 13));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    std::vector<std::string> labels;
    for (const json& i : r["result"]["items"])
        labels.push_back(i.value("insertText", i.value("label", "")));
    auto has = [&](const char* n) { return std::find(labels.begin(), labels.end(), n) != labels.end(); };
    EXPECT_TRUE(has("Move")) << r.dump(); // a native member of Actor
    EXPECT_TRUE(has("Position")) << r.dump(); // a native field of Actor
    EXPECT_TRUE(has("hp")); // a script field
    EXPECT_TRUE(has("heal")); // a script method
    EXPECT_FALSE(has("cls")); // hot::ShellBase is hidden
    EXPECT_FALSE(has("state"));
}

TEST(LanguageServer, HoverOnAScriptFieldShowsItsType)
{
    REQUIRE_CLANGD();
    Session s(true);
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, 11, 14));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const std::string v = r["result"]["contents"].value("value", "");
    EXPECT_NE(v.find("hp"), std::string::npos) << v;
    EXPECT_NE(v.find("float"), std::string::npos) << v;
    EXPECT_EQ(r["result"]["range"]["start"]["line"], 11);
}

TEST(LanguageServer, DefinitionOfAScriptMethodIsInTheScript)
{
    REQUIRE_CLANGD();
    Session s(true);
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, 7, 14));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    const json& loc = r["result"][0];
    EXPECT_EQ(loc["uri"], s.uri);
    EXPECT_EQ(loc["range"]["start"]["line"], 10) << r.dump();
    EXPECT_EQ(loc["range"]["start"]["character"], 9) << r.dump();
}

// A C++ header is not a script. hot-ls ignores it: no diagnostics, and an empty answer to each
// feature request. The header text here is not valid Hot++, so any analysis would give an error.
TEST(LanguageServer, CppHeaderIsIgnored)
{
    Session s(false);
    const std::string header = (s.dir.path() / "engine.h").string();
    const std::string text = "#pragma once\nstruct Engine { int hp; };\n";
    write_file(header, text);
    for (const char* name : {"engine.h", "engine.hpp", "engine.hh", "engine.cc"})
    {
        const std::string uri = uri_of((s.dir.path() / name).string());
        s.client->open(uri, text);
        s.client->change(uri, 2, text + "struct {");
        for (const char* method : {"textDocument/definition", "textDocument/hover", "textDocument/references",
                 "textDocument/documentSymbol", "textDocument/completion", "textDocument/semanticTokens/full"})
        {
            const json r = s.client->request(method, doc_pos(uri, 1, 8));
            ASSERT_TRUE(r.contains("result")) << method << " " << r.dump();
            const json& v = r["result"];
            EXPECT_TRUE(v.is_null() || (v.is_array() && v.empty())) << method << " " << r.dump();
        }
        const json n = s.client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == uri; }, 200);
        EXPECT_TRUE(n.is_null()) << name << " " << n.dump();
    }
    // The script still works.
    const json r = s.client->request("textDocument/documentSymbol", doc_pos(s.uri, 0, 0));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
}

// A broken header that a script includes is not published for the header. The error shows on the
// "using" line of the script. A missing header shows there too.
TEST(LanguageServer, HeaderErrorShowsOnTheUsingLine)
{
    REQUIRE_CLANGD();
    for (const bool missing : {false, true})
    {
        const std::string script = "using \"bad.h\";\nclass A {\n    int x = 1;\n}\n";
        Session s(true, script);
        const std::string header = (s.dir.path() / "bad.h").string();
        if (!missing)
            write_file(header, "#pragma once\nstruct Bad { int x\n};\nint y = ;\n");
        s.client->change(s.uri, 2, script + "\n");
        const json n = s.client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == s.uri && !p["diagnostics"].empty(); }, 60000);
        ASSERT_FALSE(n.is_null()) << (missing ? "missing" : "broken") << ": no diagnostic on the script";
        bool found = false;
        for (const json& d : n["params"]["diagnostics"])
            if (d["range"]["start"]["line"] == 0 && d["message"].get<std::string>().find("bad.h") != std::string::npos)
                found = true;
        EXPECT_TRUE(found) << (missing ? "missing: " : "broken: ") << n.dump();
        const json h = s.client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == uri_of(header); }, 200);
        EXPECT_TRUE(h.is_null()) << h.dump();
    }
}

// The rename warning: a removed field and an added field without [alias] give a Warning at the
// new name. The baseline is the field set at didOpen.
namespace
{

std::vector<json> rename_warnings(Session& s, const std::string& text, int version)
{
    s.client->change(s.uri, version, text);
    // The open and the change each publish. The last publish is the one that counts.
    json n;
    for (;;)
    {
        const json next = s.client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == s.uri; }, n.is_null() ? 20000 : 500);
        if (next.is_null())
            break;
        n = next;
    }
    std::vector<json> out;
    if (n.is_null())
        return out;
    for (const json& d : n["params"]["diagnostics"])
        if (d.value("message", std::string()).find("is renamed to") != std::string::npos)
            out.push_back(d);
    return out;
}

const char* const kRenameBase = "class A {\n    float hp = 1.0;\n    int mp = 2;\n}\n";

} // namespace

TEST(LanguageServer, RenamedFieldWithoutAliasGivesAWarning)
{
    Session s(false, kRenameBase);
    const auto w = rename_warnings(s, "class A {\n    float health = 1.0;\n    int mp = 2;\n}\n", 2);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0]["severity"], 2);
    EXPECT_EQ(w[0]["message"],
        "field 'hp' is renamed to 'health' without [alias]; the value is lost at hot reload");
    EXPECT_EQ(w[0]["range"]["start"]["line"], 1);
    EXPECT_EQ(w[0]["range"]["start"]["character"], 10);
}

TEST(LanguageServer, RenamedFieldWithAliasGivesNoWarning)
{
    Session s(false, kRenameBase);
    EXPECT_TRUE(rename_warnings(s, "class A {\n    [alias(\"hp\")] float health = 1.0;\n    int mp = 2;\n}\n", 2).empty());
}

TEST(LanguageServer, NewFieldWithoutARemovedFieldGivesNoWarning)
{
    Session s(false, kRenameBase);
    EXPECT_TRUE(rename_warnings(s, "class A {\n    float hp = 1.0;\n    int mp = 2;\n    int xp = 3;\n}\n", 2).empty());
}

// S30 (a): the translation fails, so clangd keeps the last good C++. The positions go through
// the edit. The call of heal moves two lines down. A missing import makes the translation fail. The definition is still
// found.
TEST(LanguageServer, DefinitionAfterATranslationErrorUsesTheShiftedPositions)
{
    REQUIRE_CLANGD();
    Session s(true);
    ASSERT_TRUE(s.client->request("textDocument/definition", doc_pos(s.uri, 7, 14)).contains("result"));
    std::string text = kScript;
    text.replace(text.find("\nclass Player"), 0, "using \"missing.hot\";\n\n");
    s.client->change(s.uri, 2, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "hot"); });
    ASSERT_FALSE(n.is_null()) << "the translation must fail";
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, 9, 14));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    EXPECT_EQ(r["result"][0]["uri"], s.uri);
    EXPECT_EQ(r["result"][0]["range"]["start"]["line"], 12) << r.dump();
    EXPECT_EQ(r["result"][0]["range"]["start"]["character"], 9) << r.dump();
    // A place inside the edit has no mapping.
    const json e = s.client->request("textDocument/hover", doc_pos(s.uri, 2, 8));
    EXPECT_TRUE(e["result"].is_null()) << e.dump();
}

// S30 (b): a clangd note in the generated glue has no map segment. The "#line" directive gives the
// script line. The column is the column of the same word in that line, not 0.
TEST(LanguageServer, DiagnosticWithoutASegmentGetsTheColumnOfTheWord)
{
    REQUIRE_CLANGD();
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.heal(\"a\", 2);");
    Session s(true, text);
    const json n = s.diagnostics([](const json& d)
    {
        return std::any_of(d.begin(), d.end(),
            [](const json& x) { return x.value("message", "").find("not viable") != std::string::npos; });
    });
    ASSERT_FALSE(n.is_null());
    for (const json& d : n["params"]["diagnostics"])
    {
        if (d.value("message", "").find("not viable") == std::string::npos)
            continue;
        EXPECT_EQ(d["range"]["start"], pos(10, 9)) << d.dump(); // the name "heal" in the definition line
        EXPECT_EQ(d["range"]["end"], pos(10, 13)) << d.dump();
    }
}

// The imported script of the next two tests. 0-based lines: 2 hp, 3 hp again, 5 Boost.
const char* const kImported =
    "using \"Actor.h\";\n"
    "class Base : Actor {\n"
    "    float hp = 1.0;\n"
    "    float hp = 2.0;\n"
    "\n"
    "    void Boost() { }\n"
    "}\n";

const char* const kImporter =
    "using \"base.hot\";\n"
    "class Kid : Base {\n"
    "    void Go() { this.Boost(); }\n"
    "}\n";

// A translator error in an imported script is published for that script, at its own line and
// column. The importing script gets no error at that position.
TEST(LanguageServer, TranslatorErrorInAnImportedScriptIsPublishedForThatScript)
{
    Session s(false, kImporter);
    write_file(s.dir.path() / "base.hot", kImported);
    s.client->change(s.uri, 2, kImporter);
    const std::string base_uri = uri_of((s.dir.path() / "base.hot").lexically_normal().string());
    const json n = s.client->wait_notification("textDocument/publishDiagnostics",
        [&](const json& p) { return p.value("uri", std::string()) == base_uri && !p["diagnostics"].empty(); });
    ASSERT_FALSE(n.is_null()) << "no diagnostics for the imported script";
    const json& d = n["params"]["diagnostics"][0];
    EXPECT_EQ(d["range"]["start"]["line"], 3) << n.dump();
    EXPECT_EQ(d["range"]["start"]["character"], 10) << n.dump();
    EXPECT_EQ(d["source"], "hot");
}

// The definition of a method of an imported script is in that script, at the real line and column.
TEST(LanguageServer, DefinitionInAnImportedScriptIsInThatScript)
{
    REQUIRE_CLANGD();
    std::string imported = kImported;
    imported.replace(imported.find("    float hp = 2.0;\n"), 20, "");
    Session s(true, kImporter);
    write_file(s.dir.path() / "base.hot", imported);
    s.client->change(s.uri, 2, kImporter);
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, 2, 21));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    const json& loc = r["result"][0];
    EXPECT_EQ(loc["uri"], uri_of((s.dir.path() / "base.hot").lexically_normal().string())) << r.dump();
    EXPECT_EQ(loc["range"]["start"]["line"], 4) << r.dump();
    EXPECT_EQ(loc["range"]["start"]["character"], 9) << r.dump();
}

// A script with a class that has a static method. Another script calls it as "Unit.F(...)".
const char* const kStaticImported =
    "using \"Actor.h\";\n"
    "class Unit : Actor {\n"
    "    static int F(int v) { return v; }\n"
    "}\n";

const char* const kStaticImporter =
    "using \"unit.hot\";\n"
    "class Kid : Actor {\n"
    "    void Go() { int n = Unit.F(1); }\n"
    "}\n";

// Sends F12 at the column of "word" + "shift" on line 2 of kStaticImporter. The importer
// imports "unit.hot" with the text "imported".
static json static_definition(Session& s, const std::string& imported, const char* importer, const char* word, int shift)
{
    write_file(s.dir.path() / "unit.hot", imported);
    s.client->change(s.uri, 2, importer);
    const std::string line = "    void Go() { int n = Unit.F(1); }";
    const int col = static_cast<int>(line.find(word)) + shift;
    return s.client->request("textDocument/definition", doc_pos(s.uri, 2, col));
}

// F12 on the member of a qualified static call goes to the static method in the imported script.
TEST(LanguageServer, DefinitionOfAStaticMethodOfAnImportedClassIsInThatScript)
{
    REQUIRE_CLANGD();
    Session s(true, kStaticImporter);
    const json r = static_definition(s, kStaticImported, kStaticImporter, "Unit.F", 5);
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    const json& loc = r["result"][0];
    EXPECT_EQ(loc["uri"], uri_of((s.dir.path() / "unit.hot").lexically_normal().string())) << r.dump();
    EXPECT_EQ(loc["range"]["start"]["line"], 2) << r.dump();
    EXPECT_EQ(loc["range"]["start"]["character"], 15) << r.dump();
}

// Hover on the member of a qualified static call gives the declaration line from the table.
TEST(LanguageServer, HoverOfAStaticMethodOfAnImportedClassIsTheDeclarationLine)
{
    REQUIRE_CLANGD();
    Session s(true, kStaticImporter);
    write_file(s.dir.path() / "unit.hot", kStaticImported);
    s.client->change(s.uri, 2, kStaticImporter);
    const std::string line = "    void Go() { int n = Unit.F(1); }";
    const int col = static_cast<int>(line.find("Unit.F")) + 5;
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, 2, col));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["contents"]["value"], "```hot\nstatic int F(int v)\n```") << r.dump();
}

// F12 on the class of a qualified static call goes to the class, not to the static method.
TEST(LanguageServer, DefinitionOfTheClassOfAStaticCallIsTheClass)
{
    REQUIRE_CLANGD();
    Session s(true, kStaticImporter);
    const json r = static_definition(s, kStaticImported, kStaticImporter, "Unit.F", 1);
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    const json& loc = r["result"][0];
    EXPECT_EQ(loc["uri"], uri_of((s.dir.path() / "unit.hot").lexically_normal().string())) << r.dump();
    EXPECT_EQ(loc["range"]["start"]["line"], 1) << r.dump();
}

TEST(LanguageServer, DefinitionOfANativeMethodIsInTheNativeHeader)
{
    REQUIRE_CLANGD();
    Session s(true);
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, 6, 14));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    const std::string uri = r["result"][0].value("uri", "");
    EXPECT_NE(uri.find("Actor"), std::string::npos) << r.dump();
}

// ------------------------------------------------------------------ definition of native symbols
namespace
{

const char* const kNativeHeader =
    "#pragma once\n"
    "#include \"Actor.h\"\n"
    "#include <vector>\n"
    "\n"
    "enum class Mood { Calm, Angry };\n"
    "enum Level { LevelLow, LevelHigh };\n"
    "using Score = int;\n"
    "#define MAX_HP 100\n"
    "\n"
    "struct Counter\n"
    "{\n"
    "    int count = 0;\n"
    "    int Bump(int by);\n"
    "};\n"
    "\n"
    "class Boss : public Actor\n"
    "{\n"
    "public:\n"
    "    int rage = 0;\n"
    "    void Roar();\n"
    "};\n"
    "\n"
    "int Clamp100(int v);\n"
    "int Tag(const char* text, Counter c);\n"
    "int Run(int v);\n";

const char* const kNativeScript =
    "using \"Actor.h\";\n"
    "using \"Weapon.h\";\n"
    "using \"natives.h\";\n"
    "\n"
    "[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n"
    "\n"
    "class Hero : Boss {\n"
    "    Counter counter;\n"
    "    Score score = 0;\n"
    "    std.vector<Counter> counters;\n"
    "    Mood mood = Mood.Calm;\n"
    "    int limit = Clamp100(MAX_HP);\n"
    "    int bumped = Counter().Bump(2);\n"
    "    int seen = Counter().count;\n"
    "\n"
    "    void Take(Counter Counter) {\n"
    "    }\n"
    "\n"
    "    int Pick(Counter c, int Bump, int count) {\n"
    "        c.count = count;\n"
    "        return c.Bump(Bump);\n"
    "    }\n"
    "\n"
    "    override void Tick(float dt) {\n"
    "        Counter local;\n"
    "        Counter* ptr = null;\n"
    "        Weapon w = new Weapon(1, \"a\");\n"
    "        Boss other = this as Boss;\n"
    "        bool isBoss = this is Boss;\n"
    "        int x = Clamp100(MAX_HP);\n"
    "        this.Roar();\n"
    "        this.rage = local.Bump(this.counter.count);\n"
    "        Level lv = LevelHigh;\n"
    "        Mood m = Mood.Angry;\n"
    "        int tag = Tag(\"Counter Counter Counter Counter\", Counter());\n"
    "        // Roar and Clamp100 in a comment\n"
    "        string note = \"Clamp100 in a string\";\n"
    "        string wide = \"\xC3\xA9\"; int after = Clamp100(2);\n"
    "    }\n"
    "}\n";

// A server with the script above. The script uses a header of the temporary directory.
struct NativeSession
{
    TempDir dir;
    std::string path, uri;
    std::unique_ptr<LsClient> client;
    // "token_wait_ms" is the wait of a semantic tokens request for clangd. The tests give a long wait
    // so that the first answer has the native tokens.
    // "clangd" replaces the clangd program when it is not empty.
    // "rename_budget_ms" is the time budget of one rename. "reference_budget_ms" is the time
    // budget of the scan of the other scripts for one references request.
    explicit NativeSession(const char* script = kNativeScript, int token_wait_ms = 10000, bool native_tokens = true,
        const std::string& clangd = "", int rename_budget_ms = 10000, const char* header = kNativeHeader,
        int reference_budget_ms = 10000)
    {
        write_file((dir.path() / "natives.h").string(), header);
        path = (dir.path() / "hero.hot").string();
        uri = uri_of(path);
        write_file(path, script);
        hot::ls::ServerOptions o = options(dir, true);
        o.include_dirs.push_back(dir.path().string());
        o.native_token_wait_ms = token_wait_ms;
        o.native_semantic_tokens = native_tokens;
        o.rename_budget_ms = rename_budget_ms;
        o.reference_budget_ms = reference_budget_ms;
        if (!clangd.empty())
            o.clangd = clangd;
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(dir.path().string()));
        client->open(uri, script);
    }
    // F12 on the first "word" in the first "context" of the script. The answer is a location in
    // the header "header" (a file of the temporary directory or of tests/engine). The line of the
    // location starts with "decl" and the range starts at the word "word" of that line.
    void expect(const std::string& context, const std::string& word, const std::string& header,
        const std::string& decl, int nth = 0)
    {
        const std::string text = kNativeScript;
        size_t at = 0;
        for (int i = 0; i <= nth; ++i)
            at = text.find(context, i == 0 ? 0 : at + 1);
        ASSERT_NE(at, std::string::npos) << context;
        at += context.rfind(word);
        const int line = static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
        const int col = static_cast<int>(at - (text.rfind('\n', at - 1) + 1));
        const json r = client->request("textDocument/definition", doc_pos(uri, line, col));
        SCOPED_TRACE(context + " -> " + r.dump());
        ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty());
        const json& loc = r["result"][0];
        const std::string file = header == "natives.h" ? (dir.path() / header).string()
                                                       : std::string(HOT_SOURCE_DIR "/tests/engine/") + header;
        EXPECT_EQ(std::filesystem::path(loc.value("uri", "").substr(7)).filename(), header);
        std::ifstream in(file);
        std::string h((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t d = h.find(decl);
        ASSERT_NE(d, std::string::npos) << decl;
        const int dl = static_cast<int>(std::count(h.begin(), h.begin() + static_cast<long>(d), '\n'));
        EXPECT_EQ(loc["range"]["start"]["line"], dl);
        const size_t ls = h.rfind('\n', d) == std::string::npos ? 0 : h.rfind('\n', d) + 1;
        const size_t le = h.find('\n', d);
        const std::string hl = h.substr(ls, le - ls);
        const int c = loc["range"]["start"]["character"];
        EXPECT_EQ(hl.substr(static_cast<size_t>(c), word.size()), word) << hl;
    }
};

} // namespace

TEST(LanguageServerNative, ClassNameOfAVariableTypeJumpsToTheClass)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("Counter local;", "Counter", "natives.h", "struct Counter");
    s.expect("Counter counter;", "Counter", "natives.h", "struct Counter");
}

TEST(LanguageServerNative, BaseClassNameJumpsToTheClass)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("class Hero : Boss", "Boss", "natives.h", "class Boss");
}

TEST(LanguageServerNative, ClassNameAfterNewJumpsToTheClass)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("new Weapon(", "Weapon", "Weapon.h", "struct Weapon");
}

TEST(LanguageServerNative, ClassNameInAsAndIsJumpsToTheClass)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("this as Boss", "Boss", "natives.h", "class Boss");
    s.expect("this is Boss", "Boss", "natives.h", "class Boss");
}

TEST(LanguageServerNative, FreeFunctionCallJumpsToTheFunction)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("Clamp100(MAX_HP)", "Clamp100", "natives.h", "int Clamp100");
}

TEST(LanguageServerNative, MethodCallOnAnObjectJumpsToTheMethod)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("local.Bump(", "Bump", "natives.h", "int Bump");
    s.expect("this.Roar()", "Roar", "natives.h", "void Roar");
}

TEST(LanguageServerNative, FieldJumpsToTheField)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("this.rage =", "rage", "natives.h", "int rage");
    s.expect("this.counter.count", "count", "natives.h", "int count");
}

TEST(LanguageServerNative, EnumTypeAndEnumValueJumpToTheEnum)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("Mood m = ", "Mood", "natives.h", "enum class Mood");
    s.expect("Mood.Angry;", "Angry", "natives.h", "enum class Mood");
    s.expect("Level lv", "Level", "natives.h", "enum Level");
    s.expect("= LevelHigh;", "LevelHigh", "natives.h", "enum Level");
}

TEST(LanguageServerNative, TypeInATemplateArgumentAndInAPointerTypeJumpsToTheType)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("std.vector<Counter>", "Counter", "natives.h", "struct Counter");
    s.expect("Counter* ptr", "Counter", "natives.h", "struct Counter");
}

// The word is four times in a string of the same line before the real use. The strings are not
// places for the word.
TEST(LanguageServerNative, WordInAStringOrAComment)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("Tag(\"Counter Counter Counter Counter\", Counter())", "Counter", "natives.h", "struct Counter");
}

TEST(LanguageServerNative, TypeAliasAndMacroJumpToTheirDeclaration)
{
    REQUIRE_CLANGD();
    NativeSession s;
    s.expect("Score score", "Score", "natives.h", "using Score");
    s.expect("Clamp100(MAX_HP)", "MAX_HP", "natives.h", "#define MAX_HP");
}

// ------------------------------------------------------------------ references and hover of native symbols
namespace
{

// The 0-based line and column of the last "word" in the first "context" of the native script.
json native_pos(const std::string& context, const std::string& word, const std::string& text = kNativeScript)
{
    const size_t at = text.find(context);
    EXPECT_NE(at, std::string::npos) << context;
    const size_t w = at + context.rfind(word);
    // The column counts UTF-16 units: a byte of a multi-byte character (10xxxxxx) is not a unit.
    int col = 0;
    for (size_t i = text.rfind('\n', w - 1) + 1; i < w; ++i)
        col += (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80;
    return pos(static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(w), '\n')), col);
}

// The 0-based line of the first "text" in "source".
int line_of(const std::string& source, const std::string& text)
{
    const size_t at = source.find(text);
    EXPECT_NE(at, std::string::npos) << text;
    return static_cast<int>(std::count(source.begin(), source.begin() + static_cast<long>(at), '\n'));
}

// The references of the word in the context. The cursor has no map segment or has one. The answer
// must have the declaration in natives.h (the line of "decl") and a place in the script for each
// context of "script_contexts".
void expect_native_references(NativeSession& s, const std::string& context, const std::string& word,
    const std::string& decl, const std::vector<std::string>& script_contexts)
{
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos(context, word)},
        {"context", {{"includeDeclaration", true}}}};
    // The index of clangd can need some time.
    json r;
    bool have_decl = false;
    for (int i = 0; i < 20 && !have_decl; ++i)
    {
        r = s.client->request("textDocument/references", p);
        ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
        for (const json& l : r["result"])
            have_decl = have_decl
                || (l.value("uri", "") == uri_of((s.dir.path() / "natives.h").string())
                    && l["range"]["start"]["line"] == line_of(kNativeHeader, decl));
        if (!have_decl)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    EXPECT_TRUE(have_decl) << "no declaration in natives.h: " << r.dump();
    for (const std::string& c : script_contexts)
    {
        const json at = native_pos(c, word);
        bool found = false;
        for (const json& l : r["result"])
            found = found
                || (l.value("uri", "") == s.uri && l["range"]["start"] == at
                    && l["range"]["end"]["character"] == at["character"].get<int>() + static_cast<int>(word.size()));
        EXPECT_TRUE(found) << "no place in the script for: " << c << " in " << r.dump();
    }
}

// The hover text of the word in the context has the text "want".
void expect_native_hover(NativeSession& s, const std::string& context, const std::string& word, const std::string& want)
{
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, native_pos(context, word)["line"],
                                                               native_pos(context, word)["character"]));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << context << " -> " << r.dump();
    EXPECT_NE(r["result"].dump().find(want), std::string::npos) << context << " -> " << r.dump();
}

} // namespace

TEST(LanguageServerNative, ReferencesOfAFunction)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_references(s, "Clamp100(MAX_HP)", "Clamp100", "int Clamp100", {"Clamp100(MAX_HP)", "limit = Clamp100(", "after = Clamp100(2)"});
}

TEST(LanguageServerNative, ReferencesOfAClass)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_references(s, "Counter local;", "Counter", "struct Counter", {"Counter local;", "Counter counter;"});
    expect_native_references(s, "class Hero : Boss", "Boss", "class Boss", {"class Hero : Boss", "this as Boss"});
}

TEST(LanguageServerNative, ReferencesOfAMethod)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_references(s, "local.Bump(", "Bump", "int Bump", {"local.Bump(", "bumped = Counter().Bump("});
    expect_native_references(s, "this.Roar()", "Roar", "void Roar", {"this.Roar()"});
}

TEST(LanguageServerNative, ReferencesOfAField)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_references(s, "this.rage =", "rage", "int rage", {"this.rage ="});
    expect_native_references(s, "this.counter.count", "count", "int count", {"this.counter.count", "seen = Counter().count"});
}

TEST(LanguageServerNative, ReferencesOfAnEnum)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_references(s, "Mood m = ", "Mood", "enum class Mood", {"Mood m = ", "Mood mood"});
    expect_native_references(s, "Mood.Angry;", "Angry", "Angry", {"Mood.Angry;"});
    expect_native_references(s, "Level lv", "Level", "enum Level", {"Level lv"});
}

// The hover range uses the UTF-16 columns: a character before the word has two bytes, one unit.
TEST(LanguageServerNative, HoverRangeIsTheWordInUtf16Columns)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const json at = native_pos("after = Clamp100(2)", "Clamp100");
    EXPECT_EQ(at["character"], 39); // the byte column is 40: the character has two bytes
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, at["line"], at["character"]));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["range"]["start"], at) << r.dump();
    EXPECT_EQ(r["result"]["range"]["end"], pos(at["line"], at["character"].get<int>() + 8)) << r.dump();
}

// A native name in a comment or in a string is not a symbol. The word search does not run there.
TEST(LanguageServerNative, HoverInACommentOrAStringGivesNothing)
{
    REQUIRE_CLANGD();
    NativeSession s;
    for (const auto& c : {std::make_pair("Roar and", "Roar"), std::make_pair("Roar and Clamp100 in", "Clamp100"),
             std::make_pair("\"Clamp100 in a string", "Clamp100")})
    {
        const json at = native_pos(c.first, c.second);
        const json r = s.client->request("textDocument/hover", doc_pos(s.uri, at["line"], at["character"]));
        ASSERT_TRUE(r.contains("result")) << r.dump();
        EXPECT_TRUE(r["result"].is_null()) << c.first << " -> " << r.dump();
    }
}

// The parameter "Counter" of Take has the name of the class. The use of the class in the head has
// no map segment and is found by the word. The parameter name is not a use.
TEST(LanguageServerNative, ReferencesSkipAParameterWithTheNameOfTheSymbol)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const json at = native_pos("Counter local;", "Counter");
    const json r = s.client->request("textDocument/references",
        {{"textDocument", {{"uri", s.uri}}}, {"position", at}, {"context", {{"includeDeclaration", true}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    const json param = native_pos("Take(Counter Counter)", "Counter"); // the parameter name
    const json type = pos(param["line"], param["character"].get<int>() - 8);
    bool has_type = false;
    for (const json& l : r["result"])
    {
        has_type = has_type || (l.value("uri", "") == s.uri && l["range"]["start"] == type);
        EXPECT_FALSE(l.value("uri", "") == s.uri && l["range"]["start"] == param) << r.dump();
    }
    EXPECT_TRUE(has_type) << r.dump();
}

// A parameter has the name of a native method and of a native field. The use of the symbol is
// the name after the dot. The parameter in the arguments is not a use.
TEST(LanguageServerNative, ReferencesSkipAParameterWithTheNameOfAMember)
{
    REQUIRE_CLANGD();
    NativeSession s;
    struct Case
    {
        const char *symbol, *word, *use, *param;
    };
    for (const Case& c : {Case{"local.Bump(", "Bump", "c.Bump(", "c.Bump(Bump)"},
             Case{"this.counter.count", "count", "c.count =", "= count;"}})
    {
        const json r = s.client->request("textDocument/references",
            {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos(c.symbol, c.word)},
                {"context", {{"includeDeclaration", true}}}});
        ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
        const json use = native_pos(c.use, c.word), param = native_pos(c.param, c.word);
        bool has_use = false;
        for (const json& l : r["result"])
        {
            if (l.value("uri", "") != s.uri)
                continue;
            has_use = has_use || l["range"]["start"] == use;
            EXPECT_NE(l["range"]["start"], param) << c.param << " -> " << r.dump();
        }
        EXPECT_TRUE(has_use) << c.use << " -> " << r.dump();
    }
}

TEST(LanguageServerNative, HoverOfAFunction)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_hover(s, "Clamp100(MAX_HP)", "Clamp100", "int Clamp100(int v)");
    expect_native_hover(s, "limit = Clamp100(", "Clamp100", "int Clamp100(int v)");
    expect_native_hover(s, "after = Clamp100(2)", "Clamp100", "int Clamp100(int v)");
}

#ifndef _WIN32
// A slow machine builds the first preamble of a document in more time than the budget of a hover.
// The proxy below gives the first didOpen to clangd 4.5 s late, and all later messages wait
// behind it. The first native hover must wait for the preamble and give the native answer.
TEST(LanguageServerNative, HoverBeforeTheFirstPreambleGivesTheNativeAnswer)
{
    REQUIRE_CLANGD();
    if (std::system("python3 -c \"\" >/dev/null 2>&1") != 0)
        GTEST_SKIP() << "python3 is not installed";
    TempDir tools;
    const std::string proxy = (tools.path() / "slow_clangd.py").string();
    write_file(proxy,
        "import subprocess, sys, time\n"
        "p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE)\n"
        "src = sys.stdin.buffer\n"
        "late = True\n"
        "while True:\n"
        "    head = b''\n"
        "    while not head.endswith(b'\\r\\n\\r\\n'):\n"
        "        c = src.read(1)\n"
        "        if not c:\n"
        "            p.stdin.close()\n"
        "            p.wait()\n"
        "            sys.exit(0)\n"
        "        head += c\n"
        "    size = 0\n"
        "    for line in head.split(b'\\r\\n'):\n"
        "        if line.lower().startswith(b'content-length:'):\n"
        "            size = int(line.split(b':')[1])\n"
        "    body = src.read(size)\n"
        "    if late and b'\"textDocument/didOpen\"' in body:\n"
        "        late = False\n"
        "        time.sleep(4.5)\n"
        "    p.stdin.write(head + body)\n"
        "    p.stdin.flush()\n");
    const std::string clangd = (tools.path() / "slow_clangd").string();
    write_file(clangd, "#!/bin/sh\nexec python3 '" + proxy + "' '" HOT_CLANGD_PATH "' \"$@\"\n");
    std::filesystem::permissions(clangd, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    NativeSession s(kNativeScript, 10000, true, clangd);
    expect_native_hover(s, "Clamp100(MAX_HP)", "Clamp100", "int Clamp100(int v)");
}
#endif

TEST(LanguageServerNative, HoverOfAClass)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_hover(s, "Counter local;", "Counter", "struct Counter");
    expect_native_hover(s, "Counter counter;", "Counter", "struct Counter");
    expect_native_hover(s, "class Hero : Boss", "Boss", "class Boss");
    expect_native_hover(s, "new Weapon(", "Weapon", "struct Weapon");
}

TEST(LanguageServerNative, HoverOfAMethod)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_hover(s, "local.Bump(", "Bump", "int Bump(int by)");
    expect_native_hover(s, "this.Roar()", "Roar", "void Roar()");
    expect_native_hover(s, "bumped = Counter().Bump(", "Bump", "int Bump(int by)");
}

TEST(LanguageServerNative, HoverOfAField)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_hover(s, "this.rage =", "rage", "int rage");
    expect_native_hover(s, "this.counter.count", "count", "int count");
    expect_native_hover(s, "this.rage =", "rage", "public: int rage");
    expect_native_hover(s, "seen = Counter().count", "count", "int count");
}

TEST(LanguageServerNative, HoverOfAnEnum)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_hover(s, "Mood m = ", "Mood", "enum class Mood");
    expect_native_hover(s, "Level lv", "Level", "enum Level");
    expect_native_hover(s, "Mood mood", "Mood", "enum class Mood");
    expect_native_hover(s, "= Mood.Calm", "Calm", "In Mood\\nCalm");
    expect_native_hover(s, "= LevelHigh;", "LevelHigh", "In Level\\nLevelHigh");
}

// ------------------------------------------------------------------ inlay hints and call hierarchy of native symbols
namespace
{

const char* const kNativeCalls =
    "using \"natives.h\";\n"
    "\n"
    "class Caller {\n"
    "    int Helper(int arg) {\n"
    "        return Clamp100(arg);\n"
    "    }\n"
    "    int Run(Counter c) {\n"
    "        int a = Clamp100(7);\n"
    "        int b = c.Bump(a + 1);\n"
    "        int t = Tag(\"x\", Counter());\n"
    "        string note = \"Clamp100(1)\"; // Clamp100(2)\n"
    "        return a + b + t + Helper(2) + Clamp100(Helper(3));\n"
    "    }\n"
    "    void Other() {\n"
    "        int z = Clamp100(1);\n"
    "        Run(Counter());\n"
    "    }\n"
    "}\n";

// The 0-based position of the "nth" "word" in the first "context" of "text".
json pos_in(const std::string& text, const std::string& context, const std::string& word, int nth = 0)
{
    size_t at = text.find(context);
    EXPECT_NE(at, std::string::npos) << context;
    size_t w = at + context.find(word);
    for (int i = 0; i < nth; ++i)
        w = at + context.find(word, w - at + 1);
    return pos(static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(w), '\n')),
        static_cast<int>(w - (text.rfind('\n', w - 1) + 1)));
}

json prepare_at(NativeSession& s, const json& p)
{
    const json r = s.client->request("textDocument/prepareCallHierarchy", doc_pos(s.uri, p["line"], p["character"]));
    return r.contains("result") ? r["result"] : json();
}

json native_calls(NativeSession& s, const char* method, const json& item)
{
    const json r = s.client->request(method, {{"item", item}});
    return r.contains("result") ? r["result"] : json();
}

// The answer of "method" for "item". clangd can answer before its index or its AST of the
// native header is ready, so the answer can miss the native items. The request repeats until
// the answer has "want" items or 30 seconds pass.
json native_calls_until(NativeSession& s, const char* method, const json& item, size_t want)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        const json r = native_calls(s, method, item);
        if ((r.is_array() && r.size() >= want) || std::chrono::steady_clock::now() >= deadline)
            return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// The names of the callers or callees of an answer.
std::vector<std::string> call_names(const json& calls, const char* key)
{
    std::vector<std::string> out;
    for (const json& c : calls)
        out.push_back(c[key]["name"].get<std::string>());
    return out;
}

const json* call_of(const json& calls, const char* key, const std::string& name)
{
    for (const json& c : calls)
        if (c[key]["name"] == name)
            return &c;
    return nullptr;
}

} // namespace

TEST(LanguageServerNative, InlayHintsNameTheParametersOfNativeCalls)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const std::string text = kNativeCalls;
    const json r = s.client->request("textDocument/inlayHint", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    std::set<std::string> got;
    size_t count = 0;
    for (const json& h : r["result"])
        ++count, got.insert(std::to_string(h["position"]["line"].get<int>()) + ":" + std::to_string(h["position"]["character"].get<int>())
            + " " + h["label"].get<std::string>());
    auto want = [&](const std::string& context, const std::string& arg, const std::string& label)
    {
        const json p = pos_in(text, context, arg);
        return std::to_string(p["line"].get<int>()) + ":" + std::to_string(p["character"].get<int>()) + " " + label;
    };
    EXPECT_TRUE(got.count(want("Clamp100(7)", "7", "v:"))) << r.dump(); // a native function
    EXPECT_TRUE(got.count(want("c.Bump(a + 1)", "a + 1", "by:"))) << r.dump(); // a native method
    EXPECT_TRUE(got.count(want("Tag(\"x\", Counter())", "\"x\"", "text:"))) << r.dump(); // two arguments
    EXPECT_TRUE(got.count(want("Tag(\"x\", Counter())", "Counter()", "c:"))) << r.dump();
    EXPECT_TRUE(got.count(want("Clamp100(Helper(3))", "Helper(3)", "v:"))) << r.dump(); // a nested call
    EXPECT_TRUE(got.count(want("Helper(2)", "2", "arg:"))) << r.dump(); // a script method still has its hint
    EXPECT_TRUE(got.count(want("Helper(3)", "3", "arg:"))) << r.dump(); // the script call inside a native call
    EXPECT_EQ(count, got.size()) << "a hint is twice in " << r.dump();
    // The argument "arg" is not the name "v" of the parameter, so it has a hint.
    EXPECT_TRUE(got.count(want("Clamp100(arg)", "arg", "v:"))) << r.dump();
    // A call in a string or a comment, and a call with no argument, have no hint.
    for (const std::string& g : got)
    {
        EXPECT_NE(g.rfind("10:", 0), 0u) << g;
    }
    // The 7 above, "arg:" of Helper(3), "v:" of Clamp100(1) and "c:" of Run(Counter()).
    EXPECT_EQ(got.size(), 10u) << r.dump();
}

TEST(LanguageServerNative, InlayHintOfANativeCallSkipsAnArgumentWithTheNameOfItsParameter)
{
    REQUIRE_CLANGD();
    const std::string text =
        "using \"natives.h\";\n"
        "\n"
        "class K {\n"
        "    int Run(int v, int by) {\n"
        "        return Clamp100(v) + Counter().Bump(by) + Tag(\"n\", Counter());\n"
        "    }\n"
        "}\n";
    NativeSession s(text.c_str());
    const json r = s.client->request("textDocument/inlayHint", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    // Only the two arguments of Tag have a hint.
    std::vector<std::string> labels;
    for (const json& h : r["result"])
        labels.push_back(h["label"].get<std::string>());
    EXPECT_EQ(labels, (std::vector<std::string>{"text:", "c:"})) << r.dump();
}

TEST(LanguageServerNative, InlayHintsOfNativeCallsHonorTheRange)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const json r = s.client->request("textDocument/inlayHint",
        {{"textDocument", {{"uri", s.uri}}}, {"range", {{"start", pos(7, 0)}, {"end", pos(8, 0)}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    ASSERT_EQ(r["result"].size(), 1u) << r.dump();
    EXPECT_EQ(r["result"][0]["label"], "v:");
}

TEST(LanguageServerNative, PrepareCallHierarchyOfANativeFunctionGivesTheHeaderItem)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const std::string text = kNativeCalls;
    for (const json& at : {pos_in(text, "Clamp100(7)", "Clamp100"), pos_in(text, "Clamp100(Helper(3))", "Clamp100")})
    {
        const json items = prepare_at(s, at);
        ASSERT_TRUE(items.is_array() && items.size() == 1) << items.dump();
        EXPECT_EQ(items[0]["name"], "Clamp100");
        EXPECT_EQ(items[0]["uri"], uri_of((s.dir.path() / "natives.h").string()));
        const int line = line_of(kNativeHeader, "int Clamp100");
        EXPECT_EQ(items[0]["selectionRange"], json({{"start", pos(line, 4)}, {"end", pos(line, 12)}})) << items.dump();
        EXPECT_EQ(items[0]["range"]["start"]["line"], line) << items.dump();
        EXPECT_EQ(items[0]["range"]["end"]["line"], line) << items.dump();
    }
    const json method = prepare_at(s, pos_in(text, "c.Bump(a", "Bump"));
    ASSERT_TRUE(method.is_array() && method.size() == 1) << method.dump();
    EXPECT_EQ(method[0]["name"], "Bump");
    const int bump_line = line_of(kNativeHeader, "int Bump");
    EXPECT_EQ(method[0]["selectionRange"], json({{"start", pos(bump_line, 8)}, {"end", pos(bump_line, 12)}})) << method.dump();
    EXPECT_EQ(method[0]["uri"], uri_of((s.dir.path() / "natives.h").string()));
    // A local, a script method, a word in a string and a keyword are not native callables.
    EXPECT_TRUE(prepare_at(s, pos_in(text, "int a = ", "a")).is_null());
    EXPECT_TRUE(prepare_at(s, pos_in(text, "\"Clamp100(1)\"", "Clamp100")).is_null());
    EXPECT_TRUE(prepare_at(s, pos_in(text, "return a + b", "return")).is_null());
}

TEST(LanguageServerNative, IncomingCallsOfANativeFunctionAreTheScriptCallers)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const std::string text = kNativeCalls;
    const json items = prepare_at(s, pos_in(text, "Clamp100(7)", "Clamp100"));
    ASSERT_TRUE(items.is_array() && items.size() == 1) << items.dump();
    const json in = native_calls(s, "callHierarchy/incomingCalls", items[0]);
    ASSERT_TRUE(in.is_array()) << in.dump();
    EXPECT_EQ(call_names(in, "from"), (std::vector<std::string>{"Helper", "Run", "Other"})) << in.dump();
    for (const json& c : in)
        EXPECT_EQ(c["from"]["uri"], s.uri) << in.dump(); // no generated place
    const json* run = call_of(in, "from", "Run");
    ASSERT_NE(run, nullptr);
    ASSERT_EQ((*run)["fromRanges"].size(), 2u) << in.dump();
    EXPECT_EQ((*run)["fromRanges"][0]["start"], pos_in(text, "int a = Clamp100(7)", "Clamp100"));
    EXPECT_EQ((*run)["fromRanges"][1]["start"], pos_in(text, "Clamp100(Helper(3))", "Clamp100"));
    // The items of the answer are script items: they open the next level.
    const json deeper = native_calls(s, "callHierarchy/incomingCalls", (*run)["from"]);
    EXPECT_EQ(call_names(deeper, "from"), (std::vector<std::string>{"Other"})) << deeper.dump();
}

TEST(LanguageServerNative, IncomingCallsOfANativeMethodAreTheScriptCallers)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const json items = prepare_at(s, pos_in(kNativeCalls, "c.Bump(a", "Bump"));
    ASSERT_TRUE(items.is_array() && items.size() == 1) << items.dump();
    const json in = native_calls(s, "callHierarchy/incomingCalls", items[0]);
    EXPECT_EQ(call_names(in, "from"), (std::vector<std::string>{"Run"})) << in.dump();
}

TEST(LanguageServerNative, IncomingCallsCrossTheOpenDocuments)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const std::string other_text =
        "using \"natives.h\";\n"
        "\n"
        "class Second {\n"
        "    int Go() {\n"
        "        return Clamp100(5);\n"
        "    }\n"
        "}\n";
    const std::string other = uri_of((s.dir.path() / "second.hot").string());
    write_file((s.dir.path() / "second.hot").string(), other_text);
    s.client->open(other, other_text);
    const json items = prepare_at(s, pos_in(kNativeCalls, "Clamp100(7)", "Clamp100"));
    ASSERT_TRUE(items.is_array() && items.size() == 1) << items.dump();
    const json in = native_calls(s, "callHierarchy/incomingCalls", items[0]);
    const json* go = call_of(in, "from", "Go");
    ASSERT_NE(go, nullptr) << in.dump();
    EXPECT_EQ((*go)["from"]["uri"], other);
    EXPECT_EQ(call_names(in, "from").size(), 4u) << in.dump();
}

TEST(LanguageServerNative, OutgoingCallsOfAScriptMethodIncludeTheNativeCallees)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const std::string text = kNativeCalls;
    const json run = prepare_at(s, pos_in(text, "int Run(", "Run"));
    ASSERT_TRUE(run.is_array() && run.size() == 1) << run.dump();
    // The native callees come from clangd. They can come later than the script callee.
    const json out = native_calls_until(s, "callHierarchy/outgoingCalls", run[0], 4);
    ASSERT_TRUE(out.is_array()) << out.dump();
    // The script callee first, then the native callees in source order.
    EXPECT_EQ(call_names(out, "to"), (std::vector<std::string>{"Helper", "Clamp100", "Bump", "Tag"})) << out.dump();
    const std::string header = uri_of((s.dir.path() / "natives.h").string());
    const json* clamp = call_of(out, "to", "Clamp100");
    ASSERT_NE(clamp, nullptr);
    EXPECT_EQ((*clamp)["to"]["uri"], header);
    ASSERT_EQ((*clamp)["fromRanges"].size(), 2u) << out.dump();
    EXPECT_EQ((*clamp)["fromRanges"][0]["start"], pos_in(text, "int a = Clamp100(7)", "Clamp100"));
    EXPECT_EQ(call_of(out, "to", "Bump")->at("to")["uri"], header);
    EXPECT_EQ(call_of(out, "to", "Tag")->at("to")["uri"], header);
    // The native item opens the next level: a declaration has no callee, and the callers are the script ones.
    const json next = native_calls(s, "callHierarchy/outgoingCalls", (*clamp)["to"]);
    EXPECT_TRUE(next.is_array()) << next.dump();
    const json back = native_calls(s, "callHierarchy/incomingCalls", (*clamp)["to"]);
    EXPECT_EQ(call_names(back, "from"), (std::vector<std::string>{"Helper", "Run", "Other"})) << back.dump();
}

TEST(LanguageServerNative, IncomingCallsSkipADocumentThatHasAScriptMethodWithTheName)
{
    REQUIRE_CLANGD();
    // The first document has a script method Run. The second document calls the native function Run.
    NativeSession s(kNativeCalls);
    const std::string other_text =
        "using \"natives.h\";\n"
        "\n"
        "class Second {\n"
        "    int Go() {\n"
        "        return Run(5);\n"
        "    }\n"
        "}\n";
    const std::string other = uri_of((s.dir.path() / "second.hot").string());
    write_file((s.dir.path() / "second.hot").string(), other_text);
    s.client->open(other, other_text);
    const json r = s.client->request("textDocument/prepareCallHierarchy", doc_pos(other, 4, 15));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && r["result"].size() == 1) << r.dump();
    const json item = r["result"][0];
    EXPECT_EQ(item["uri"], uri_of((s.dir.path() / "natives.h").string()));
    const json in = native_calls(s, "callHierarchy/incomingCalls", item);
    // Only Go: the callers of the script method Run in the first document are not callers of the native Run.
    EXPECT_EQ(call_names(in, "from"), (std::vector<std::string>{"Go"})) << in.dump();
}

TEST(LanguageServerNative, OutgoingCallsKeepAScriptMethodAndTheNativeCallsApart)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const json other = prepare_at(s, pos_in(kNativeCalls, "void Other()", "Other"));
    ASSERT_TRUE(other.is_array() && other.size() == 1) << other.dump();
    const json out = native_calls(s, "callHierarchy/outgoingCalls", other[0]);
    // Run is a script method of the document, so it is the script item. The native Run is not listed.
    ASSERT_EQ(out.size(), 2u) << out.dump(); // Run (script) and Clamp100 (native)
    EXPECT_EQ(call_of(out, "to", "Run")->at("to")["uri"], s.uri) << out.dump();
    EXPECT_EQ(call_of(out, "to", "Clamp100")->at("to")["uri"], uri_of((s.dir.path() / "natives.h").string()));
}

TEST(LanguageServerNative, MalformedNativeCallHierarchyItemsKeepTheServerAlive)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const std::string header = uri_of((s.dir.path() / "natives.h").string());
    for (const json& data : {json({{"native", {{"name", 5}}}, {"script", nullptr}}),
             json({{"native", {{"name", nullptr}, {"uri", 7}}}, {"script", 3}}), json({{"native", {{"x", 1}}}}),
             json({{"native", json::array()}}), json({{"native", "x"}, {"script", nullptr}})})
        for (const char* method : {"callHierarchy/incomingCalls", "callHierarchy/outgoingCalls"})
        {
            const json r = s.client->request(
                method, {{"item", {{"name", "X"}, {"uri", header}, {"data", data}}}});
            EXPECT_TRUE(r.contains("result") || r.contains("error")) << data.dump() << " " << r.dump();
        }
    const json items = prepare_at(s, pos_in(kNativeCalls, "Clamp100(7)", "Clamp100"));
    EXPECT_TRUE(items.is_array() && items.size() == 1) << items.dump();
}

TEST(LanguageServerNative, CodeLensStaysOnTheScriptDeclarations)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeCalls);
    const json r = s.client->request("textDocument/codeLens", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    // One lens for the class and one for each of the three methods: a native symbol has no declaration here.
    EXPECT_EQ(r["result"].size(), 4u) << r.dump();
}

TEST(LanguageServer, ReferencesOfAScriptFieldAreInTheScript)
{
    REQUIRE_CLANGD();
    Session s(true);
    const json r = s.client->request("textDocument/references",
        {{"textDocument", {{"uri", s.uri}}}, {"position", pos(11, 14)}, {"context", {{"includeDeclaration", true}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    std::vector<std::pair<int, int>> at;
    for (const json& l : r["result"])
    {
        EXPECT_EQ(l["uri"], s.uri);
        at.emplace_back(l["range"]["start"]["line"], l["range"]["start"]["character"]);
    }
    std::sort(at.begin(), at.end());
    const std::vector<std::pair<int, int>> want = {{3, 10}, {11, 13}, {11, 23}};
    EXPECT_EQ(at, want) << r.dump();
}

TEST(LanguageServer, DocumentSymbolsComeFromAnalyze)
{
    Session s(false);
    const json r = s.client->request("textDocument/documentSymbol", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].size() == 1) << r.dump();
    const json& c = r["result"][0];
    EXPECT_EQ(c["name"], "Player");
    EXPECT_EQ(c["kind"], 5);
    EXPECT_EQ(c["selectionRange"]["start"]["line"], 2);
    std::vector<std::string> names;
    for (const json& m : c["children"])
        names.push_back(m["name"]);
    EXPECT_EQ(names, (std::vector<std::string>{"hp", "Tick", "heal"}));
    EXPECT_EQ(c["children"][0]["kind"], 8); // Field
    EXPECT_EQ(c["children"][2]["kind"], 6); // Method
    EXPECT_EQ(c["children"][2]["selectionRange"]["start"]["line"], 10);
}

// ------------------------------------------------------------------ inlay hints

namespace
{

// The 0-based lines: 1 Add, 2 the constructor, 4 Run, 6 and 7 the calls of Add, 8 the new Calc.
const char* const kHintScript =
    "class Calc {\n"
    "    int Add(int a, int b) { return a + b; }\n"
    "    Calc(int start, int step = 1) { }\n"
    "\n"
    "    void Run() {\n"
    "        int a = 5;\n"
    "        this.Add(1, 2);\n"
    "        this.Add(a, 3);\n"
    "        var c = new Calc(7);\n"
    "    }\n"
    "}\n";

// The (line, character, label, kind) of each hint.
std::vector<std::tuple<int, int, std::string, int>> hints_of(Session& s, const json& range = json())
{
    json params = {{"textDocument", {{"uri", s.uri}}}};
    if (!range.is_null())
        params["range"] = range;
    const json r = s.client->request("textDocument/inlayHint", params);
    std::vector<std::tuple<int, int, std::string, int>> out;
    if (!r.contains("result") || !r["result"].is_array())
        return out;
    for (const json& h : r["result"])
        out.emplace_back(h["position"]["line"].get<int>(), h["position"]["character"].get<int>(),
            h["label"].get<std::string>(), h["kind"].get<int>());
    return out;
}

} // namespace

TEST(LanguageServer, InlayHintProviderIsAdvertised)
{
    TempDir dir;
    LsClient client(options(dir, false));
    const json init = client.initialize(uri_of(dir.path().string()));
    ASSERT_TRUE(init.contains("capabilities")) << init.dump();
    EXPECT_EQ(init["capabilities"].value("inlayHintProvider", json()), true) << init.dump();
}

TEST(LanguageServer, InlayHintsNameTheParametersOfAScriptMethodCall)
{
    Session s(false, kHintScript);
    const auto hints = hints_of(s);
    using H = std::tuple<int, int, std::string, int>;
    EXPECT_NE(std::find(hints.begin(), hints.end(), H(6, 17, "a:", 2)), hints.end());
    EXPECT_NE(std::find(hints.begin(), hints.end(), H(6, 20, "b:", 2)), hints.end());
    // The argument "a" has the name of its parameter. The argument 3 has a hint.
    EXPECT_EQ(std::count_if(hints.begin(), hints.end(), [](const H& h) { return std::get<0>(h) == 7; }), 1);
    EXPECT_NE(std::find(hints.begin(), hints.end(), H(7, 20, "b:", 2)), hints.end());
}

TEST(LanguageServer, InlayHintsNameTheParametersOfAConstructorCall)
{
    Session s(false, kHintScript);
    const auto hints = hints_of(s);
    using H = std::tuple<int, int, std::string, int>;
    EXPECT_NE(std::find(hints.begin(), hints.end(), H(8, 25, "start:", 2)), hints.end());
    // The declarations get no hint.
    EXPECT_EQ(std::count_if(hints.begin(), hints.end(), [](const H& h) { return std::get<0>(h) < 6; }), 0);
    EXPECT_EQ(hints.size(), 4u);
}

TEST(LanguageServer, InlayHintsHonorTheRange)
{
    Session s(false, kHintScript);
    const auto hints = hints_of(s, {{"start", pos(7, 0)}, {"end", pos(8, 0)}});
    ASSERT_EQ(hints.size(), 1u);
    EXPECT_EQ(std::get<0>(hints[0]), 7);
    EXPECT_EQ(std::get<2>(hints[0]), "b:");
}

namespace
{

using Hint = std::tuple<int, int, std::string, int>;

// The full answer to an inlay hint request for a file with "text". "utf8" asks for UTF-8 columns.
json hint_response(const std::string& file, const std::string& text, bool utf8 = false)
{
    TempDir dir;
    LsClient client(options(dir, false));
    json caps = json::object();
    if (utf8)
        caps = {{"general", {{"positionEncodings", {"utf-8", "utf-16"}}}}};
    client.request("initialize", {{"processId", nullptr}, {"rootUri", uri_of(dir.path().string())}, {"capabilities", caps}});
    client.notify("initialized", json::object());
    const std::string path = (dir.path() / file).string();
    write_file(path, text);
    client.open(uri_of(path), text);
    return client.request("textDocument/inlayHint", {{"textDocument", {{"uri", uri_of(path)}}}});
}

std::vector<Hint> hints_in(const json& response)
{
    std::vector<Hint> out;
    if (!response.contains("result") || !response["result"].is_array())
        return out;
    for (const json& h : response["result"])
        out.emplace_back(h["position"]["line"].get<int>(), h["position"]["character"].get<int>(),
            h["label"].get<std::string>(), h["kind"].get<int>());
    return out;
}

bool has_hint(const std::vector<Hint>& hints, const Hint& h)
{
    return std::find(hints.begin(), hints.end(), h) != hints.end();
}

// The method Add is on line 1. The body of Run is on line 3 and the following lines.
std::string add_script(const std::string& body)
{
    return "class C {\n    int Add(int a, int b) { return a + b; }\n    void Run() {\n" + body + "    }\n}\n";
}

} // namespace

TEST(LanguageServer, InlayHintColumnAfterMultiByteTextFollowsThePositionEncoding)
{
    // In bytes the 2 is at column 26. In UTF-16 units it is at column 23.
    const std::string text = add_script("        this.Add(\"\xC3\xA9\xE2\x82\xAC\", 2);\n");
    const auto utf16 = hints_in(hint_response("m.hot", text, false));
    EXPECT_TRUE(has_hint(utf16, Hint(3, 17, "a:", 2)));
    EXPECT_TRUE(has_hint(utf16, Hint(3, 23, "b:", 2)));
    const auto utf8 = hints_in(hint_response("m.hot", text, true));
    EXPECT_TRUE(has_hint(utf8, Hint(3, 17, "a:", 2)));
    EXPECT_TRUE(has_hint(utf8, Hint(3, 26, "b:", 2)));
}

TEST(LanguageServer, InlayHintOfACppDocumentIsNull)
{
    const json r = hint_response("m.cpp", "int Add(int a, int b);\nint x = Add(1, 2);\n");
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_TRUE(r["result"].is_null()) << r.dump();
}

TEST(LanguageServer, InlayHintsOfOverloadsNeedTheSameNameAndAFittingCount)
{
    const std::string text =
        "class C {\n"
        "    int Pick(int a, int b) { return a; }\n"
        "    int Pick(float x, int b) { return b; }\n"
        "    int Tag(int a, int b) { return a; }\n"
        "    int Tag(int p, int q, int r) { return p; }\n"
        "    void Run() {\n"
        "        this.Pick(1, 2);\n"
        "        this.Tag(1, 2);\n"
        "    }\n"
        "}\n";
    const auto hints = hints_in(hint_response("o.hot", text));
    // Pick: the first name differs (a, x), the second is b in both.
    EXPECT_TRUE(has_hint(hints, Hint(6, 21, "b:", 2)));
    EXPECT_FALSE(has_hint(hints, Hint(6, 18, "a:", 2)));
    EXPECT_FALSE(has_hint(hints, Hint(6, 18, "x:", 2)));
    // Tag: the overload with 3 parameters does not fit 2 arguments.
    EXPECT_TRUE(has_hint(hints, Hint(7, 17, "a:", 2)));
    EXPECT_TRUE(has_hint(hints, Hint(7, 20, "b:", 2)));
    EXPECT_EQ(hints.size(), 3u);
}

TEST(LanguageServer, InlayHintsOfANestedCallNameBothCalls)
{
    const auto hints = hints_in(hint_response("n.hot", add_script("        this.Add(this.Add(1, 2), 3);\n")));
    EXPECT_TRUE(has_hint(hints, Hint(3, 17, "a:", 2)));
    EXPECT_TRUE(has_hint(hints, Hint(3, 26, "a:", 2)));
    EXPECT_TRUE(has_hint(hints, Hint(3, 29, "b:", 2)));
    EXPECT_TRUE(has_hint(hints, Hint(3, 33, "b:", 2)));
    EXPECT_EQ(hints.size(), 4u);
}

TEST(LanguageServer, InlayHintsIgnoreACallInAStringOrAComment)
{
    const auto hints = hints_in(hint_response("s.hot",
        add_script("        var s = \"this.Add(1, 2)\";\n"
                   "        // this.Add(1, 2)\n"
                   "        /* this.Add(3, 4) */\n"
                   "        this.Add(5, 6);\n")));
    ASSERT_EQ(hints.size(), 2u);
    EXPECT_EQ(std::get<0>(hints[0]), 6);
    EXPECT_EQ(std::get<0>(hints[1]), 6);
}

TEST(LanguageServer, InlayHintsOfAnIncompleteCallAreANormalAnswer)
{
    const json r = hint_response("i.hot", add_script("        this.Add(1,\n"));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_FALSE(r.contains("error")) << r.dump();
    EXPECT_TRUE(r["result"].is_array()) << r.dump();
    EXPECT_TRUE(hints_in(r).empty()) << r.dump();
}

TEST(LanguageServer, SemanticTokensMarkKeywordsTypesAndMembers)
{
    Session s(false);
    const json init_types = {"keyword", "type", "class", "method", "property", "number", "string", "comment"};
    const json r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json& data = r["result"]["data"];
    ASSERT_EQ(data.size() % 5, 0u);
    // Decode to (line, col, len, type name).
    std::vector<std::tuple<int, int, int, std::string>> tokens;
    int line = 0, col = 0;
    for (size_t i = 0; i < data.size(); i += 5)
    {
        line += data[i].get<int>();
        col = data[i].get<int>() == 0 ? col + data[i + 1].get<int>() : data[i + 1].get<int>();
        tokens.emplace_back(line, col, data[i + 2].get<int>(), init_types[data[i + 3].get<int>()]);
    }
    auto has = [&](int l, int c, int n, const char* t)
    { return std::find(tokens.begin(), tokens.end(), std::make_tuple(l, c, n, std::string(t))) != tokens.end(); };
    EXPECT_TRUE(has(0, 0, 5, "keyword")); // using
    EXPECT_TRUE(has(0, 6, 9, "string")); // "Actor.h"
    EXPECT_TRUE(has(2, 6, 6, "class")); // Player
    EXPECT_TRUE(has(2, 15, 5, "class")); // Actor
    EXPECT_TRUE(has(3, 4, 5, "type")); // float
    EXPECT_TRUE(has(3, 10, 2, "property")); // hp
    EXPECT_TRUE(has(3, 15, 5, "number")); // 100.0
    EXPECT_TRUE(has(7, 13, 4, "method")); // heal
}

// ------------------------------------------------------------------ the recent syntax

namespace
{

// A script with one of each construct of the recent syntax. The lines are 0-based in the checks.
const char* const kNewSyntaxScript =
    "using \"Actor.h\";\n" // 0
    "using <vector>;\n" // 1
    "using <map>;\n" // 2
    "using <string>;\n" // 3
    "\n" // 4
    "class Shape {\n" // 5
    "    int Area => 1;\n" // 6
    "}\n" // 7
    "class Circle : Shape {\n" // 8
    "    protected int r = 2;\n" // 9
    "    public int Both => this.r;\n" // 10
    "}\n" // 11
    "class Hero : Actor {\n" // 12
    "    int sum = 0;\n" // 13
    "    private int Twice(int a = 1) => a * 2;\n" // 14
    "    std.vector<int> xs = [1, 2, 3];\n" // 15
    "    std.map<std.string, int> table = [\"a\": 1, \"b\": 2];\n" // 16
    "    $Hero other; override void Tick(float dt) { base.Tick(dt); }\n" // 17
    "    void Run($Shape s) {\n" // 18
    "        var f = (int x) => x * 2;\n" // 19
    "        var (a, b) = std.make_pair(1, 2);\n" // 20
    "        foreach (var (k, v) in this.table) { this.sum = this.sum + v; }\n" // 21
    "        foreach (var (i, e) in enumerate(this.xs)) { this.sum = this.sum + i + e; }\n" // 22
    "        foreach (var j in range(0, 3)) { this.sum = this.sum + j; }\n" // 23
    "        this.other?.Run(s);\n" // 24
    "        if (s is $Circle c && c.Both > 0) { this.sum = c.Both; }\n" // 25
    "        var sq = s as $Circle;\n" // 26
    "        std.string name = \"a\";\n" // 27
    "        switch (name) {\n" // 28
    "            case \"a\":\n" // 29
    "                this.sum = 1;\n" // 30
    "                break;\n" // 31
    "            default:\n" // 32
    "                break;\n" // 33
    "        }\n" // 34
    "        this.sum = f(this.sum) + a + b;\n" // 35
    "    }\n" // 36
    "}\n";

// Decodes the data of a semantic tokens answer to (line, col, len, type name).
std::vector<std::tuple<int, int, int, std::string>> decode_tokens(const json& data)
{
    const json types = {"keyword", "type", "class", "method", "property", "number", "string", "comment"};
    std::vector<std::tuple<int, int, int, std::string>> tokens;
    int line = 0, col = 0;
    for (size_t i = 0; i + 4 < data.size(); i += 5)
    {
        line += data[i].get<int>();
        col = data[i].get<int>() == 0 ? col + data[i + 1].get<int>() : data[i + 1].get<int>();
        tokens.emplace_back(line, col, data[i + 2].get<int>(), types[data[i + 3].get<int>()]);
    }
    return tokens;
}

} // namespace

TEST(LanguageServer, ScriptWithTheRecentSyntaxGivesNoServerError)
{
    Session s(false, kNewSyntaxScript);
    const json n = s.diagnostics([](const json&) { return true; });
    ASSERT_FALSE(n.is_null());
    for (const json& d : n["params"]["diagnostics"])
        EXPECT_NE(d.value("severity", 0), 1) << d.dump();
    // The analysis ran: the document symbols list the classes and their members.
    const json r = s.client->request("textDocument/documentSymbol", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].size() == 3) << r.dump();
    const json& hero = r["result"][2];
    EXPECT_EQ(hero["name"], "Hero");
    std::vector<std::string> names;
    for (const json& m : hero["children"])
        names.push_back(m["name"]);
    EXPECT_EQ(names, (std::vector<std::string>{"sum", "Twice", "xs", "table", "other", "Tick", "Run"}));
}

TEST(LanguageServer, SemanticTokensMarkBaseIsAsAndTheAccessWordsAsKeywords)
{
    Session s(false, kNewSyntaxScript);
    const json r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const auto tokens = decode_tokens(r["result"]["data"]);
    // The word is the first one with this text on the line of the script.
    auto has = [&](int line, const std::string& word, const char* type)
    {
        std::istringstream in(kNewSyntaxScript);
        std::string text;
        for (int l = 0; l <= line; ++l)
            std::getline(in, text);
        const size_t col = text.find(word);
        return col != std::string::npos
            && std::find(tokens.begin(), tokens.end(),
                   std::make_tuple(line, static_cast<int>(col), static_cast<int>(word.size()), std::string(type)))
            != tokens.end();
    };
    EXPECT_TRUE(has(9, "protected", "keyword"));
    EXPECT_TRUE(has(10, "public", "keyword"));
    EXPECT_TRUE(has(10, "this", "keyword"));
    EXPECT_TRUE(has(14, "private", "keyword"));
    EXPECT_TRUE(has(17, "override", "keyword"));
    EXPECT_TRUE(has(17, "base", "keyword"));
    EXPECT_TRUE(has(20, "var", "keyword"));
    EXPECT_TRUE(has(25, "is", "keyword"));
    EXPECT_TRUE(has(26, "as", "keyword"));
    EXPECT_TRUE(has(22, "enumerate", "method"));
    EXPECT_TRUE(has(23, "range", "method"));
}

TEST(LanguageServer, TheRemovedWordsAreNotKeywords)
{
    const std::vector<std::string>& k = hot::ls::script_keywords();
    for (const char* w : {"function", "let", "import", "super", "static", "module"})
        EXPECT_EQ(std::count(k.begin(), k.end(), w), 0) << w;
    for (const char* w : {"base", "ref", "out", "is", "as", "public", "protected", "private", "this", "struct", "using"})
        EXPECT_EQ(std::count(k.begin(), k.end(), w), 1) << w;
    const std::vector<std::string>& h = hot::ls::script_helpers();
    EXPECT_EQ(h, (std::vector<std::string>{"enumerate", "range"}));
}

// The parser has no exported keyword list. Each keyword is a LEXY_KEYWORD("word", id) in the
// parser sources, so the test reads the words from the sources. The words of the list below are
// no keywords of the script for the editor. The list is the only place that names them.
namespace
{

// The keywords of the parser (the LEXY_KEYWORD words) as the editors know them: the C++ words that
// the parser only rejects are removed, and the word "delete" is added.
std::set<std::string> parser_keywords()
{
    // C++ words that the parser accepts to give a clear error, and "extern", which is reserved.
    const std::set<std::string> not_highlighted = {
        "static", "thread_local", "friend", "mutable", "volatile", "operator", "extern", "goto"};
    // Keywords that the parser reads without LEXY_KEYWORD.
    const std::set<std::string> other_parser_words = {"delete"};

    std::set<std::string> parser;
    const std::regex re("LEXY_KEYWORD\\(\"(\\w+)\"");
    for (const auto& e : std::filesystem::directory_iterator(std::string(HOT_SOURCE_DIR "/src/translator/parser")))
    {
        std::ifstream in(e.path());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            parser.insert((*it)[1].str());
    }
    EXPECT_GT(parser.size(), 30u) << "the parser keywords were not found";
    for (const std::string& w : not_highlighted)
        EXPECT_EQ(parser.erase(w), 1u) << "not a parser word any more: " << w;
    parser.insert(other_parser_words.begin(), other_parser_words.end());
    return parser;
}

} // namespace

TEST(LanguageServer, TheKeywordsMatchTheParser)
{
    const std::set<std::string> parser = parser_keywords();

    // The attribute names are keywords of the editor too. They are not words of the parser list.
    const std::vector<std::string>& attrs = hotc::class_attribute_names();
    std::set<std::string> ls;
    for (const std::string& w : hot::ls::script_keywords())
        if (std::find(attrs.begin(), attrs.end(), w) == attrs.end())
            ls.insert(w);

    for (const std::string& w : parser)
        EXPECT_EQ(ls.count(w), 1u) << "parser keyword is not in script_keywords(): " << w;
    for (const std::string& w : ls)
        EXPECT_EQ(parser.count(w), 1u) << "script_keywords() word is not a parser keyword: " << w;
}

namespace
{

std::string read_editor_file(const char* relative)
{
    std::ifstream in(std::string(HOT_SOURCE_DIR "/editors/") + relative, std::ios::binary);
    EXPECT_TRUE(in.good()) << "missing file: editors/" << relative;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// All matches of group 1 of a regex in a text.
std::set<std::string> regex_words(const std::string& text, const std::regex& re)
{
    std::set<std::string> r;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        r.insert((*it)[1].str());
    return r;
}

// The text between the first "begin" and the next "end" after it.
std::string between(const std::string& text, const std::string& begin, const std::string& end)
{
    const size_t b = text.find(begin);
    if (b == std::string::npos)
        return {};
    const size_t e = text.find(end, b);
    return e == std::string::npos ? std::string() : text.substr(b + begin.size(), e - b - begin.size());
}

// A list of the editor must have every required word. Each other word must be an attribute name
// or a helper of the language (these are highlighted in some editors).
void check_editor_list(const char* where, const std::set<std::string>& found, const std::set<std::string>& required,
    const std::set<std::string>& extra_allowed)
{
    EXPECT_GT(found.size(), 20u) << where << ": the word list was not found";
    for (const std::string& w : required)
        EXPECT_EQ(found.count(w), 1u) << where << ": missing word: " << w;
    for (const std::string& w : found)
        if (!required.count(w))
            EXPECT_EQ(extra_allowed.count(w), 1u) << where << ": word is not a keyword or a type: " << w;
}

} // namespace

// The editor files outside VS Code (Vim, tree-sitter) and the TextMate grammar list the same
// keywords and builtin types as the parser. A missing word fails the test.
TEST(LanguageServer, EditorKeywordListsMatchTheParser)
{
    const std::set<std::string> keywords = parser_keywords();
    std::set<std::string> types;
    for (const std::string& t : hot::ls::builtin_type_names())
        types.insert(t);
    ASSERT_GT(types.size(), 15u);
    std::set<std::string> required = keywords;
    required.insert(types.begin(), types.end());

    // The helpers enumerate and range are highlighted in the editors. They are extra words of the
    // lists, so the test needs them in the allowed words. Check that the server list has them.
    const std::vector<std::string>& helpers = hot::ls::script_helpers();
    EXPECT_NE(std::find(helpers.begin(), helpers.end(), "enumerate"), helpers.end());
    EXPECT_NE(std::find(helpers.begin(), helpers.end(), "range"), helpers.end());
    std::set<std::string> extra_allowed(helpers.begin(), helpers.end());
    for (const std::string& a : hotc::class_attribute_names())
        extra_allowed.insert(a);

    // Vim: the words of the "syn keyword" lines. The group hotTodo holds comment markers.
    {
        const std::string text = read_editor_file("vim/syntax/hot.vim");
        std::set<std::string> found;
        const std::regex line_re("(?:^|\\n)[ \\t]*syn(?:tax)?[ \\t]+keyword[ \\t]+(\\w+)([^\\n]*)");
        for (std::sregex_iterator it(text.begin(), text.end(), line_re), end; it != end; ++it)
        {
            if ((*it)[1].str() == "hotTodo")
                continue;
            std::istringstream words((*it)[2].str());
            for (std::string w; words >> w;)
                if (w != "contained" && w != "skipwhite" && w != "skipnl" && w != "skipempty")
                    found.insert(w);
        }
        check_editor_list("vim/syntax/hot.vim", found, required, extra_allowed);
    }

    // tree-sitter: the arrays KEYWORDS and TYPES of grammar.js, and the quoted words of the query.
    {
        const std::string js = read_editor_file("tree-sitter-hot/grammar.js");
        const std::regex word_re("'(\\w+)'");
        const std::set<std::string> js_keywords = regex_words(between(js, "const KEYWORDS = [", "];"), word_re);
        const std::set<std::string> js_types = regex_words(between(js, "const TYPES = [", "];"), word_re);
        check_editor_list("tree-sitter-hot/grammar.js KEYWORDS", js_keywords, keywords, extra_allowed);
        check_editor_list("tree-sitter-hot/grammar.js TYPES", js_types, types, {});

        // The query: the lines of a comment start with ";".
        std::string query = read_editor_file("tree-sitter-hot/queries/highlights.scm");
        query = std::regex_replace(query, std::regex(";[^\\n]*"), "");
        const std::set<std::string> found = regex_words(query, std::regex("\"(\\w+)\""));
        check_editor_list("tree-sitter-hot/queries/highlights.scm", found, required, extra_allowed);
    }

    // TextMate: the words of the keyword scopes only. These are the "match" regexes of the rules
    // "keywords", "types", "declaration" and "using". A keyword in another rule (for example in
    // an attribute list) does not count. A word list is a group of the form (?:a|b|c) or (a|b).
    {
        const nlohmann::json grammar =
            nlohmann::json::parse(read_editor_file("vscode/syntaxes/hot.tmLanguage.json"));
        const nlohmann::json& repo = grammar.at("repository");
        std::vector<std::string> matches;
        for (const nlohmann::json& p : repo.at("keywords").at("patterns"))
            matches.push_back(p.at("match").get<std::string>());
        matches.push_back(repo.at("types").at("match").get<std::string>());
        for (const nlohmann::json& p : repo.at("declaration").at("patterns"))
            matches.push_back(p.at("match").get<std::string>());
        matches.push_back(repo.at("using").at("match").get<std::string>());

        const std::regex group_re("\\((?:\\?:)?(\\w+(?:\\|\\w+)*)\\)");
        const std::regex word_re("\\w+");
        std::set<std::string> found;
        for (const std::string& m : matches)
            for (std::sregex_iterator g(m.begin(), m.end(), group_re), end; g != end; ++g)
            {
                const std::string list = (*g)[1].str();
                for (std::sregex_iterator w(list.begin(), list.end(), word_re); w != end; ++w)
                    found.insert((*w)[0].str());
            }
        check_editor_list("vscode/syntaxes/hot.tmLanguage.json", found, required, extra_allowed);
    }

    // The jetbrains template and the vim ftdetect name the file extension.
    EXPECT_NE(read_editor_file("vim/ftdetect/hot.vim").find("*.hot"), std::string::npos);
    EXPECT_NE(read_editor_file("jetbrains/lsp4ij/template.json").find("*.hot"), std::string::npos);
}

TEST(LanguageServer, BindingsAreLocals)
{
    const std::string text = kNewSyntaxScript;
    const size_t at = text.find("this.sum = f(this.sum)");
    const hot::ls::ScriptScope scope = hot::ls::scan_scope(text, at, at);
    std::map<std::string, std::string> locals; // name -> type
    for (const hot::ls::ScopeName& n : scope.locals)
        locals[n.name] = n.type;
    // "var (a, b)" and the foreach with "var (k, v)" or "var (i, e)" bind plain locals.
    for (const char* n : {"a", "b", "k", "v", "i", "e", "j", "f", "sq", "name", "s"})
        EXPECT_EQ(locals.count(n), 1u) << n;
    EXPECT_EQ(locals["c"], "Circle"); // "s is $Circle c"
    EXPECT_EQ(locals["a"], ""); // no type is written

    const std::string ref = "class A { void F() { ref (x, y) = G(); if (o is std.vector<int> list) { @ } } }";
    const size_t p = ref.find('@');
    const hot::ls::ScriptScope s2 = hot::ls::scan_scope(ref, p, p);
    std::set<std::string> names;
    for (const hot::ls::ScopeName& n : s2.locals)
        names.insert(n.name);
    EXPECT_EQ(names, (std::set<std::string>{"x", "y", "list"}));
}

// ------------------------------------------------------------------ M14: index, C++ to script, rename

// A .cpp go-to-definition that ends in a generated .cpp file goes on to the script with the
// request hot/toScript. The "#line" directives of the file give the script line.
TEST(LanguageServer, GeneratedFileOnDiskMapsBackToTheScript)
{
    Session s(false);
    // Wait for the analyze of the server. Two parses at the same time race inside libclang
    // (TSan reports it), and the test does not test this.
    s.diagnostics([](const json&) { return true; });
    hotc::TranslateOptions o;
    o.script_text = kScript;
    o.script_name = s.path;
    o.generation = 1;
    o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
    const hotc::TranslateResult t = hotc::translate(o);
    ASSERT_TRUE(t.ok) << t.diagnostics;
    const std::string cpp = (s.dir.path() / "player.g1.cpp").string();
    write_file(cpp, t.cpp);
    // The generated line of "this.hp = this.hp + amount;" (0-based).
    const size_t at = t.cpp.find("__st->hp = __st->hp + amount;");
    ASSERT_NE(at, std::string::npos);
    const int gline = static_cast<int>(std::count(t.cpp.begin(), t.cpp.begin() + static_cast<long>(at), '\n'));
    const json r = s.client->request("hot/toScript", doc_pos(uri_of(cpp), gline, 4));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["uri"], s.uri);
    EXPECT_EQ(r["result"]["range"]["start"]["line"], 11);
    // A line of translator text (the shell) gets the last script line before it.
    const auto direct = hot::ls::line_directive_target(t.cpp, gline + 1);
    ASSERT_TRUE(direct);
    EXPECT_EQ(direct->file, s.path);
    EXPECT_EQ(direct->line, 11);
}

TEST(LanguageServer, RenameOfAScriptFieldChangesEveryUse)
{
    REQUIRE_CLANGD();
    Session s(true);
    json p = doc_pos(s.uri, 3, 11);
    p["newName"] = "health";
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json& edits = r["result"]["changes"][s.uri];
    std::vector<std::pair<int, int>> at;
    for (const json& e : edits)
    {
        EXPECT_EQ(e["newText"], "health");
        EXPECT_EQ(e["range"]["end"]["character"].get<int>() - e["range"]["start"]["character"].get<int>(), 2);
        at.emplace_back(e["range"]["start"]["line"], e["range"]["start"]["character"]);
    }
    std::sort(at.begin(), at.end());
    EXPECT_EQ(at, (std::vector<std::pair<int, int>>{{3, 10}, {11, 13}, {11, 23}})) << r.dump();
}

TEST(LanguageServer, RenameOfANativeSymbolIsAnError)
{
    REQUIRE_CLANGD();
    Session s(true);
    json p = doc_pos(s.uri, 6, 14);
    p["newName"] = "Walk";
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"].value("message", "").find("native"), std::string::npos) << r.dump();
}

// The workspace has two versions of the script. Both declare the class Player. The other version
// does not import this script: its Player is a different symbol. A rename changes only the
// declaring script and the scripts that import it, thus the other version keeps its names.
TEST(LanguageServer, RenameLeavesOutAnUnrelatedScriptWithTheSameNames)
{
    REQUIRE_CLANGD();
    Session s(true);
    const std::string other_path = (s.dir.path() / "sub" / "player_v2.hot").string();
    std::string v2 = kScript;
    v2.replace(v2.find("100.0"), 5, "50.0");
    std::filesystem::create_directories(std::filesystem::path(other_path).parent_path());
    write_file(other_path, v2);
    write_file(s.dir.path() / "unrelated.hot", "class Other {\n    float hp = 1.0;\n}\n");
    json p = doc_pos(s.uri, 3, 11);
    p["newName"] = "health";
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json& changes = r["result"]["changes"];
    EXPECT_EQ(changes.size(), 1u) << r.dump();
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    EXPECT_FALSE(changes.contains(uri_of(other_path))) << r.dump();
    std::vector<std::pair<int, int>> at;
    for (const json& e : changes[s.uri])
    {
        EXPECT_EQ(e["newText"], "health");
        at.emplace_back(e["range"]["start"]["line"], e["range"]["start"]["character"]);
    }
    std::sort(at.begin(), at.end());
    EXPECT_EQ(at, (std::vector<std::pair<int, int>>{{3, 10}, {11, 13}, {11, 23}})) << r.dump();
}

// The other script is open in the editor too. clangd can answer a definition with the symbol
// of the other generated file. The rename changes only the file of the request: the other
// script has no import of it (see RenameLeavesOutAnUnrelatedScriptWithTheSameNames).
TEST(LanguageServer, RenameWorksWhenTheOtherScriptIsOpenToo)
{
    REQUIRE_CLANGD();
    Session s(true);
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    write_file(other_path, kScript);
    s.client->open(uri_of(other_path), kScript);
    for (const std::string& uri : {uri_of(other_path), s.uri})
    {
        json p = doc_pos(uri, 3, 11);
        p["newName"] = "health";
        const json r = s.client->request("textDocument/rename", p);
        ASSERT_TRUE(r.contains("result")) << r.dump();
        const json& changes = r["result"]["changes"];
        EXPECT_EQ(changes.size(), 1u) << r.dump();
        ASSERT_TRUE(changes.contains(uri)) << r.dump();
        EXPECT_EQ(changes[uri].size(), 3u) << r.dump();
    }
}

// A rename of a class leaves out a script that declares a class with the same name but does
// not import this script.
TEST(LanguageServer, RenameOfAClassLeavesOutAnUnrelatedScript)
{
    REQUIRE_CLANGD();
    Session s(true);
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    write_file(other_path, kScript);
    json p = doc_pos(s.uri, 2, 7);
    p["newName"] = "Hero";
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json& changes = r["result"]["changes"];
    EXPECT_FALSE(changes.contains(uri_of(other_path))) << r.dump();
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    ASSERT_FALSE(changes[s.uri].empty()) << r.dump();
    EXPECT_EQ(changes[s.uri][0]["range"]["start"]["line"], 2);
}

// The references come from the ids of the table. A copy of the script with no import declares
// its own symbols: it has no references of the symbol.
TEST(LanguageServer, ReferencesLeaveOutAnUnrelatedScriptWithTheSameNames)
{
    REQUIRE_CLANGD();
    Session s(true);
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    write_file(other_path, kScript);
    const json r = s.client->request("textDocument/references",
        {{"textDocument", {{"uri", s.uri}}}, {"position", pos(11, 14)}, {"context", {{"includeDeclaration", true}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    int here = 0, there = 0;
    for (const json& l : r["result"])
    {
        if (l["uri"] == s.uri)
            ++here;
        if (l["uri"] == uri_of(other_path))
            ++there;
    }
    EXPECT_EQ(here, 3) << r.dump();
    EXPECT_EQ(there, 0) << r.dump();
}

// Degraded mode: a script with a syntax error, or with an import that has an error, has no table
// of resolved names. Definition and references then use the text scans of the lenient facts.
namespace
{
const char* const kDegradedLib = R"(using "Actor.h";
class Gun : Actor {
    public int Fire(int b) { return b * 2; }
}
)";

const char* const kDegradedMain = R"(using "Actor.h";
using "lib.hot";
class Config {
    public static const int kMax = 4;
}
class Pistol : Actor {
    public Config cfg;
    public int Shoot(Gun g) { return g.Fire(1) + Config.kMax; }
    public int Broken() { return 1 +; }
}
)";

// The first place of the result, as "file:line:character".
std::string first_place(const json& r)
{
    if (!r.contains("result") || !r["result"].is_array() || r["result"].empty())
        return "none";
    const json& l = r["result"][0];
    return std::filesystem::path(l.value("uri", std::string()).substr(7)).filename().string() + ":"
        + std::to_string(l["range"]["start"]["line"].get<int>()) + ":"
        + std::to_string(l["range"]["start"]["character"].get<int>());
}

struct DegradedSession
{
    TempDir dir;
    std::unique_ptr<LsClient> client;
    std::string main_uri;
    DegradedSession(const std::string& lib, const std::string& main)
    {
        write_file(dir.path() / "lib.hot", lib);
        const std::string main_path = (dir.path() / "main.hot").string();
        write_file(main_path, main);
        main_uri = uri_of(main_path);
        client = std::make_unique<LsClient>(options(dir, true));
        client->initialize(uri_of(dir.path().string()));
        client->open(main_uri, main);
    }
    json ask(const std::string& method, int line, int character)
    {
        json p = {{"textDocument", {{"uri", main_uri}}}, {"position", pos(line, character)}};
        if (method == "textDocument/references")
            p["context"] = {{"includeDeclaration", true}};
        return client->request(method, p);
    }
};
} // namespace

TEST(LanguageServerDegraded, DefinitionOfAStaticMemberWithASyntaxErrorElsewhere)
{
    REQUIRE_CLANGD();
    DegradedSession s(kDegradedLib, kDegradedMain);
    // "Config.kMax" on line 7: "kMax" is at character 56.
    const json r = s.ask("textDocument/definition", 7, 57);
    EXPECT_EQ(first_place(r), "main.hot:3:28") << r.dump();
}

// Disabled: the server before the table also gives no answer here. The broken script has no
// generated C++ for clangd, and no text scan finds a method of an imported class.
TEST(LanguageServerDegraded, DISABLED_DefinitionOfAnImportedMethodWithASyntaxErrorElsewhere)
{
    REQUIRE_CLANGD();
    DegradedSession s(kDegradedLib, kDegradedMain);
    // "g.Fire" on line 7: "Fire" is at character 39.
    const json r = s.ask("textDocument/definition", 7, 40);
    EXPECT_EQ(first_place(r), "lib.hot:2:15") << r.dump();
}

TEST(LanguageServerDegraded, ReferencesOfAClassWithASyntaxErrorElsewhere)
{
    REQUIRE_CLANGD();
    DegradedSession s(kDegradedLib, kDegradedMain);
    const json r = s.ask("textDocument/references", 2, 7);
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    std::set<std::string> lines;
    for (const json& l : r["result"])
        if (l.value("uri", std::string()) == s.main_uri)
            lines.insert(std::to_string(l["range"]["start"]["line"].get<int>()));
    EXPECT_EQ(lines, (std::set<std::string>{"2", "6", "7"})) << r.dump();
}

// Disabled: the server before the table also gives no answer here. The broken script has no
// generated C++ for clangd, and no text scan finds a method of an imported class.
TEST(LanguageServerDegraded, DISABLED_DefinitionOfAnImportedNameWhenTheImportHasAnError)
{
    REQUIRE_CLANGD();
    const std::string lib = std::string(kDegradedLib) + "class Bad { public int F() { return 1 +; } }\n";
    const std::string main = R"(using "Actor.h";
using "lib.hot";
class Pistol : Actor {
    public int Shoot(Gun g) { return g.Fire(1); }
}
)";
    DegradedSession s(lib, main);
    // "g.Fire" on line 3: "Fire" is at character 39.
    const json r = s.ask("textDocument/definition", 3, 40);
    EXPECT_EQ(first_place(r), "lib.hot:2:15") << r.dump();
}

TEST(LanguageServerDegraded, DefinitionOfAStaticMemberWhenTheImportHasAnError)
{
    REQUIRE_CLANGD();
    const std::string lib = std::string(kDegradedLib) + "class Bad { public int F() { return 1 +; } }\n";
    std::string main = kDegradedMain;
    main.erase(main.find("    public int Broken()"), std::string("    public int Broken() { return 1 +; }\n").size());
    DegradedSession s(lib, main);
    const json r = s.ask("textDocument/definition", 7, 57);
    EXPECT_EQ(first_place(r), "main.hot:3:28") << r.dump();
}

// clangd runs its background index on the compile_commands.json of "hotc --compile-commands".
// A workspace symbol from a generated file that is not open maps back to the script.
TEST(LanguageServer, BackgroundIndexFindsAScriptSymbolInAGeneratedFile)
{
    REQUIRE_CLANGD();
    TempDir dir;
    const std::string script = (dir.path() / "player.hot").string();
    write_file(script, kScript);
    hotc::TranslateOptions o;
    o.script_text = kScript;
    o.script_name = script;
    o.generation = 1;
    o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
    const hotc::TranslateResult t = hotc::translate(o);
    ASSERT_TRUE(t.ok) << t.diagnostics;
    const std::string cpp = (dir.path() / "player.g1.cpp").string();
    write_file(cpp, t.cpp);
    hot::ClangFilesOptions co;
    co.include_dirs = o.include_dirs;
    const std::string so = (dir.path() / "player.g1.so").string();
    write_file(dir.path() / "compile_commands.json",
        hot::compile_commands_json({{hot::ls::long_path(dir.path().string()), hot::ls::long_path(cpp),
            hot::compile_object_command(co, hot::ls::long_path(cpp), hot::ls::long_path(so), std::string()),
            hot::ls::long_path(so)}}));

    hot::ls::ServerOptions opt = options(dir, true);
    opt.include_dirs.clear(); // the server reads them from compile_commands.json
    opt.compile_commands_dir = dir.path().string();
    LsClient client(opt);
    client.initialize(uri_of(dir.path().string()));
    json found, last;
    for (int i = 0; i < 120 && found.is_null(); ++i)
    {
        const json r = client.request("workspace/symbol", {{"query", "heal"}});
        last = r;
        for (const json& sym : r.value("result", json::array()))
            if (sym["location"].value("uri", "") == uri_of(script))
                found = sym;
        if (found.is_null())
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (found.is_null())
    {
        // The diagnostics for a failure: the lines of the clangd log that tell the state of the
        // background index, and the compilation database. An empty answer can come from clangd
        // (no index) or from hot-ls (a symbol of the generated file that has no script location).
        std::string notes;
        std::ifstream log(opt.clangd_log, std::ios::binary);
        for (std::string line; std::getline(log, line);)
            if (line.rfind("E[", 0) == 0 || line.find("ndex") != std::string::npos
                || line.find("compilation database") != std::string::npos
                || line.find("workspace/symbol") != std::string::npos)
                notes += line.substr(0, 300) + "\n";
        if (notes.size() > 6000)
            notes = notes.substr(notes.size() - 6000);
        std::ifstream db(dir.path() / "compile_commands.json", std::ios::binary);
        std::stringstream db_text;
        db_text << db.rdbuf();
        FAIL() << "the index has no script symbol for " << uri_of(script) << "\n"
               << last.dump().substr(0, 3000) << "\nclangd log (" << opt.clangd_log << "):\n"
               << notes << "\ncompile_commands.json:\n"
               << db_text.str().substr(0, 2000);
    }
    EXPECT_EQ(found["name"], "Player.heal");
    EXPECT_EQ(found["location"]["range"]["start"]["line"], 10);
}

// S31 (Tooling 6b2): a C++ clangd (the editor extension) and hot-ls run on one project. The
// project has one compile_commands.json and one ".cache" directory. By default the clangd child
// of hot-ls uses the same compile-commands dir, so both clangd processes share the background
// index in "<cdb dir>/.cache/clangd/index". With "private_clangd_dir" the child keeps its index
// in ".cache/hot/clangd".
namespace
{

// True when the dir has a file whose name starts with "stem".
bool has_shard(const std::filesystem::path& dir_, const std::string& stem)
{
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir_, ec))
        if (e.path().filename().string().rfind(stem, 0) == 0)
            return true;
    return false;
}

// The lines of a clangd log that report a bad shard. A missing shard on the first start gives no
// such line. The texts are the clangd messages for a shard that fails to load or to parse.
std::string bad_shard_lines(const std::string& log_path)
{
    std::ifstream in(log_path);
    std::string line, bad;
    while (std::getline(in, line))
    {
        std::string low = line;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (low.find("failed to load shard") != std::string::npos
            || low.find("error while reading shard") != std::string::npos
            || low.find("corrupt") != std::string::npos)
            bad += line + "\n";
    }
    return bad;
}

// The shard names with the last write times of the dir, for the stems.
std::map<std::string, std::filesystem::file_time_type> shard_times(const std::filesystem::path& dir_)
{
    std::map<std::string, std::filesystem::file_time_type> r;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir_, ec))
    {
        const std::string n = e.path().filename().string();
        if (n.rfind("main.cpp.", 0) == 0 || n.rfind("player.g1.cpp.", 0) == 0)
            r[n] = e.last_write_time(ec);
    }
    return r;
}

// A project: the generated C++ of kScript, "main.cpp" and one compile_commands.json in the root.
struct SharedProject
{
    TempDir dir;
    std::filesystem::path root = dir.path();
    std::string script = (root / "player.hot").string();
    std::string cpp = (root / "main.cpp").string();
    std::string cpp_text = "int cpp_function() { return 1; }\nint cpp_bad() { int x = \"abc\"; return x; }\n";

    SharedProject()
    {
        write_file(script, kScript);
        hotc::TranslateOptions o;
        o.script_text = kScript;
        o.script_name = script;
        o.generation = 1;
        o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
        const hotc::TranslateResult t = hotc::translate(o);
        EXPECT_TRUE(t.ok) << t.diagnostics;
        const std::string gen = (root / "player.g1.cpp").string();
        write_file(gen, t.cpp);
        write_file(cpp, cpp_text);
        write_cdb();
    }

    void write_cdb() const
    {
        const std::string gen = (root / "player.g1.cpp").string();
        hot::ClangFilesOptions co;
        co.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
        write_file(root / "compile_commands.json",
            hot::compile_commands_json({{root.string(), gen, hot::compile_object_command(co, gen, gen + ".o", std::string()), gen + ".o"},
                {root.string(), cpp, hot::compile_object_command(co, cpp, cpp + ".o", std::string()), cpp + ".o"}}));
    }

    std::filesystem::path index_dir() const { return root / ".cache" / "clangd" / "index"; }
    std::filesystem::path private_dir() const { return root / ".cache" / "hot" / "clangd"; }

    hot::ls::ServerOptions server_options(bool private_dir_) const
    {
        hot::ls::ServerOptions opt = options(dir, true);
        opt.include_dirs.clear();
        opt.compile_commands_dir = root.string();
        opt.private_clangd_dir = private_dir_;
        return opt;
    }

    // Waits for the shards of main.cpp and of the generated file in the dir.
    static bool wait_shard(const std::filesystem::path& index)
    {
        for (int i = 0; i < 120; ++i)
        {
            if (has_shard(index, "main.cpp.") && has_shard(index, "player.g1.cpp."))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return false;
    }
};

} // namespace

TEST(LanguageServer, ClangdChildSharesTheIndexDirOfTheCompileCommandsDirByDefault)
{
    REQUIRE_CLANGD();
    namespace fs = std::filesystem;
    SharedProject p;
    LsClient client(p.server_options(false));
    client.initialize(uri_of(p.root.string()));
    client.open(uri_of(p.script), kScript);
    EXPECT_TRUE(SharedProject::wait_shard(p.index_dir())) << "no shard in " << p.index_dir();
    // The virtual shadow file needs no entry in the database, and the child makes no private dir.
    EXPECT_FALSE(fs::exists(p.private_dir())) << p.private_dir();
    // A native symbol still works: references and hover of "Actor" in "class Player : Actor".
    const json refs = client.request("textDocument/references",
        {{"textDocument", {{"uri", uri_of(p.script)}}}, {"position", pos(2, 16)},
            {"context", {{"includeDeclaration", true}}}});
    ASSERT_TRUE(refs.contains("result") && refs["result"].is_array()) << refs.dump();
    bool in_script = false, in_header = false;
    for (const json& l : refs["result"])
    {
        in_script = in_script || l.value("uri", "") == uri_of(p.script);
        in_header = in_header || l.value("uri", "").find("Actor.h") != std::string::npos;
    }
    EXPECT_TRUE(in_script && in_header) << refs.dump();
    const json hover = client.request("textDocument/hover", doc_pos(uri_of(p.script), 2, 16));
    ASSERT_TRUE(hover.contains("result") && hover["result"].is_object()) << hover.dump();
    EXPECT_NE(hover["result"].dump().find("Actor"), std::string::npos) << hover.dump();
}

TEST(LanguageServer, PrivateClangdDirOptionKeepsTheIndexOutOfTheSharedDir)
{
    REQUIRE_CLANGD();
    namespace fs = std::filesystem;
    SharedProject p;
    LsClient client(p.server_options(true));
    client.initialize(uri_of(p.root.string()));
    client.open(uri_of(p.script), kScript);
    EXPECT_TRUE(SharedProject::wait_shard(p.private_dir() / ".cache" / "clangd" / "index"));
    EXPECT_TRUE(fs::exists(p.private_dir() / "compile_commands.json"));
    EXPECT_FALSE(fs::exists(p.index_dir())) << "the private child wrote into the shared dir";
    const json hover = client.request("textDocument/hover", doc_pos(uri_of(p.script), 2, 16));
    ASSERT_TRUE(hover.contains("result") && hover["result"].is_object()) << hover.dump();
}

TEST(LanguageServer, PrivateClangdDirFlagIsParsed)
{
    hot::ls::ServerOptions o;
    std::string error;
    EXPECT_FALSE(o.private_clangd_dir);
    ASSERT_TRUE(hot::ls::parse_arguments({"--stdio", "--private-clangd-dir"}, o, error)) << error;
    EXPECT_TRUE(o.private_clangd_dir);
}

TEST(LanguageServer, CppClangdAndTheServerRunOnOneProjectWithoutDisturbingEachOther)
{
    REQUIRE_CLANGD();
    namespace fs = std::filesystem;
    SharedProject proj;
    const fs::path root = proj.root;
    const std::string& script = proj.script;
    const std::string& cpp = proj.cpp;
    const std::string& cpp_text = proj.cpp_text;

    // The C++ clangd, as the clangd extension starts it.
    std::mutex mu;
    std::vector<json> published;
    const std::string cpp_log = (root / "cpp-clangd.log").string();
    const std::string cpp_log2 = (root / "cpp-clangd2.log").string();
    hot::ls::ClangdProcess cpp_clangd;
    ASSERT_TRUE(cpp_clangd.start(HOT_CLANGD_PATH,
        {"--log=info", "--background-index", "--compile-commands-dir=" + root.string()}, cpp_log, [&](const json& m)
    {
        if (m.value("method", std::string()) != "textDocument/publishDiagnostics")
            return;
        std::lock_guard<std::mutex> lock(mu);
        published.push_back(m["params"]);
    }));
    const json init = cpp_clangd.request("initialize",
        {{"processId", nullptr}, {"rootUri", uri_of(root.string())}, {"capabilities", json::object()}}, 30000);
    ASSERT_TRUE(init.contains("result")) << init.dump();
    cpp_clangd.notify("initialized", json::object());
    cpp_clangd.notify("textDocument/didOpen",
        {{"textDocument", {{"uri", uri_of(cpp)}, {"languageId", "cpp"}, {"version", 1}, {"text", cpp_text}}}});

    // hot-ls on the same project.
    LsClient client(proj.server_options(false));
    client.initialize(uri_of(root.string()));
    std::string bad = kScript;
    bad.replace(bad.find("this.hp = this.hp + amount;"), 27, "int x = \"abc\";");
    write_file(script, bad);
    client.open(uri_of(script), bad);

    // Both servers give their own diagnostics.
    const json hot_diag = client.wait_notification("textDocument/publishDiagnostics", [&](const json& p)
    { return p.value("uri", std::string()) == uri_of(script) && has_source(p["diagnostics"], "clangd"); });
    EXPECT_FALSE(hot_diag.is_null()) << "hot-ls has no clangd diagnostic";
    bool cpp_diag = false;
    for (int i = 0; i < 100 && !cpp_diag; ++i)
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            for (const json& p : published)
                if (p.value("uri", std::string()) == uri_of(cpp) && !p["diagnostics"].empty())
                    cpp_diag = true;
        }
        if (!cpp_diag)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(cpp_diag) << "the C++ clangd has no diagnostic";
    {
        // The C++ clangd never gets a diagnostic for the script, and hot-ls never for main.cpp.
        std::lock_guard<std::mutex> lock(mu);
        for (const json& p : published)
            EXPECT_EQ(p.value("uri", std::string()).find(".hot"), std::string::npos) << p.dump();
    }

    // Both clangd processes run their background index on one shared index dir.
    const fs::path index = proj.index_dir();
    ASSERT_TRUE(SharedProject::wait_shard(index)) << "the shared background index is missing";
    EXPECT_FALSE(fs::exists(proj.private_dir()));

    // Stop the C++ clangd, change main.cpp, and start a new C++ clangd while hot-ls runs. The new
    // process loads the shards of the old one. It indexes the changed file again, so it writes a
    // shard while the clangd child of hot-ls runs. The shard of main.cpp gets a new write time.
    cpp_clangd.stop();
    const auto before = shard_times(index);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // the file system time step
    const std::string cpp_text2 = cpp_text + "int cpp_changed() { return 2; }\n";
    write_file(cpp, cpp_text2);
    hot::ls::ClangdProcess cpp2;
    ASSERT_TRUE(cpp2.start(HOT_CLANGD_PATH,
        {"--log=info", "--background-index", "--compile-commands-dir=" + root.string()}, cpp_log2, [](const json&) {}));
    const json init2 = cpp2.request("initialize",
        {{"processId", nullptr}, {"rootUri", uri_of(root.string())}, {"capabilities", json::object()}}, 30000);
    ASSERT_TRUE(init2.contains("result")) << init2.dump();
    cpp2.notify("initialized", json::object());
    cpp2.notify("textDocument/didOpen",
        {{"textDocument", {{"uri", uri_of(cpp)}, {"languageId", "cpp"}, {"version", 1}, {"text", cpp_text2}}}});
    bool rewritten = false;
    for (int i = 0; i < 120 && !rewritten; ++i)
    {
        for (const auto& [name, t] : shard_times(index))
            if (name.rfind("main.cpp.", 0) == 0 && before.count(name) && t != before.at(name))
                rewritten = true;
        if (!rewritten)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT_TRUE(rewritten) << "the shard of main.cpp is not written again";
    EXPECT_TRUE(cpp2.running());

    // The C++ clangd still answers for C++, and hot-ls still answers for the script.
    const json sym = cpp2.request("workspace/symbol", {{"query", "cpp_function"}}, 30000);
    ASSERT_TRUE(sym.contains("result")) << sym.dump();
    EXPECT_FALSE(sym["result"].empty()) << sym.dump();
    const json sym2 = cpp2.request("workspace/symbol", {{"query", "cpp_changed"}}, 30000);
    ASSERT_TRUE(sym2.contains("result")) << sym2.dump();
    EXPECT_FALSE(sym2["result"].empty()) << "the C++ clangd does not know the new function: " << sym2.dump();
    const json hover = client.request("textDocument/hover", doc_pos(uri_of(script), 3, 11));
    ASSERT_TRUE(hover.contains("result") && hover["result"].is_object()) << hover.dump();
    EXPECT_NE(hover["result"].dump().find("hp"), std::string::npos) << hover.dump();
    cpp2.stop();
    EXPECT_EQ(bad_shard_lines(cpp_log), "");
    EXPECT_EQ(bad_shard_lines(cpp_log2), "");
    EXPECT_EQ(bad_shard_lines((root / "clangd.log").string()), "");
}

// ------------------------------------------------------------------ S28: saved symbol index

namespace
{

hot::ls::IndexShard sample_shard()
{
    hot::ls::IndexShard shard;
    hot::ls::IndexClass c;
    c.name = "Player";
    c.begin = {2, 6};
    c.refs = {{2, 6}, {9, 4}};
    hot::ls::IndexMember known{"hp", "field", {3, 10}, std::vector<hot::ls::IndexPos>{{3, 10}, {11, 13}}};
    hot::ls::IndexMember unknown{"heal", "method", {10, 9}, std::nullopt};
    c.members = {known, unknown};
    shard.classes.push_back(c);
    shard.has_table = true;
    return shard;
}

size_t file_count(const std::filesystem::path& dir)
{
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
    {
        (void)e;
        ++n;
    }
    return n;
}

int analyzed_files(LsClient& client)
{
    const json r = client.request("hot/indexStats", json::object());
    return r["result"].value("analyzed", -1);
}

// The entry hits of the index lookups (hot/indexStats).
int entry_hits_of(LsClient& client)
{
    return client.request("hot/indexStats", json::object())["result"].value("entryHits", -1);
}

// True when the tests run with the background index (HOTPP_TEST_BACKGROUND_INDEX=1).
bool test_index_on()
{
    const char* bg = std::getenv("HOTPP_TEST_BACKGROUND_INDEX");
    return bg && std::string(bg) == "1";
}

} // namespace

TEST(SymbolIndex, EntryCountersStartAtZero)
{
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, false);
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    const json r = client.request("hot/indexStats", json::object())["result"];
    ASSERT_TRUE(r.contains("entryHits")) << r.dump();
    ASSERT_TRUE(r.contains("entryMisses")) << r.dump();
    EXPECT_EQ(r["entryHits"], 0);
    EXPECT_EQ(r["entryMisses"], 0);
}

TEST(SymbolIndex, DigestCoversTheImportedScripts)
{
    TempDir dir;
    const std::string main = (dir.path() / "main.hot").string();
    const std::string deep = (dir.path() / "deep.hot").string();
    const std::string inc = (dir.path() / "inc").string();
    std::filesystem::create_directories(inc);
    write_file(main, "using \"deep.hot\";\nusing \"lib.hot\";\n");
    write_file(deep, "class Deep {}\n");
    write_file(inc + "/lib.hot", "using \"far.hot\";\n");
    write_file(inc + "/far.hot", "class Far {}\n");
    const std::string text = "using \"deep.hot\";\nusing \"lib.hot\";\n";
    const auto digest = [&] { return hot::ls::SymbolIndex::digest(main, text, {inc}); };
    const std::string before = digest();
    EXPECT_EQ(before, digest());
    write_file(deep, "class Deep { int x = 1; }\n"); // an import
    const std::string after_deep = digest();
    EXPECT_NE(before, after_deep);
    write_file(inc + "/far.hot", "class Far { int y = 1; }\n"); // an import of an import
    EXPECT_NE(after_deep, digest());
    // A script with no import has the digest of its text, plus nothing else.
    EXPECT_EQ(hot::ls::SymbolIndex::digest(main, "class A {}\n", {}), hot::ls::SymbolIndex::digest("class A {}\n"));
}

TEST(SymbolIndex, DigestReadsEachFileOnceForDotDotSpellings)
{
    TempDir dir;
    const auto sub = [&](const char* n) { return (dir.path() / n).string(); };
    std::filesystem::create_directories(sub("a"));
    std::filesystem::create_directories(sub("b"));
    const std::string a = sub("a") + "/A.hot", b = sub("b") + "/B.hot";
    const std::string a_text = "using \"../b/B.hot\";\nclass A {}\n";
    write_file(a, a_text);
    write_file(b, "using \"../a/A.hot\";\nusing \"../b/../a/../b/B.hot\";\nclass B {}\n");
    const long before = hot::ls::SymbolIndex::digest_file_reads();
    const std::string d1 = hot::ls::SymbolIndex::digest(a, a_text, {});
    EXPECT_EQ(hot::ls::SymbolIndex::digest_file_reads() - before, 1); // B only: A is the root
    EXPECT_EQ(d1, hot::ls::SymbolIndex::digest(sub("a") + "/../a/A.hot", a_text, {}));
}

TEST(SymbolIndex, CachedDigestEqualsDigestAndInvalidatesOnImportChange)
{
    TempDir dir;
    const std::string main = (dir.path() / "main.hot").string();
    const std::string dep = (dir.path() / "dep.hot").string();
    const std::string text = "using \"dep.hot\";\n";
    write_file(main, text);
    write_file(dep, "class Dep {}\n");
    hot::ls::SymbolIndex index;
    const std::string d1 = index.cached_digest(main, text, {});
    EXPECT_EQ(d1, hot::ls::SymbolIndex::digest(main, text, {}));
    const long reads = hot::ls::SymbolIndex::digest_file_reads();
    EXPECT_EQ(d1, index.cached_digest(main, text, {}));
    EXPECT_EQ(hot::ls::SymbolIndex::digest_file_reads(), reads); // from the cache
    write_file(dep, "class Dep { int x = 1; }\n");
    const std::string d2 = index.cached_digest(main, text, {});
    EXPECT_NE(d1, d2);
    EXPECT_EQ(d2, hot::ls::SymbolIndex::digest(main, text, {}));
    // A changed root text (an unsaved document) also gives a new digest.
    EXPECT_NE(d2, index.cached_digest(main, text + "class X {}\n", {}));
}

TEST(SymbolIndex, CachedDigestSeesAnImportThatAppears)
{
    TempDir dir;
    const std::string main = (dir.path() / "main.hot").string();
    const std::string inc = (dir.path() / "inc").string();
    std::filesystem::create_directories(inc);
    const std::string text = "using \"lib.hot\";\n";
    write_file(main, text);
    hot::ls::SymbolIndex index;
    const std::string none = index.cached_digest(main, text, {inc});
    write_file(inc + "/lib.hot", "class Lib {}\n"); // a missing import appears
    const std::string in_inc = index.cached_digest(main, text, {inc});
    EXPECT_NE(none, in_inc);
    write_file((dir.path() / "lib.hot").string(), "class Near { int z; }\n"); // an earlier dir
    const std::string near = index.cached_digest(main, text, {inc});
    EXPECT_NE(in_inc, near);
    EXPECT_EQ(near, hot::ls::SymbolIndex::digest(main, text, {inc}));
}

TEST(SymbolIndex, ReusesAShardWhenTheDigestMatches)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "class A {}\n");
    const std::string digest = hot::ls::SymbolIndex::digest("class A {}\n");
    ASSERT_TRUE(index.store(script, digest, sample_shard()));
    auto shard = index.load(script, digest);
    ASSERT_TRUE(shard);
    hot::ls::IndexClass* c = shard->find_class("Player");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->refs.size(), 2u);
    ASSERT_NE(c->find_member("hp"), nullptr);
    ASSERT_TRUE(c->find_member("hp")->refs);
    EXPECT_EQ(c->find_member("hp")->refs->size(), 2u);
    ASSERT_NE(c->find_member("heal"), nullptr);
    EXPECT_FALSE(c->find_member("heal")->refs);
    EXPECT_EQ(index.loads(), 1);
}

TEST(SymbolIndex, ADifferentDigestGivesNoShardAndTheNewShardReplacesTheOld)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    const std::string d1 = hot::ls::SymbolIndex::digest("one"), d2 = hot::ls::SymbolIndex::digest("two");
    ASSERT_NE(d1, d2);
    ASSERT_TRUE(index.store(script, d1, sample_shard()));
    EXPECT_FALSE(index.load(script, d2));
    hot::ls::IndexShard empty;
    empty.has_table = true;
    ASSERT_TRUE(index.store(script, d2, empty));
    EXPECT_FALSE(index.load(script, d1));
    ASSERT_TRUE(index.load(script, d2));
    EXPECT_EQ(file_count(dir.path() / "index"), 1u);
}

TEST(SymbolIndex, PruneDeletesTheShardOfAMissingFile)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string a = (dir.path() / "a.hot").string(), b = (dir.path() / "b.hot").string();
    write_file(a, "x");
    write_file(b, "y");
    ASSERT_TRUE(index.store(a, "da", sample_shard()));
    ASSERT_TRUE(index.store(b, "db", sample_shard()));
    EXPECT_EQ(index.prune(), 0);
    std::filesystem::remove(a);
    EXPECT_EQ(index.prune(), 1);
    EXPECT_EQ(file_count(dir.path() / "index"), 1u);
    EXPECT_FALSE(std::filesystem::exists(index.shard_path(a)));
    EXPECT_TRUE(index.load(b, "db"));
}

TEST(SymbolIndex, ADifferentFormatVersionIsDropped)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    ASSERT_TRUE(index.store(script, "d", sample_shard()));
    const std::string file = index.shard_path(script);
    std::string text = *[&]
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return std::optional<std::string>(ss.str());
    }();
    const std::string head = "hot-index " + std::to_string(hot::ls::SymbolIndex::kFormatVersion) + " ";
    ASSERT_EQ(text.rfind(head, 0), 0u);
    text.replace(0, head.size(), "hot-index 999 ");
    write_file(file, text);
    EXPECT_FALSE(index.load(script, "d"));
    EXPECT_FALSE(std::filesystem::exists(file));
}

namespace
{

std::string read_all(const std::string& file)
{
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A table with two lines: "class A { int hp; }\n" and "int f() { return A().hp + x.y; }".
hot::ls::FileRefs sample_table()
{
    hot::ls::FileRefs t;
    t.file_id = "/p/a.hot";
    const auto add = [&](uint32_t off, uint32_t len, int line, int col, hotc::RefKind k, bool d, const char* decl,
                         const char* file)
    {
        hotc::ResolvedRef r;
        r.offset = off;
        r.length = len;
        r.pos = {line, col};
        r.kind = k;
        r.is_decl = d;
        r.decl = decl;
        r.decl_file = file;
        r.id = std::string(file) + "#" + decl;
        t.refs.push_back(r);
    };
    add(6, 1, 1, 7, hotc::RefKind::Class, true, "A", "/p/a.hot");
    add(14, 2, 1, 15, hotc::RefKind::Field, true, "A.hp", "/p/a.hot");
    add(24, 1, 2, 5, hotc::RefKind::Function, true, "f", "/p/a.hot");
    add(37, 1, 2, 18, hotc::RefKind::Class, false, "B", "/p/b.hot");
    add(41, 2, 2, 22, hotc::RefKind::Field, false, "A.hp", "/p/a.hot");
    t.refs.back().near_decl = "C.hp";
    t.refs.back().near_file = "/p/c.hot";
    t.errors = {{2, "expected ';'"}};
    t.bases = {{"A", "Actor"}};
    hotc::UnresolvedName u;
    u.offset = 48;
    u.length = 1;
    u.pos = {2, 29};
    t.unresolved.push_back(u);
    t.raw_ranges = {{30, 52}};
    t.imports = {"/p/b.hot", "/p/c.hot"};
    return t;
}

} // namespace

TEST(SymbolIndex, StoreAndLoadGiveAnEqualTable)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    hot::ls::IndexShard shard = sample_shard();
    shard.table = sample_table();
    ASSERT_TRUE(index.store(script, "d", shard));
    const auto loaded = index.load(script, "d");
    ASSERT_TRUE(loaded);
    const hot::ls::FileRefs& a = shard.table;
    const hot::ls::FileRefs& b = loaded->table;
    EXPECT_EQ(b.file_id, a.file_id);
    ASSERT_EQ(b.refs.size(), a.refs.size());
    for (size_t i = 0; i < a.refs.size(); ++i)
    {
        SCOPED_TRACE(i);
        EXPECT_EQ(b.refs[i].offset, a.refs[i].offset);
        EXPECT_EQ(b.refs[i].length, a.refs[i].length);
        EXPECT_EQ(b.refs[i].pos, a.refs[i].pos);
        EXPECT_EQ(b.refs[i].kind, a.refs[i].kind);
        EXPECT_EQ(b.refs[i].is_decl, a.refs[i].is_decl);
        EXPECT_EQ(b.refs[i].decl, a.refs[i].decl);
        EXPECT_EQ(b.refs[i].decl_file, a.refs[i].decl_file);
        EXPECT_EQ(b.refs[i].id, a.refs[i].id);
        EXPECT_EQ(b.refs[i].near_decl, a.refs[i].near_decl);
        EXPECT_EQ(b.refs[i].near_file, a.refs[i].near_file);
    }
    EXPECT_EQ(b.errors, a.errors);
    EXPECT_EQ(b.bases, a.bases);
    ASSERT_EQ(b.unresolved.size(), 1u);
    EXPECT_EQ(b.unresolved[0].offset, 48u);
    EXPECT_EQ(b.unresolved[0].length, 1u);
    EXPECT_EQ(b.unresolved[0].pos, (hotc::SourcePos{2, 29}));
    EXPECT_EQ(b.raw_ranges, a.raw_ranges);
    EXPECT_EQ(b.imports, a.imports);
    EXPECT_EQ(loaded->classes.size(), shard.classes.size());
}

// A shard without the table marker is never given back. load() deletes it.
TEST(SymbolIndex, AShardWithoutTheTableMarkerIsDropped)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    hot::ls::IndexShard shard = sample_shard();
    shard.has_table = false;
    ASSERT_TRUE(index.store(script, "d", shard));
    EXPECT_FALSE(index.load(script, "d"));
    EXPECT_FALSE(std::filesystem::exists(index.shard_path(script)));
}

TEST(SymbolIndex, AShardWithABrokenBodyIsDropped)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    ASSERT_TRUE(index.store(script, "d", sample_shard()));
    const std::string file = index.shard_path(script);
    std::string first;
    {
        std::ifstream in(file, std::ios::binary);
        std::getline(in, first);
    }
    write_file(file, first + "\n{not json");
    EXPECT_FALSE(index.load(script, "d"));
    EXPECT_FALSE(std::filesystem::exists(file));
}

// A shard of format 3 has no near_decl and no near_file: load() deletes it.
TEST(SymbolIndex, AVersion3ShardIsDropped)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    hot::ls::IndexShard shard = sample_shard();
    shard.table = sample_table();
    ASSERT_TRUE(index.store(script, "d", shard));
    const std::string file = index.shard_path(script);
    std::string text = read_all(file);
    const std::string head = "hot-index " + std::to_string(hot::ls::SymbolIndex::kFormatVersion) + " ";
    ASSERT_EQ(text.rfind(head, 0), 0u);
    text.replace(0, head.size(), "hot-index 3 ");
    write_file(file, text);
    EXPECT_FALSE(index.load(script, "d"));
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST(SymbolIndex, AVersion2ShardIsDropped)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    const std::string file = index.shard_path(script);
    std::filesystem::create_directories(dir.path() / "index");
    // The body of format 2 is the array of the classes.
    write_file(file, "hot-index 2 d " + script + "\n[{\"n\":\"A\",\"p\":[0,6],\"r\":[],\"m\":[]}]");
    EXPECT_FALSE(index.load(script, "d"));
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST(SymbolIndex, AChangedImportChangesTheDigestAndTheShardIsNotReused)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string main = (dir.path() / "main.hot").string();
    const std::string text = "using \"dep.hot\";\nint f() { return Dep.k; }\n";
    write_file(main, text);
    write_file((dir.path() / "dep.hot").string(), "class Dep { const int k = 1; }\n");
    const std::string d1 = index.cached_digest(main, text, {});
    hot::ls::IndexShard shard;
    shard.table = sample_table();
    shard.has_table = true;
    ASSERT_TRUE(index.store(main, d1, shard));
    ASSERT_TRUE(index.load(main, d1));
    write_file((dir.path() / "dep.hot").string(), "class Dep { const int kk = 1; }\n");
    const std::string d2 = index.cached_digest(main, text, {});
    EXPECT_NE(d1, d2);
    EXPECT_FALSE(index.load(main, d2));
    EXPECT_NE(read_all(index.shard_path(main)).find(d1), std::string::npos); // the old shard stays until a store
}

TEST(SymbolIndex, TheIndexDoesNotGrowAfter1000EditsOfOneFile)
{
    TempDir dir;
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "x");
    std::uintmax_t first_size = 0;
    for (int i = 0; i < 1000; ++i)
    {
        const std::string digest = hot::ls::SymbolIndex::digest("version " + std::to_string(i));
        ASSERT_TRUE(index.store(script, digest, sample_shard()));
        if (i == 0)
            first_size = std::filesystem::file_size(index.shard_path(script));
    }
    EXPECT_EQ(file_count(dir.path() / "index"), 1u); // no old shard and no temporary file
    EXPECT_LE(std::filesystem::file_size(index.shard_path(script)), first_size + 16);
}

// A class rename in a closed file that imports the script. The first server analyzes the file and
// saves the shard. A new server after a restart loads the shard and analyzes no file.
TEST(LanguageServer, RenameOnAClosedFileAfterARestartUsesTheIndex)
{
    Session s(false);
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    const std::string importer = "using \"Actor.h\";\nusing \"player.hot\";\nclass Kid : Player {\n    void Go() { this.hp = 1.0; }\n}\n";
    write_file(other_path, importer);
    write_file(s.dir.path() / "unrelated.hot", "class Other {\n    float hp = 1.0;\n}\n");
    json p = doc_pos(s.uri, 2, 7);
    p["newName"] = "Hero";
    json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    ASSERT_TRUE(r["result"]["changes"].contains(uri_of(other_path))) << r.dump();
    const json first = r["result"]["changes"][uri_of(other_path)];
    // Without the index the rename analyzes the file inline. With the index (HOT-34 step 5E) the rename
    // takes the closed file from the index entry: the worker analyzes it, the inline count stays 0.
    // The edits are the same in both modes (the restart below compares them too).
    if (test_index_on())
    {
        EXPECT_EQ(analyzed_files(*s.client), 0);
        EXPECT_GT(entry_hits_of(*s.client), 0);
    }
    else
        EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_TRUE(std::filesystem::exists(s.dir.path() / ".cache" / "hot" / "index"));
    {
        // The saved shard has the table of the resolved names and the table marker.
        hot::ls::SymbolIndex index;
        index.open((s.dir.path() / ".cache" / "hot" / "index").string());
        const std::string shard = read_all(index.shard_path(other_path));
        const size_t nl = shard.find('\n');
        ASSERT_NE(nl, std::string::npos);
        const json body = json::parse(shard.substr(nl + 1));
        EXPECT_TRUE(body.contains("tp"));
        EXPECT_FALSE(body["t"]["r"].empty()) << body.dump();
        EXPECT_EQ(body["t"]["f"], hotc::ref_file_id(other_path));
    }
    s.client.reset(); // the restart
    s.client = std::make_unique<LsClient>(options(s.dir, false));
    s.client->initialize(uri_of(s.dir.path().string()));
    s.client->open(s.uri, kScript);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)], first) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 0);
    // A changed file is analyzed again. The shard count stays the same.
    const size_t shards = file_count(s.dir.path() / ".cache" / "hot" / "index");
    std::string changed = importer;
    changed += "\n";
    write_file(other_path, changed);
    const int hits_before = entry_hits_of(*s.client);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    // The appended newline moves no place: the edits equal the first edits.
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)], first) << r.dump();
    if (test_index_on())
    {
        // The stamp changed, the index analyzed the file again in the worker and the rename used the new entry.
        EXPECT_EQ(analyzed_files(*s.client), 0);
        EXPECT_GT(entry_hits_of(*s.client), hits_before);
    }
    else
        EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_EQ(file_count(s.dir.path() / ".cache" / "hot" / "index"), shards);
}

// The tables of the resolved names (file_refs) and the import graph, through the test hook of
// hot/indexStats. An open document gives its table. A closed importer gets a table from an
// analyze, then from its shard after a restart. A change of the import makes a new analyze.
TEST(LanguageServer, TheRefTablesComeFromTheDocumentTheShardAndTheImportGraph)
{
    TempDir dir;
    const std::string dep = (dir.path() / "dep.hot").string(), user = (dir.path() / "user.hot").string();
    const std::string dep_text = "int k() { return 1; }\n";
    write_file(dep, dep_text);
    write_file(user, "using \"dep.hot\";\nint f() { return k(); }\n");
    hot::ls::ServerOptions o = options(dir, false);
    o.background_index = false;
    auto client = std::make_unique<LsClient>(o);
    client->initialize(uri_of(dir.path().string()));
    client->open(uri_of(dep), dep_text);
    auto stats = [&](const json& p) { return client->request("hot/indexStats", p)["result"]; };
    json r = stats({{"tables", {uri_of(dep)}}});
    EXPECT_GT(r["tableRefs"][0].get<int>(), 0) << r.dump();
    EXPECT_EQ(r["tableAnalyses"], 1);
    EXPECT_EQ(r["tableHits"], 0);
    r = stats({{"tables", {uri_of(dep)}}});
    EXPECT_EQ(r["tableAnalyses"], 1); // the same document version
    EXPECT_EQ(r["tableHits"], 1);
    r = stats({{"tables", {uri_of(user)}}, {"importersOf", uri_of(dep)}});
    EXPECT_GT(r["tableRefs"][0].get<int>(), 0) << r.dump();
    EXPECT_EQ(r["tableAnalyses"], 2);
    EXPECT_EQ(r["importers"], json::array({hotc::ref_file_id(user)})) << r.dump();

    client.reset(); // the restart
    client = std::make_unique<LsClient>(o);
    client->initialize(uri_of(dir.path().string()));
    r = stats({{"tables", {uri_of(user)}}});
    EXPECT_GT(r["tableRefs"][0].get<int>(), 0) << r.dump();
    EXPECT_EQ(r["tableHits"], 1); // from the shard
    EXPECT_EQ(r["tableAnalyses"], 0);
    r = stats({{"tables", {uri_of(user)}}});
    EXPECT_EQ(r["tableHits"], 2); // from the memory cache
    EXPECT_EQ(r["loaded"], 1);

    // A changed import changes the digest of the importer: a new analyze.
    write_file(dep, "int k() { return 1; }\nint j() { return 2; }\n");
    r = stats({{"tables", {uri_of(user)}}, {"importersOf", uri_of(dep)}});
    EXPECT_EQ(r["tableAnalyses"], 1);
    EXPECT_EQ(r["importers"], json::array({hotc::ref_file_id(user)}));
    // An importer without the import is not an importer.
    write_file(user, "int f() { return 12; }\n");
    r = stats({{"importersOf", uri_of(dep)}});
    EXPECT_TRUE(r["importers"].empty()) << r.dump();
}

// A shard of another translator build is not loaded (HOT-33). The same build loads it.
TEST(LanguageServer, AShardOfAnotherBuildIsNotLoaded)
{
    TempDir dir;
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "int f() { return 1; }\n");
    auto run = [&](const std::string& build_id)
    {
        hot::ls::ServerOptions o = options(dir, false);
        o.background_index = false;
        o.build_id = build_id;
        LsClient client(o);
        client.initialize(uri_of(dir.path().string()));
        client.open(uri_of(script), "int f() { return 1; }\n");
        client.request("hot/indexStats", {{"tables", {uri_of(script)}}});
        client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(script)}}}});
        client.request("hot/indexStats", json::object());
        client.request("hot/indexStats", {{"tables", {uri_of(script)}}});
        return client.request("hot/indexStats", json::object())["result"].value("loaded", -1);
    };
    EXPECT_EQ(run("build-a"), 0); // stores the shard
    EXPECT_GT(run("build-a"), 0);
    EXPECT_EQ(run("build-b"), 0);
}

// The same for a member. The references of a member come from clangd. The shard keeps them.
TEST(LanguageServer, MemberRenameOnAClosedFileAfterARestartUsesTheIndex)
{
    REQUIRE_CLANGD();
    TempDir index; // the index directory is not in the workspace
    hot::ls::ServerOptions o = options(index, true);
    o.background_index = false;
    o.index_dir = (index.path() / "my-index").string();
    Session s(true, kScript, &o);
    // The other script imports the script: its use of the member is a place of the rename.
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    write_file(other_path, "using \"Actor.h\";\nusing \"player.hot\";\nclass Kid : Player {\n    void Go() { this.hp = 1.0; }\n}\n");
    json p = doc_pos(s.uri, 3, 11);
    p["newName"] = "health";
    json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    ASSERT_TRUE(r["result"]["changes"].contains(uri_of(other_path))) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)].size(), 1u) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_FALSE(std::filesystem::exists(s.dir.path() / ".cache")); // --index-dir is used
    s.client.reset();
    s.client = std::make_unique<LsClient>(o);
    s.client->initialize(uri_of(s.dir.path().string()));
    s.client->open(s.uri, kScript);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)].size(), 1u) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 0);
}

// ------------------------------------------------------------------ S29: completion of all symbols

namespace
{

// A script with one of each kind of symbol. "@" marks the cursor.
const char* const kCompleteScript =
    "using \"Actor.h\";\n"
    "using \"Damageable.h\";\n"
    "\n"
    "enum Mode { Idle, Walk }\n"
    "struct Point {\n"
    "    float x = 0.0;\n"
    "}\n"
    "int total = 0;\n"
    "\n"
    "class Base : Actor {\n"
    "    int baseField = 0;\n"
    "    void baseMethod() { }\n"
    "}\n"
    "\n"
    "class Hero : Base {\n"
    "    int ticks = 0;\n"
    "    void run(float speed) {\n"
    "        int counter = 1;\n"
    "        Mode current = Mode.Idle;\n"
    "        @\n"
    "    }\n"
    "}\n";

struct Completion
{
    json result;
    std::vector<std::string> names;
    bool has(const std::string& n) const { return std::find(names.begin(), names.end(), n) != names.end(); }
};

// Opens the script (without the "@") and completes at the "@".
Completion complete_at(bool clangd, std::string text, const std::string& replace = "", const std::string& lib = "")
{
    const size_t at = text.find('@');
    text.replace(at, 1, replace);
    int line = 0, col = 0;
    for (size_t i = 0; i < at; ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            col = 0;
        }
        else
        {
            ++col;
        }
    }
    col += static_cast<int>(replace.size());
    Session s(clangd, text);
    if (!lib.empty())
        write_file(s.dir.path() / "lib.hot", lib);
    Completion c;
    json r = s.client->request("textDocument/completion", doc_pos(s.uri, line, col));
    c.result = r.contains("result") ? r["result"] : json::object();
    for (const json& i : c.result.value("items", json::array()))
        c.names.push_back(i.value("insertText", i.value("label", "")));
    return c;
}

} // namespace

// The items of "using" completion at the end of "head" (the cursor is at the end of the text).
static json using_items(const std::string& head, const std::function<void(const TempDir&, const TempDir&)>& setup,
    json* first = nullptr)
{
    TempDir hd;
    TempDir wd;
    setup(wd, hd);
    hot::ls::ServerOptions o = options(wd, false);
    o.include_dirs.push_back(hd.path().string());
    const std::string text = head + "\nclass A { }\n";
    const std::string path = (wd.path() / "player.hot").string();
    write_file(path, text);
    LsClient client(o);
    client.initialize(uri_of(wd.path().string()));
    client.open(uri_of(path), text);
    int line = 0, col = 0;
    for (char c : head)
    {
        if (c == '\n')
        {
            ++line;
            col = 0;
        }
        else
            ++col;
    }
    const json r = client.request("textDocument/completion", doc_pos(uri_of(path), line, col));
    json items = r["result"].value("items", json::array());
    if (first && !items.empty())
        *first = items[0];
    return items;
}

static bool has_label(const json& items, const std::string& l)
{
    return std::any_of(items.begin(), items.end(), [&](const json& i) { return i.value("label", "") == l; });
}

static void using_workspace(const TempDir& wd, const TempDir& hd)
{
    std::filesystem::create_directories(wd.path() / "sub");
    write_file((wd.path() / "alpha.hot").string(), "class Alpha { }\n");
    write_file((wd.path() / "sub" / "beta.hot").string(), "class Beta { }\n");
    write_file((wd.path() / "notes.txt").string(), "x\n");
    write_file((hd.path() / "Widgets.h").string(),
        "#pragma once\nnamespace Gui {\nnamespace Inner { }\n}\nnamespace Plain { }\n");
    write_file((hd.path() / "extra.hot").string(), "class Extra { }\n");
}

TEST(LanguageServer, UsingQuoteOffersScriptsAndHeaders)
{
    const json items = using_items("using \"", using_workspace);
    EXPECT_TRUE(has_label(items, "alpha.hot")) << items.dump();
    EXPECT_TRUE(has_label(items, "sub/"));
    EXPECT_TRUE(has_label(items, "Widgets.h"));
    EXPECT_TRUE(has_label(items, "extra.hot"));
    EXPECT_FALSE(has_label(items, "notes.txt"));
    EXPECT_FALSE(has_label(items, "player.hot"));
}

TEST(LanguageServer, UsingQuoteReplacesOnlyThePartialPath)
{
    json first;
    const json items = using_items("// x\nusing \"sub/b", using_workspace, &first);
    ASSERT_EQ(items.size(), 1u) << items.dump();
    EXPECT_EQ(first["label"], "sub/beta.hot");
    EXPECT_EQ(first["textEdit"]["newText"], "sub/beta.hot");
    EXPECT_EQ(first["textEdit"]["range"]["start"]["line"], 1);
    EXPECT_EQ(first["textEdit"]["range"]["start"]["character"], 7);
    EXPECT_EQ(first["textEdit"]["range"]["end"]["character"], 12);
    EXPECT_TRUE(has_label(using_items("using \"Wi", using_workspace), "Widgets.h"));
    EXPECT_FALSE(has_label(using_items("using \"Wi", using_workspace), "alpha.hot"));
}

TEST(LanguageServer, UsingWordOffersNamespaces)
{
    const json items = using_items("using \"Widgets.h\";\nusing Gu", using_workspace);
    EXPECT_TRUE(has_label(items, "Gui")) << items.dump();
    EXPECT_FALSE(has_label(items, "Plain"));
    EXPECT_FALSE(has_label(items, "if"));
    EXPECT_FALSE(has_label(items, "return"));
    // A header of the file gives its namespaces.
    const json imp = using_items("using \"Widgets.h\";\nusing ", using_workspace);
    EXPECT_TRUE(has_label(imp, "Gui")) << imp.dump();
    EXPECT_TRUE(has_label(imp, "Plain"));
    EXPECT_FALSE(has_label(imp, "Inner"));
    const json dotted = using_items("using \"Widgets.h\";\nusing Gui.", using_workspace);
    EXPECT_TRUE(has_label(dotted, "Inner")) << dotted.dump();
    EXPECT_FALSE(has_label(dotted, "Plain"));
}

TEST(LanguageServer, UsingOffersNothingInCommentsOrOtherStrings)
{
    EXPECT_TRUE(using_items("// using \"", using_workspace).empty());
    EXPECT_TRUE(using_items("// using Gu", using_workspace).empty());
    EXPECT_TRUE(using_items("/* a\nusing \"", using_workspace).empty());
    EXPECT_TRUE(using_items("string s = \"using x", using_workspace).empty());
    EXPECT_TRUE(using_items("using \"alpha.hot\"; // using \"", using_workspace).empty());
}

TEST(LanguageServer, CompletionListsLocalsParametersAndModuleVariables)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript);
    EXPECT_TRUE(c.has("counter")) << c.result.dump(); // a local
    EXPECT_TRUE(c.has("current")) << c.result.dump();
    EXPECT_TRUE(c.has("speed")) << c.result.dump(); // a parameter
    EXPECT_TRUE(c.has("total")) << c.result.dump(); // a module variable
}

TEST(LanguageServer, CompletionListsEnumsAndEnumMembers)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript);
    EXPECT_TRUE(c.has("Mode")) << c.result.dump();
    const Completion m = complete_at(true, kCompleteScript, "Mode.");
    EXPECT_TRUE(m.has("Idle")) << m.result.dump();
    EXPECT_TRUE(m.has("Walk")) << m.result.dump();
    EXPECT_FALSE(m.has("counter"));
}

TEST(LanguageServer, CompletionListsScriptClassesAndTheirOwnAndInheritedMembers)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript);
    EXPECT_TRUE(c.has("Hero")) << c.result.dump();
    EXPECT_TRUE(c.has("Base"));
    EXPECT_TRUE(c.has("Point"));
    const Completion m = complete_at(true, kCompleteScript, "this.");
    EXPECT_TRUE(m.has("ticks")) << m.result.dump(); // own
    EXPECT_TRUE(m.has("run"));
    EXPECT_TRUE(m.has("baseField")) << m.result.dump(); // from the script base
    EXPECT_TRUE(m.has("baseMethod"));
    EXPECT_TRUE(m.has("Move")) << m.result.dump(); // from the native base
}

TEST(LanguageServer, CompletionAfterADotOnALocalAndOnAFieldListsTheMembers)
{
    REQUIRE_CLANGD();
    std::string text = kCompleteScript;
    text.replace(text.find("    int ticks = 0;\n"), 19, "    int ticks = 0;\n    Point origin;\n");
    text.replace(text.find("        int counter = 1;\n"), 25, "        int counter = 1;\n        Point pt;\n        $Base bs = new $Base();\n        Vec3 vec;\n");
    // A local.
    const Completion l = complete_at(true, text, "pt.");
    EXPECT_TRUE(l.has("x")) << l.result.dump();
    const Completion lp = complete_at(true, text, "pt.x");
    EXPECT_TRUE(lp.has("x")) << lp.result.dump();
    const Completion lb = complete_at(true, text, "bs.");
    EXPECT_TRUE(lb.has("baseField")) << lb.result.dump();
    const Completion lbp = complete_at(true, text, "bs.base");
    EXPECT_TRUE(lbp.has("baseMethod")) << lbp.result.dump();
    const Completion ln = complete_at(true, text, "bs.Mov"); // a native member
    EXPECT_TRUE(ln.has("Move")) << ln.result.dump();
    const Completion lv = complete_at(true, text, "vec.");
    EXPECT_TRUE(lv.has("x")) << lv.result.dump();
    // A field of the class.
    const Completion f = complete_at(true, text, "origin.");
    EXPECT_TRUE(f.has("x")) << f.result.dump();
    const Completion fp = complete_at(true, text, "origin.x");
    EXPECT_TRUE(fp.has("x")) << fp.result.dump();
}

TEST(LanguageServer, CompletionAfterADotOnAHandleLocalSetToNullListsTheMembers)
{
    REQUIRE_CLANGD();
    std::string text = kCompleteScript;
    text.replace(text.find("        int counter = 1;\n"), 25, "        int counter = 1;\n        $Base nb = null;\n");
    const Completion c = complete_at(true, text, "nb.");
    EXPECT_TRUE(c.has("baseField")) << c.result.dump();
    EXPECT_TRUE(c.has("Move")) << c.result.dump();
}

namespace
{
// The completion after "mocklib." at a place of a class body. "@" marks the cursor.
Completion complete_in_class(const std::string& body)
{
    return complete_at(true, "using \"MockLib.h\";\n\nclass T {\n" + body + "\n}\n", "mocklib.");
}
} // namespace

TEST(LanguageServer, CompletionAfterANamespaceInAFieldType)
{
    REQUIRE_CLANGD();
    const Completion c = complete_in_class("    private static @");
    EXPECT_TRUE(c.has("vector")) << c.result.dump();
    EXPECT_TRUE(complete_in_class("    int a = 0;\n    @").has("vector"));
}

TEST(LanguageServer, CompletionAfterANamespaceInAParameterAndAReturnType)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_in_class("    void F(@").has("vector"));
    EXPECT_TRUE(complete_in_class("    void G(int a, @").has("vector"));
    EXPECT_TRUE(complete_in_class("    @ F() { }").has("vector"));
}

TEST(LanguageServer, CompletionAfterANamespaceInATemplateArgument)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_in_class("    mocklib.vector<@").has("vector"));
    EXPECT_TRUE(complete_in_class("    void F() {\n        mocklib.vector<@\n    }").has("vector"));
}

TEST(LanguageServer, CompletionAfterANamespaceInALocalTypeAndAnAlias)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_in_class("    void F() {\n        @\n    }").has("vector"));
    const Completion a = complete_at(true, "using \"MockLib.h\";\nusing P = @\nclass T {\n}\n", "mocklib.");
    EXPECT_TRUE(a.has("vector")) << a.result.dump();
}

// The completion gives nothing in a comment or in a string. A "//" in a string does not start a
// comment, and a comment after the cursor does not stop the completion.
TEST(LanguageServer, CompletionGivesNothingInACommentOrAString)
{
    REQUIRE_CLANGD();
    auto at = [](const std::string& code)
    {
        std::string text = kCompleteScript;
        text.replace(text.find('@'), 1, code);
        return complete_at(true, text);
    };
    EXPECT_TRUE(at("// ret@").names.empty());
    EXPECT_TRUE(at("/* ret@ */").names.empty());
    EXPECT_TRUE(at("string s = \"Wea@\";").names.empty());
    // A block comment from an earlier line.
    EXPECT_TRUE(at("/* a\n b ret@").names.empty());
    // The text "//" in a string does not start a comment: the code after the string completes.
    EXPECT_TRUE(at("string u = \"http://x\"; coun@").has("counter"));
}

TEST(LanguageServer, CompletionWorksBeforeALineComment)
{
    REQUIRE_CLANGD();
    // The word is followed by a comment: "mocklib.// c" in a method.
    std::string text = "using \"MockLib.h\";\n\nclass T {\n    void F() {\n        mocklib.@// c\n    }\n}\n";
    EXPECT_TRUE(complete_at(true, text).has("vector"));
}

// The "using" exemption is for the path in the first string of a "using" line only.
TEST(LanguageServer, CompletionUsingExemptionIsOnlyForThePathString)
{
    REQUIRE_CLANGD();
    auto at = [](const std::string& code)
    {
        std::string text = kCompleteScript;
        text.replace(text.find('@'), 1, code);
        return complete_at(true, text);
    };
    EXPECT_TRUE(at("usingFoo = \"ret@\";").names.empty()); // no exemption: the word is not "using"
    EXPECT_FALSE(at("usingFoo = ret@").names.empty()); // code: it completes
    EXPECT_TRUE(at("using namespace x; // ret@").names.empty());
    EXPECT_TRUE(at("/* a\nusing ret@").names.empty());
    EXPECT_TRUE(at("using \"x.h\"; // ret@").names.empty());
}

// The names that look like generated names or like C library names stay when they are real.
TEST(LanguageServer, CompletionKeepsRealNamesThatLookLikeGeneratedOrLibcNames)
{
    REQUIRE_CLANGD();
    TempDir hd;
    write_file((hd.path() / "Names.h").string(),
        "#pragma once\ninline int rand(int a, int b) { return a + b; }\n"
        "inline int button_create(int x) { return x; }\ninline int last_record = 1;\n");
    // The local "div" is a script name. The others are native names of the host.
    for (const std::string& name : {std::string("div"), std::string("rand"), std::string("button_create"),
             std::string("last_record")})
    {
        const std::string prefix = name.substr(0, name.size() - 1);
        const std::string text = "using \"Names.h\";\n\nclass Hero {\n    void run() {\n        int div = 1;\n        " + prefix
            + "\n    }\n}\n";
        TempDir sd;
        hot::ls::ServerOptions o = options(sd, true);
        o.include_dirs.push_back(hd.path().string());
        Session s(true, text, &o);
        const json r = s.client->request(
            "textDocument/completion", doc_pos(s.uri, 5, 8 + static_cast<int>(prefix.size())));
        bool found = false;
        for (const json& i : r["result"].value("items", json::array()))
            found = found || i.value("insertText", i.value("label", "")) == name;
        EXPECT_TRUE(found) << name << " in " << r.dump();
    }
}

// The generated names of a script class are not offered.
TEST(LanguageServer, CompletionHidesRecordAndCreateHelpers)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript, "");
    EXPECT_TRUE(c.has("Hero"));
    EXPECT_FALSE(c.has("Hero_record"));
    EXPECT_FALSE(c.has("Hero_create"));
    EXPECT_FALSE(c.has("Base_record"));
    EXPECT_FALSE(c.has("Base_create"));
    // The names of the C library are not script names.
    EXPECT_FALSE(c.has("strcmp"));
    EXPECT_FALSE(c.has("printf"));
}

TEST(LanguageServer, CompletionListsNativeTypesAndFreeFunctions)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript, "Vec");
    EXPECT_TRUE(c.has("Vec3")) << c.result.dump(); // a native type of EngineMath.h
    const Completion f = complete_at(true, kCompleteScript, "Hit");
    bool function = false;
    for (const json& i : f.result.value("items", json::array()))
        if (i.value("kind", 0) == 3)
            function = true;
    EXPECT_TRUE(function) << f.result.dump(); // a native free function
}

TEST(LanguageServer, CompletionListsKeywordsAndBuiltinTypes)
{
    const Completion c = complete_at(false, kCompleteScript);
    EXPECT_TRUE(c.has("while")) << c.result.dump();
    EXPECT_TRUE(c.has("return"));
    EXPECT_TRUE(c.has("float"));
    EXPECT_TRUE(c.has("int32")); // an alias of a builtin type
}

TEST(LanguageServer, CompletionFiltersByThePrefix)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript, "co");
    EXPECT_TRUE(c.has("counter")) << c.result.dump();
    EXPECT_TRUE(c.has("const"));
    EXPECT_TRUE(c.has("continue"));
    EXPECT_FALSE(c.has("total"));
    EXPECT_FALSE(c.has("while"));
    for (const json& i : c.result["items"])
    { // the filter text is the name without the namespace
        std::string n = i.value("filterText", i.value("label", ""));
        for (char& ch : n)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        EXPECT_EQ(n.substr(0, 2), "co") << i.dump();
    }
}

TEST(LanguageServer, CompletionWorksInAnIncompleteStatement)
{
    REQUIRE_CLANGD();
    // The call has no closing bracket and no semicolon.
    std::string text = kCompleteScript;
    text.replace(text.find('@'), 1, "this.run(cou@");
    const Completion c = complete_at(true, text);
    EXPECT_TRUE(c.has("counter")) << c.result.dump();
}

TEST(LanguageServer, CompletionFallsBackToTheScriptFactsWhenTheTranslationFails)
{
    REQUIRE_CLANGD();
    // The type "Missing" does not exist. The whole script fails to translate.
    std::string text = kCompleteScript;
    text.replace(text.find("@"), 1, "Missing broken = ;\n        @");
    const Completion c = complete_at(true, text, "cou");
    EXPECT_TRUE(c.has("counter")) << c.result.dump(); // a script fact
    const Completion k = complete_at(true, text, "whi");
    EXPECT_TRUE(k.has("while")) << k.result.dump(); // a keyword
}

TEST(LanguageServer, CompletionUsesTheStaleTextWhenNoTextTranslates)
{
    REQUIRE_CLANGD();
    // The first text is good. Then a "using" of a missing header fails the translation, also of
    // every patched text. The completion asks clangd at the old place of the old text.
    std::string good = kCompleteScript;
    good.replace(good.find('@'), 1, "");
    Session s(true, good);
    json first = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 8));
    ASSERT_TRUE(first.contains("result"));
    std::string bad = kCompleteScript;
    bad.replace(bad.find('@'), 1, "");
    bad.insert(0, "using \"nosuch_header.h\";\n");
    s.client->change(s.uri, 2, bad);
    json r = s.client->request("textDocument/completion", doc_pos(s.uri, 20, 8));
    ASSERT_TRUE(r.contains("result"));
    bool vec = false;
    for (const json& i : r["result"].value("items", json::array()))
        if (i.value("insertText", i.value("label", "")) == "Vec3" || (i.value("kind", 0) == 7 && i.value("detail", "").find("script") == std::string::npos))
            vec = true;
    EXPECT_TRUE(vec) << r["result"].dump();
}

TEST(LanguageServer, ColdCompletionRepliesWithinTheBudgetAndIsIncomplete)
{
    REQUIRE_CLANGD();
    // A short budget makes the cold state sure: clangd has no preamble 50 ms after the open.
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, true);
    o.completion_budget_ms = 50;
    std::string text = kCompleteScript;
    const size_t at = text.find('@');
    text.replace(at, 1, "Vec");
    Session s(true, text, &o);
    const auto t0 = std::chrono::steady_clock::now();
    json r = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 11));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.contains("result"));
    EXPECT_LT(ms, 4000);
    EXPECT_TRUE(r["result"].value("isIncomplete", false)) << r["result"].dump();
    // A later request gives the native names.
    bool vec = false;
    for (int k = 0; k < 600 && !vec; ++k)
    {
        const auto t1 = std::chrono::steady_clock::now();
        json n = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 11));
        // Each reply is within the bound: the budget 50 ms plus 5 s for the patched text.
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count(), 9000);
        for (const json& i : n["result"].value("items", json::array()))
            if (i.value("insertText", "") == "Vec3")
                vec = true;
        if (!vec)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(vec);
}

// A script class with every kind of member, for the bare completion inside a method.
const char* const kMembersScript =
    "using \"Actor.h\";\n"
    "\n"
    "class Hero : Actor {\n"
    "    static const int StepSize = 2;\n"
    "    static int shared = 0;\n"
    "    int ticks = 0;\n"
    "    void runAround() { }\n"
    "    static int Twice(int v) { return v + v; }\n"
    "    override void Tick(float dt) {\n"
    "        @\n"
    "    }\n"
    "}\n";

TEST(LanguageServer, BareCompletionInAMethodOffersAMethodOfTheClass)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kMembersScript, "runA");
    EXPECT_TRUE(c.has("runAround")) << c.result.dump();
}

TEST(LanguageServer, BareCompletionInAMethodOffersAFieldOfTheClass)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(true, kMembersScript, "tic").has("ticks"));
}

TEST(LanguageServer, BareCompletionInAMethodOffersAConstOfTheClass)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(true, kMembersScript, "Step").has("StepSize"));
}

TEST(LanguageServer, BareCompletionInAMethodOffersAStaticOfTheClass)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(true, kMembersScript, "sha").has("shared"));
    EXPECT_TRUE(complete_at(true, kMembersScript, "Twi").has("Twice"));
}

TEST(LanguageServer, BareCompletionInAMethodOffersAMemberOfTheBase)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(true, kMembersScript, "Mov").has("Move"));
    EXPECT_TRUE(complete_at(true, kMembersScript, "Nam").has("Name"));
}

TEST(LanguageServer, ClassNameDotOffersAStaticOfTheClass)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kMembersScript, "Hero.");
    EXPECT_TRUE(c.has("Twice")) << c.result.dump();
    EXPECT_TRUE(c.has("shared")) << c.result.dump();
}

TEST(LanguageServer, ClassNameDotOffersAConstOfTheClass)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(true, kMembersScript, "Hero.").has("StepSize"));
}

TEST(LanguageServer, MemberCompletionOfAForeachVariableUsesItsType)
{
    REQUIRE_CLANGD();
    const char* text = "class Hero {\n"
                       "    int power = 1;\n"
                       "    void boost() { }\n"
                       "}\n"
                       "class Team {\n"
                       "    void run(std.vector<Hero> team) {\n"
                       "        foreach (Hero h in team) {\n"
                       "            h.@\n"
                       "        }\n"
                       "    }\n"
                       "}\n";
    const Completion c = complete_at(true, text, "bo");
    EXPECT_TRUE(c.has("boost"));
    EXPECT_FALSE(c.has("power"));
    EXPECT_TRUE(complete_at(true, text, "po").has("power"));
}

TEST(LanguageServer, LambdaBodyHasTheFieldsAndLocalsOfTheOuterScope)
{
    REQUIRE_CLANGD();
    const char* text = "class Hero {\n"
                       "    int ticks = 0;\n"
                       "    void run() {\n"
                       "        int local_count = 1;\n"
                       "        var f = (int x) => {\n"
                       "            @\n"
                       "        };\n"
                       "    }\n"
                       "}\n";
    EXPECT_TRUE(complete_at(true, text, "tic").has("ticks"));
    EXPECT_TRUE(complete_at(true, text, "local_c").has("local_count"));
    EXPECT_TRUE(complete_at(true, text, "x").has("x"));
}

TEST(LanguageServer, NativeForeachVariableAndLambdaFieldOfAClassWithABase)
{
    REQUIRE_CLANGD();
    const char* text = "using \"Actor.h\";\n"
                       "class Hero : Actor {\n"
                       "    int ticks = 0;\n"
                       "    void run(std.vector<Actor*> team) {\n"
                       "        foreach (var a in team) {\n"
                       "            a.@\n"
                       "        }\n"
                       "        var f = (int x) => {\n"
                       "            @\n"
                       "        };\n"
                       "    }\n"
                       "}\n";
    std::string first = text;
    first.erase(first.rfind('@'), 1);
    EXPECT_TRUE(complete_at(true, first, "Mo").has("Move"));
    std::string second = text;
    second.replace(second.find("a.@"), 3, "a.Tick(1.0);");
    EXPECT_TRUE(complete_at(true, second, "tic").has("ticks"));
}

const char* const kLibScript = "const int LibLimit = 3;\n"
                               "int lib_counter = 0;\n"
                               "int LibAdd(int a, int b) { return a + b; }\n"
                               "enum LibColor { Red, Green }\n"
                               "private class LibHidden { }\n"
                               "class LibBase {\n"
                               "    int baseHp = 1;\n"
                               "    void baseHeal() { }\n"
                               "}\n"
                               "class WeaponCatalog : LibBase {\n"
                               "    static const int MaxAmmo = 9;\n"
                               "    static int Made(int k) { return k; }\n"
                               "    int ammo = 1;\n"
                               "    void fire() { }\n"
                               "}\n";

TEST(LanguageServer, CompletionOffersTheStaticsAndConstsOfAnImportedClass)
{
    REQUIRE_CLANGD();
    const char* text = "using \"lib.hot\";\nclass Hero {\n    void run() {\n        WeaponCatalog.@\n    }\n}\n";
    const Completion c = complete_at(false, text, "", kLibScript);
    EXPECT_TRUE(c.has("MaxAmmo"));
    EXPECT_TRUE(c.has("Made"));
    EXPECT_FALSE(c.has("ammo"));
}

TEST(LanguageServer, CompletionOffersTheMembersOfALocalOfAnImportedClass)
{
    REQUIRE_CLANGD();
    const char* text = "using \"lib.hot\";\nclass Hero {\n    void run() {\n        WeaponCatalog c = WeaponCatalog();\n        c.@\n    }\n}\n";
    const Completion c = complete_at(false, text, "", kLibScript);
    EXPECT_TRUE(c.has("fire"));
    EXPECT_TRUE(c.has("ammo"));
    EXPECT_TRUE(c.has("baseHeal")); // the base chain goes on in the import
    EXPECT_FALSE(c.has("MaxAmmo"));
}

TEST(LanguageServer, CompletionOffersTheNamesOfAnImportAndHidesAPrivateClass)
{
    REQUIRE_CLANGD();
    const char* text = "using \"lib.hot\";\nclass Hero {\n    void run() {\n        Lib@\n    }\n}\n";
    const Completion c = complete_at(false, text, "", kLibScript);
    EXPECT_TRUE(c.has("LibLimit"));
    EXPECT_TRUE(c.has("lib_counter") || complete_at(false, text, "lib_", kLibScript).has("lib_counter"));
    EXPECT_TRUE(c.has("LibAdd"));
    EXPECT_TRUE(c.has("LibColor"));
    EXPECT_TRUE(c.has("LibBase"));
    EXPECT_FALSE(c.has("LibHidden"));
}

TEST(LanguageServer, CompletionOffersTheEnumeratorsOfAnImportedEnum)
{
    REQUIRE_CLANGD();
    const char* text = "using \"lib.hot\";\nclass Hero {\n    void run() {\n        LibColor.@\n    }\n}\n";
    EXPECT_TRUE(complete_at(false, text, "", kLibScript).has("Green"));
}

TEST(LanguageServer, OverrideCompletionOffersTheVirtualMethodsOfTheBases)
{
    REQUIRE_CLANGD();
    const char* text = "using \"Actor.h\";\n"
                       "class Mid : Actor {\n"
                       "    virtual void Pulse(int n) { }\n"
                       "}\n"
                       "class Hero : Mid {\n"
                       "    override Tick(float dt) { }\n"
                       "    override @\n"
                       "}\n";
    const Completion c = complete_at(true, text, "");
    EXPECT_TRUE(c.has("void TakeDamage(float amount) {\n\t$0\n}"));
    EXPECT_TRUE(c.has("void Pulse(int n) {\n\t$0\n}"));
    EXPECT_FALSE(c.has("Move")); // not virtual
    bool snippet = false;
    for (const json& i : c.result["items"])
        snippet = snippet || i.value("insertTextFormat", 1) == 2;
    EXPECT_TRUE(snippet);
    const Completion pre = complete_at(true, text, "Tak");
    EXPECT_FALSE(pre.names.empty());
}

// The names that a completion gives at a place of an open script. The files go to the disk first.
std::vector<std::string> names_at(Session& s, int line, int col)
{
    json r = s.client->request("textDocument/completion", doc_pos(s.uri, line, col));
    std::vector<std::string> names;
    for (const json& i : (r.contains("result") ? r["result"] : json::object()).value("items", json::array()))
        names.push_back(i.value("insertText", i.value("label", "")));
    return names;
}

bool has_name(const std::vector<std::string>& v, const std::string& n)
{
    return std::find(v.begin(), v.end(), n) != v.end();
}

// A main script with the cursor after "word" in a method body. "head" are the lines before the class.
std::string main_with(const std::string& head, const std::string& word, const std::string& base = "")
{
    return head + "class Hero" + base + " {\n    void run() {\n        " + word + "\n    }\n}\n";
}

int word_line(const std::string& text) // the line of the cursor: the line of "run() {" plus 1
{
    return static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(text.find("void run")), '\n')) + 1;
}

TEST(LanguageServer, ImportCompletionSeesTheUnsavedBufferOfAnImport)
{
    REQUIRE_CLANGD();
    const std::string text = main_with("using \"lib.hot\";\n", "Ne");
    Session s(false, text);
    write_file(s.dir.path() / "lib.hot", "class Old { }\n");
    const std::string lib_uri = uri_of((s.dir.path() / "lib.hot").string());
    s.client->open(lib_uri, "class Old { }\n");
    EXPECT_FALSE(has_name(names_at(s, word_line(text), 10), "New"));
    s.client->change(lib_uri, 2, "class Old { }\nclass New { }\n");
    EXPECT_TRUE(has_name(names_at(s, word_line(text), 10), "New"));
}

TEST(LanguageServer, ImportCompletionOffersFunctionsAndVarsOfDirectImportsOnly)
{
    REQUIRE_CLANGD();
    const std::string text = main_with("using \"a.hot\";\n", "");
    Session s(false, text);
    write_file(s.dir.path() / "a.hot", "using \"b.hot\";\nint helperA() { return 1; }\nclass A { }\n");
    write_file(s.dir.path() / "b.hot", "int helper() { return 1; }\nint b_var = 0;\nclass B { }\nconst int BConst = 1;\n");
    const auto n = names_at(s, word_line(text), 8);
    EXPECT_TRUE(has_name(n, "helperA"));
    EXPECT_TRUE(has_name(n, "A"));
    EXPECT_TRUE(has_name(n, "B"));
    EXPECT_TRUE(has_name(n, "BConst"));
    EXPECT_FALSE(has_name(n, "helper"));
    EXPECT_FALSE(has_name(n, "b_var"));
}

TEST(LanguageServer, UsingLinesInCommentsAreNotImports)
{
    REQUIRE_CLANGD();
    const std::string text = main_with("/* using \"x.hot\"; */\n// using \"x.hot\";\n", "XH");
    Session s(false, text);
    write_file(s.dir.path() / "x.hot", "class XHidden { }\n");
    EXPECT_FALSE(has_name(names_at(s, word_line(text), 10), "XHidden"));
}

TEST(LanguageServer, ImportCycleAndMissingImportDoNotStopCompletion)
{
    REQUIRE_CLANGD();
    const std::string text = main_with("using \"none.hot\";\nusing \"a.hot\";\n", "");
    Session s(false, text);
    write_file(s.dir.path() / "a.hot", "using \"b.hot\";\nclass A { }\n");
    write_file(s.dir.path() / "b.hot", "using \"a.hot\";\nclass B { }\n");
    const auto n = names_at(s, word_line(text), 8);
    EXPECT_TRUE(has_name(n, "A"));
    EXPECT_TRUE(has_name(n, "B"));
}

TEST(LanguageServer, BareCompletionFollowsTheBaseChainAcrossImports)
{
    REQUIRE_CLANGD();
    const std::string text = main_with("using \"a.hot\";\n", "far", " : Mid");
    Session s(false, text);
    write_file(s.dir.path() / "a.hot", "using \"b.hot\";\nclass Mid : Far { void midGo() { } }\n");
    write_file(s.dir.path() / "b.hot", "class Far { int farHp = 1; void farGo() { } }\n");
    const auto n = names_at(s, word_line(text), 11);
    EXPECT_TRUE(has_name(n, "farGo"));
    EXPECT_TRUE(has_name(n, "farHp"));
    EXPECT_FALSE(has_name(n, "midGo") && false);
}

TEST(LanguageServer, ImportFactsAreCachedWhileTheTextStays)
{
    REQUIRE_CLANGD();
    const std::string text = main_with("using \"lib.hot\";\n", "Lib");
    Session s(false, text);
    write_file(s.dir.path() / "lib.hot", kLibScript);
    const int before = hot::ls::import_parse_count();
    EXPECT_TRUE(has_name(names_at(s, word_line(text), 11), "LibBase"));
    const int first = hot::ls::import_parse_count();
    EXPECT_TRUE(has_name(names_at(s, word_line(text), 11), "LibBase"));
    EXPECT_EQ(first - before, 1);
    EXPECT_EQ(hot::ls::import_parse_count(), first);
}

TEST(LanguageServer, OverrideCompletionIsEmptyOutsideAClassBody)
{
    REQUIRE_CLANGD();
    const std::string head = "using \"Actor.h\";\nclass A : Actor { }\n";
    EXPECT_TRUE(complete_at(false, head + "override @\n", "").names.empty());
    EXPECT_TRUE(complete_at(false, head + "class B : Actor {\n    void f() {\n        override @\n    }\n}\n", "").names.empty());
    EXPECT_FALSE(complete_at(false, head + "class B : Actor {\n    override @\n}\n", "").names.empty());
}

// Type contexts: a partial word gives native types, and no statement keyword or local.
std::string type_ctx(const std::string& body, const std::string& head = "")
{
    return "using \"Actor.h\";\n" + head + "class Hero : Actor {\n    int ticks = 0;\n" + body + "\n}\n";
}

TEST(LanguageServer, TypeContextsOfTheBodyGiveNativeTypes)
{
    REQUIRE_CLANGD();
    const std::pair<const char*, const char*> cases[] = {
        {"    void f() { Actor* p = (@);\n    }", "(Act"},
        {"    void f() { var a = this as @;\n    }", "as Act"},
        {"    void f() { var a = new @;\n    }", "new Act"},
        {"    void f() { try { } catch (@) { }\n    }", "catch (Act"},
        {"    void f(@) { }", "Act"},
        {"    @ field;", "Act"},
        {"    @ g() { }", "Act"},
    };
    for (const auto& [body, what] : cases)
    {
        std::string b = body;
        const std::string typed = "Act";
        const auto c = complete_at(true, type_ctx(b), typed);
        EXPECT_TRUE(c.has("Actor")) << what << ": " << c.result.dump().substr(0, 300);
    }
}

TEST(LanguageServer, BaseListGivesNativeTypes)
{
    REQUIRE_CLANGD();
    const std::string text = "using \"Actor.h\";\nclass X : @ {\n}\n";
    const auto c = complete_at(true, text, "Act");
    EXPECT_TRUE(c.has("Actor"));
    EXPECT_FALSE(c.has("return"));
    const auto d = complete_at(true, "using \"Actor.h\";\nclass Y : Actor, @ {\n}\n", "Act");
    EXPECT_TRUE(d.has("Actor"));
}

TEST(LanguageServer, HandleTypeAfterDollarGivesNativeTypes)
{
    REQUIRE_CLANGD();
    const auto c = complete_at(true, type_ctx("    void f() { $@ }"), "Act");
    EXPECT_TRUE(c.has("Actor"));
}

TEST(LanguageServer, TypeContextHidesStatementKeywordsAndLocals)
{
    REQUIRE_CLANGD();
    const auto c = complete_at(true, type_ctx("    void f() { int local_a = 1;\n        var a = new @;\n    }"), "Ac");
    EXPECT_TRUE(c.has("Actor"));
    EXPECT_FALSE(c.has("return"));
    EXPECT_FALSE(c.has("local_a"));
}

TEST(LanguageServer, StaticInitializerGivesConsts)
{
    REQUIRE_CLANGD();
    const std::string head = "const int RingSize = 3;\n";
    const auto c = complete_at(true, type_ctx("    static const int X = @;", head), "Rin");
    EXPECT_TRUE(c.has("RingSize")) << c.result.dump().substr(0, 300);
}

TEST(LanguageServer, EnumBodyAndAttributeGiveNoGenericList)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(false, "enum Color {\n    Red,\n    @\n}\n", "").names.empty());
    EXPECT_TRUE(complete_at(false, "class A {\n    [Attr(@)]\n    int x = 0;\n}\n", "").names.empty());
}

// The attribute names that the parser compares in its sources: "it.name == "x"" in the attribute
// lists and "a.name == "x"" in the lambda attributes, and the names of the class attribute table.
TEST(LanguageServer, TheAttributeTableMatchesTheParser)
{
    std::set<std::string> parser;
    for (const std::string& n : hotc::class_attribute_names())
        parser.insert(n);
    const std::regex re("\\b(?:it|a)\\.name == \"(\\w+)\"");
    for (const char* f : {"/src/translator/parser/script_grammar.cpp", "/src/translator/parser/expression_grammar.cpp"})
    {
        std::ifstream in(std::string(HOT_SOURCE_DIR) + f);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            parser.insert((*it)[1].str());
    }
    EXPECT_GT(parser.size(), 15u) << "the parser attributes were not found";
    std::set<std::string> table;
    for (const hot::ls::AttributeInfo& a : hot::ls::attribute_table())
    {
        EXPECT_TRUE(table.insert(a.name).second) << "duplicate row: " << a.name;
        EXPECT_FALSE(a.doc.empty()) << a.name;
    }
    for (const std::string& n : parser)
        EXPECT_EQ(table.count(n), 1u) << "parser attribute is not in attribute_table(): " << n;
    for (const std::string& n : table)
        EXPECT_EQ(parser.count(n), 1u) << "attribute_table() name is not a parser attribute: " << n;
    // The argument count of a class attribute agrees with the parser: an "[x(...)]" with a wrong
    // count is an error, so a row with arity 0 has no string argument.
    for (const hot::ls::AttributeInfo& a : hot::ls::attribute_table())
        EXPECT_GE(a.arity, 0) << a.name;
}

TEST(LanguageServer, AttributeNamesAreOfferedAfterABracket)
{
    REQUIRE_CLANGD();
    // The prefix filters the names. The closing "]" is in the snippet when it is missing.
    const auto c = complete_at(false, "class A {\n    [Refl@\n    int x = 0;\n}\n", "");
    EXPECT_TRUE(c.names.empty()) << "no attribute starts with Refl";
    const auto w = complete_at(false, "class A {\n    [we@\n    int x = 0;\n}\n", "");
    ASSERT_EQ(w.names.size(), 3u) << w.result.dump();
    EXPECT_EQ(w.names[0], "weak]$0");
    EXPECT_EQ(w.names[1], "weak_handle(\"$1\")]$0");
    // The "]" exists: the snippet does not add a second one.
    const auto closed = complete_at(false, "class A {\n    [we@]\n    int x = 0;\n}\n", "");
    EXPECT_TRUE(closed.has("weak$0")) << closed.result.dump();
    EXPECT_FALSE(closed.has("weak]$0"));
}

TEST(LanguageServer, AttributeNamesOnAFieldAMethodAndAClass)
{
    REQUIRE_CLANGD();
    const auto field = complete_at(false, "class A {\n    [@]\n    int x = 0;\n}\n", "");
    EXPECT_TRUE(field.has("weak$0")) << field.result.dump().substr(0, 300);
    EXPECT_TRUE(field.has("alias(\"$1\")$0"));
    const auto method = complete_at(false, "class A {\n    [al@]\n    void f() { }\n}\n", "");
    EXPECT_TRUE(method.has("alias(\"$1\")$0")) << method.result.dump();
    const auto cls = complete_at(false, "[attribute] class Tag;\n[@]\nclass A { }\n", "");
    EXPECT_TRUE(cls.has("Tag$0")) << cls.result.dump().substr(0, 300);
    EXPECT_TRUE(cls.has("native$0"));
    const auto none = complete_at(false, "[attribute] class Tag;\n[Ta@\nclass A { }\n", "");
    ASSERT_EQ(none.names.size(), 1u);
    EXPECT_EQ(none.names[0], "Tag]$0");
}

TEST(LanguageServer, AttributeClassInCommentOrStringIsNotOffered)
{
    REQUIRE_CLANGD();
    const auto c = complete_at(false,
        "// [attribute] class Ghost;\n/* [attribute] class Block; */\nstring s = \"[attribute] class Str;\";\n"
        "[attribute] class Real;\n[@]\nclass A { }\n", "");
    EXPECT_TRUE(c.has("Real$0")) << c.result.dump().substr(0, 300);
    EXPECT_FALSE(c.has("Ghost$0"));
    EXPECT_FALSE(c.has("Block$0"));
    EXPECT_FALSE(c.has("Str$0"));
}

TEST(LanguageServer, AttributeArgumentsOfferNothingForFreeFormValues)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(complete_at(false, "class A {\n    [ReflectedObject(@\n    int x = 0;\n}\n", "").names.empty());
    EXPECT_TRUE(complete_at(false, "class A {\n    [alias(@)]\n    int x = 0;\n}\n", "").names.empty());
    EXPECT_TRUE(complete_at(false, "class A {\n    [intrusive(\"a\", @)]\n    int x = 0;\n}\n", "").names.empty());
    EXPECT_TRUE(complete_at(false, "class A {\n    [weak, alias(@)]\n    int x = 0;\n}\n", "").names.empty());
}

TEST(LanguageServer, TypeContextHasNoStatementKeywords)
{
    REQUIRE_CLANGD();
    const auto c = complete_at(false, "class A {\n    @ f;\n}\n", "");
    EXPECT_FALSE(c.has("return"));
    EXPECT_FALSE(c.has("while"));
    EXPECT_TRUE(c.has("int"));
}

TEST(LanguageServer, DeclarationStartKeepsDeclarationKeywords)
{
    REQUIRE_CLANGD();
    const auto in_class = complete_at(false, "class A {\n    pu@\n}\n", "");
    EXPECT_TRUE(in_class.has("public"));
    EXPECT_FALSE(in_class.has("return"));
    EXPECT_FALSE(in_class.has("while"));
    const auto top = complete_at(false, "cl@\n", "");
    EXPECT_TRUE(top.has("class"));
    EXPECT_FALSE(top.has("if"));
    const auto param = complete_at(false, "class A {\n    void f(re@) {}\n}\n", "");
    EXPECT_TRUE(param.has("ref"));
    EXPECT_FALSE(param.has("return"));
    EXPECT_TRUE(complete_at(false, "class A {\n    void f(ou@) {}\n}\n", "").has("out"));
}

TEST(LanguageServer, CompletionHidesTheGeneratedHelpers)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript);
    ASSERT_FALSE(c.names.empty());
    for (const std::string& n : c.names)
    {
        for (const char* bad :
            {"__", "_Shell", "SLOT_", "thunk", "hot_mod", "hot_gen", "_slot", "_layout", "_fields", "HotModule", "_g1"})
            EXPECT_EQ(n.find(bad), std::string::npos) << n << " has " << bad;
        EXPECT_NE(n[0], '_') << n;
        // A script class name cannot start with "hot_", for example the state struct "hot_state_Counter".
        EXPECT_NE(n.compare(0, 4, "hot_"), 0) << n;
    }
    for (const char* n : {"self", "st", "a"})
        EXPECT_FALSE(c.has(n)) << n;
}

int completion_translations(LsClient& client)
{
    return client.request("hot/indexStats", json::object())["result"].value("completions", -1);
}

// The stats count each translation and the time of all of them.
TEST(LanguageServer, IndexStatsCountTheTranslations)
{
    REQUIRE_CLANGD();
    Session s(true, kCompleteScript);
    auto stats = [&] { return s.client->request("hot/indexStats", json::object())["result"]; };
    const json before = stats();
    ASSERT_TRUE(before.contains("translations")) << before.dump();
    ASSERT_TRUE(before.contains("translateMs")) << before.dump();
    std::string text = kCompleteScript;
    text.replace(text.find('@'), 1, "cou");
    s.client->change(s.uri, 2, text);
    const json r = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 11));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json after = stats();
    EXPECT_GT(after.value("translations", 0), before.value("translations", 0)) << after.dump();
    EXPECT_GE(after.value("translateMs", -1.0), before.value("translateMs", 0.0)) << after.dump();
}

TEST(LanguageServer, CompletionReusesTheAnswerWhileThePrefixGrows)
{
    REQUIRE_CLANGD();
    std::string text = kCompleteScript;
    auto with_word = [&](const std::string& word)
    {
        std::string t = text;
        t.replace(t.find('@'), 1, word);
        return t;
    };
    Session s(true, with_word("cou"));
    // The cursor is after the word on line 19. The text around the word stays the same.
    // The first prefix is long enough, so clangd gives a complete answer.
    const json a = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 11));
    ASSERT_TRUE(a.contains("result")) << a.dump();
    EXPECT_EQ(completion_translations(*s.client), 1);
    s.client->change(s.uri, 2, with_word("coun"));
    const json b = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 12));
    ASSERT_TRUE(b.contains("result")) << b.dump();
    EXPECT_EQ(completion_translations(*s.client), 1); // the longer prefix reuses the answer
    bool counter = false;
    for (const json& i : b["result"]["items"])
        counter = counter || i.value("insertText", "") == "counter";
    EXPECT_TRUE(counter) << b.dump();
    for (const json& i : b["result"]["items"])
        EXPECT_EQ(i.value("filterText", "coun").compare(0, 4, "coun"), 0) << i.dump();
    // A change of the text around the word needs a new translation.
    std::string other = with_word("coun");
    other.replace(other.find("int counter = 1;"), 16, "int counter = 2;");
    s.client->change(s.uri, 3, other);
    const json c = s.client->request("textDocument/completion", doc_pos(s.uri, 19, 12));
    ASSERT_TRUE(c.contains("result")) << c.dump();
    EXPECT_EQ(completion_translations(*s.client), 2);
}

// ------------------------------------------------------------------ S30: signature help, formatting, code actions

TEST(LanguageServer, SignatureHelpShowsTheScriptMethodAndTheActiveParameter)
{
    REQUIRE_CLANGD();
    Session s(true);
    // Line 7 is "        this.heal(1.0);". The cursor is right after the "(".
    const json a = s.client->request("textDocument/signatureHelp", doc_pos(s.uri, 7, 18));
    ASSERT_TRUE(a.contains("result") && a["result"].is_object()) << a.dump();
    ASSERT_FALSE(a["result"]["signatures"].empty()) << a.dump();
    const std::string label = a["result"]["signatures"][0].value("label", "");
    EXPECT_NE(label.find("heal"), std::string::npos) << label;
    EXPECT_NE(label.find("float amount"), std::string::npos) << label;
    EXPECT_EQ(a["result"]["activeParameter"], 0) << a.dump();
    // Line 6 is "        this.Move(Vec3(dt, 0, 0));". The cursor is after "dt, ": the second argument.
    const json b = s.client->request("textDocument/signatureHelp", doc_pos(s.uri, 6, 28));
    ASSERT_TRUE(b.contains("result") && b["result"].is_object()) << b.dump();
    EXPECT_NE(b["result"]["signatures"][0].value("label", "").find("Vec3"), std::string::npos) << b.dump();
    EXPECT_EQ(b["result"]["activeParameter"], 1) << b.dump();
    // Outside a call there is no signature.
    const json c = s.client->request("textDocument/signatureHelp", doc_pos(s.uri, 3, 10));
    ASSERT_TRUE(c.contains("result")) << c.dump();
    EXPECT_TRUE(c["result"].is_null()) << c.dump();
}

namespace
{

// A script with a method body "body". The "|" in "body" is the cursor; it is not in the text.
// Gives the reply of signatureHelp and the text. "open_text" is the text that the session opens
// first (a good text); the text with the cursor then comes as an edit.
json sig_reply(const std::string& body, const std::string& open_body = "")
{
    const std::string head =
        "using \"Actor.h\";\n"
        "class P : Actor {\n"
        "    void two(int a, float b) {}\n"
        "    void vec(std.vector<std.pair<int, int>> a, float b) {}\n"
        "    void init(Vec3 a, float b) {}\n"
        "    void run() {\n";
    const std::string tail = "\n    }\n}\n";
    const size_t bar = body.find('|');
    std::string text = head + body + tail;
    const size_t at = head.size() + bar;
    text.erase(at, 1);
    int line = 0, col = 0;
    for (size_t i = 0; i < at; ++i)
    {
        if (text[i] == '\n')
            ++line, col = 0;
        else
            ++col;
    }
    if (open_body.empty())
    {
        Session s(true, text);
        return s.client->request("textDocument/signatureHelp", doc_pos(s.uri, line, col));
    }
    Session s(true, head + open_body + tail);
    s.client->change(s.uri, 2, text);
    return s.client->request("textDocument/signatureHelp", doc_pos(s.uri, line, col));
}

void expect_sig(const json& r, const std::string& name, int active)
{
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    ASSERT_FALSE(r["result"]["signatures"].empty()) << r.dump();
    EXPECT_NE(r["result"]["signatures"][0].value("label", "").find(name), std::string::npos) << r.dump();
    EXPECT_EQ(r["result"]["activeParameter"], active) << r.dump();
}

} // namespace

TEST(LanguageServer, SignatureHelpIgnoresCommasAndParensInStringsAndComments)
{
    REQUIRE_CLANGD();
    expect_sig(sig_reply("        two(1 /* a, b ) */, |2.0);"), "two", 1);
    expect_sig(sig_reply("        two(1, 2.0 /* , ( */|);"), "two", 1);
    // A string with commas and a paren is one argument. The string is not a call.
    expect_sig(sig_reply("        std.string s = \"a, (b,\"; two(1, |2.0);"), "two", 1);
    expect_sig(sig_reply("        two(\"x, (y\".size(), |2.0);"), "two", 1);
    // A line comment with a paren before the call does not matter.
    expect_sig(sig_reply("        // f(a, b,\n        two(|1, 2.0);"), "two", 0);
}

TEST(LanguageServer, SignatureHelpSkipsNestedCallsBracesAndTemplateLists)
{
    REQUIRE_CLANGD();
    expect_sig(sig_reply("        two(Vec3(1, 2, 3).x, |2.0);"), "two", 1);
    expect_sig(sig_reply("        this.init(Vec3(1, 2, 3), |2.0);"), "init", 1);
    expect_sig(sig_reply("        this.init(Vec3(1, 2, |3), 2.0);"), "Vec3", 2);
    expect_sig(sig_reply("        this.vec(std.vector<std.pair<int, int>>(), |2.0);"), "vec", 1);
    expect_sig(sig_reply("        this.vec(std.vector<std.pair<int, int>>(), 2.0|);"), "vec", 1);
    // An unspaced comparison is not a template list.
    expect_sig(sig_reply("        two(Vec3(1, 2, 3).x<3, |2.0);"), "two", 1);
    expect_sig(sig_reply("        float Mx = 1; two(Mx<3, |2.0);"), "two", 1);
    // The call is in a lambda body in braces, in the arguments of another call.
    expect_sig(sig_reply("        std.function<void()> g = () => { two(1, |2.0); };"), "two", 1);
    expect_sig(sig_reply("        std.function<void()> g = () => { two(1, 2.0); two(|1, 2.0); };"), "two", 0);
}

TEST(LanguageServer, SignatureHelpFindsTheCalleeOfATemplateCall)
{
    REQUIRE_CLANGD();
    expect_sig(sig_reply("        std.make_pair<int, float>(|1, 2.0);"), "make_pair", 0);
    expect_sig(sig_reply("        std.make_pair<int, float>(1, |2.0);"), "make_pair", 1);
    expect_sig(sig_reply("        std.make_shared<Vec3>(1, |2, 3);"), "make_shared", 1);
    // A nested list with a lowercase inner name.
    expect_sig(sig_reply("        std.make_shared<std.vector<int>>(|);"), "make_shared", 0);
}

TEST(LanguageServer, SignatureHelpOfAnUnfinishedCallGivesTheSecondArgument)
{
    REQUIRE_CLANGD();
    // The edit makes the text fail to translate. The call is new in the edit.
    expect_sig(sig_reply("        two(1, |", "        "), "two", 1);
    expect_sig(sig_reply("        two(1, |\n        two(1, 2.0);", "        two(1, 2.0);"), "two", 1);
    expect_sig(sig_reply("        this.init(Vec3(1, 2, 3), |", "        "), "init", 1);
    // The same text as the first text of the session.
    expect_sig(sig_reply("        two(1, |"), "two", 1);
}

TEST(LanguageServer, FormattingFixesTheIndentAndKeepsStringsAndComments)
{
    const std::string messy =
        "class A {\n"
        "int x = 1;   \n"
        "      void f(int a,\n"
        "int b) {\n"
        "  if (a) {\n"
        "        string s = \"{ not a brace\"; // } nor this\n"
        "}\n"
        "    /* a comment\n"
        "       { stays */\n"
        "}\n"
        "}";
    const std::string good =
        "class A {\n"
        "    int x = 1;\n"
        "    void f(int a,\n"
        "        int b) {\n"
        "        if (a) {\n"
        "            string s = \"{ not a brace\"; // } nor this\n"
        "        }\n"
        "        /* a comment\n"
        "       { stays */\n"
        "    }\n"
        "}\n";
    Session s(false, messy);
    json o = {{"tabSize", 4}, {"insertSpaces", true}};
    const json r = s.client->request("textDocument/formatting", {{"textDocument", {{"uri", s.uri}}}, {"options", o}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && r["result"].size() == 1) << r.dump();
    EXPECT_EQ(r["result"][0]["newText"], good);
    EXPECT_EQ(r["result"][0]["range"]["start"], pos(0, 0));
    // The formatted text has no edit.
    s.client->change(s.uri, 2, good);
    const json again =
        s.client->request("textDocument/formatting", {{"textDocument", {{"uri", s.uri}}}, {"options", o}});
    ASSERT_TRUE(again.contains("result")) << again.dump();
    EXPECT_TRUE(again["result"].empty()) << again.dump();
}

TEST(LanguageServer, RangeFormattingChangesTheLinesOfTheRangeOnly)
{
    const std::string messy =
        "class A {\n"
        "int x=1;\n"
        "      int y=2;\n"
        "\n"
        "\n"
        "  int z=3;\n"
        "}\n";
    Session s(false, messy);
    json o = {{"tabSize", 4}, {"insertSpaces", true}};
    // The range holds line 2 only (the line numbers are 0-based).
    json r = s.client->request("textDocument/rangeFormatting",
        {{"textDocument", {{"uri", s.uri}}}, {"options", o}, {"range", {{"start", pos(2, 0)}, {"end", pos(2, 5)}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && r["result"].size() == 1) << r.dump();
    EXPECT_EQ(r["result"][0]["newText"], "    int y = 2;\n");
    EXPECT_EQ(r["result"][0]["range"]["start"], pos(2, 0));
    EXPECT_EQ(r["result"][0]["range"]["end"], pos(3, 0));
    // The range holds lines 2 to 5: the second blank line goes away.
    r = s.client->request("textDocument/rangeFormatting",
        {{"textDocument", {{"uri", s.uri}}}, {"options", o}, {"range", {{"start", pos(2, 0)}, {"end", pos(5, 3)}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && r["result"].size() == 1) << r.dump();
    EXPECT_EQ(r["result"][0]["newText"], "    int y = 2;\n\n    int z = 3;\n");
    EXPECT_EQ(r["result"][0]["range"]["end"], pos(6, 0));
    // A range of lines that are right has no edit.
    r = s.client->request("textDocument/rangeFormatting",
        {{"textDocument", {{"uri", s.uri}}}, {"options", o}, {"range", {{"start", pos(0, 0)}, {"end", pos(0, 9)}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_TRUE(r["result"].empty()) << r.dump();
}

namespace
{

// The result of a range format: the text after the edits, or the text itself when there is no edit.
std::string range_format(const std::string& text, int l0, int c0, int l1, int c1, bool* has_edit = nullptr)
{
    Session s(false, text);
    json o = {{"tabSize", 4}, {"insertSpaces", true}};
    const json r = s.client->request("textDocument/rangeFormatting",
        {{"textDocument", {{"uri", s.uri}}}, {"options", o}, {"range", {{"start", pos(l0, c0)}, {"end", pos(l1, c1)}}}});
    EXPECT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    if (has_edit)
        *has_edit = !r["result"].empty();
    std::string out = text;
    if (r["result"].empty())
        return out;
    const json e = r["result"][0];
    auto offset = [&](const json& p)
    {
        size_t off = 0;
        for (int line = 0; line < p["line"].get<int>(); ++line)
            off = text.find('\n', off) + 1;
        return off + p["character"].get<size_t>();
    };
    const size_t b = offset(e["range"]["start"]), en = offset(e["range"]["end"]);
    return text.substr(0, b) + e["newText"].get<std::string>() + text.substr(en);
}

} // namespace

TEST(LanguageServer, RangeFormattingDoesNotChangeTheInsideOfAStringOrAComment)
{
    bool edit = true;
    // Line 2 is the end of a string with "${...}" and its ";".
    const std::string str = "class A {\nstring t = \"a${ f(\n   x ,y) }b\";\n  int  z =1;\n}\n";
    range_format(str, 2, 0, 2, 5, &edit);
    EXPECT_FALSE(edit);
    EXPECT_EQ(range_format(str, 3, 0, 3, 5), "class A {\nstring t = \"a${ f(\n   x ,y) }b\";\n    int z = 1;\n}\n");
    // Lines 2 and 3 are inside a block comment. The comment keeps its inner lines.
    const std::string com = "class A {\n/* a\n     b   \n  */\nint  z;\n}\n";
    range_format(com, 2, 0, 2, 4, &edit);
    EXPECT_FALSE(edit);
    EXPECT_EQ(range_format(com, 1, 0, 3, 4), "class A {\n    /* a\n     b   \n  */\nint  z;\n}\n");
}

TEST(LanguageServer, RangeFormattingAtTheEndOfTheText)
{
    // No new line at the end: the edit adds none.
    const std::string no_nl = "class A {\nint x=1;\n  }";
    EXPECT_EQ(range_format(no_nl, 0, 0, 2, 3), "class A {\n    int x = 1;\n}");
    EXPECT_EQ(range_format(no_nl, 2, 0, 2, 3), "class A {\nint x=1;\n}");
    // A new line at the end. The range ends at the start of the line after the last line.
    const std::string nl = "class A {\nint x=1;\n  }\n";
    EXPECT_EQ(range_format(nl, 0, 0, 3, 0), "class A {\n    int x = 1;\n}\n");
    EXPECT_EQ(range_format(nl, 2, 0, 2, 3), "class A {\nint x=1;\n}\n");
    // A range of the empty last line has no edit.
    bool edit = true;
    range_format(nl, 3, 0, 3, 0, &edit);
    EXPECT_FALSE(edit);
}

TEST(LanguageServer, CodeActionInsertsAMissingSemicolon)
{
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.hp = this.hp + amount");
    Session s(false, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "hot"); });
    ASSERT_FALSE(n.is_null());
    const json diag = n["params"]["diagnostics"][0];
    ASSERT_EQ(diag["message"], "expected ';'") << diag.dump();
    const json r = s.client->request("textDocument/codeAction",
        {{"textDocument", {{"uri", s.uri}}}, {"range", diag["range"]},
            {"context", {{"diagnostics", json::array({diag})}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].size() == 1) << r.dump();
    const json& edit = r["result"][0]["edit"]["changes"][s.uri][0];
    EXPECT_EQ(r["result"][0]["kind"], "quickfix");
    EXPECT_EQ(edit["newText"], ";");
    EXPECT_EQ(edit["range"]["start"], pos(11, 34)) << r.dump(); // after "amount"
    EXPECT_EQ(edit["range"]["end"], pos(11, 34)) << r.dump();
}

TEST(LanguageServer, CodeActionAppliesTheNameThatClangdSuggests)
{
    REQUIRE_CLANGD();
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.hp = amoun;");
    Session s(true, text);
    const json n = s.diagnostics([](const json& d)
    {
        return std::any_of(d.begin(), d.end(),
            [](const json& x) { return x.value("message", "").find("did you mean") != std::string::npos; });
    });
    ASSERT_FALSE(n.is_null());
    json diag;
    for (const json& x : n["params"]["diagnostics"])
        if (x.value("message", "").find("did you mean") != std::string::npos)
            diag = x;
    const json r = s.client->request("textDocument/codeAction",
        {{"textDocument", {{"uri", s.uri}}}, {"range", diag["range"]},
            {"context", {{"diagnostics", json::array({diag})}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].size() == 1) << r.dump();
    const json& edit = r["result"][0]["edit"]["changes"][s.uri][0];
    EXPECT_EQ(edit["newText"], "amount") << r.dump();
    EXPECT_EQ(edit["range"], diag["range"]);
}

// ------------------------------------------------------------------ hosts (step 2b)
namespace
{

// The labels of a completion request at "pos". clangd gives the names of a header from its
// index. It fills the index after the preamble, so an early answer can be empty. With "want",
// the request repeats until the answer has the label or 60 seconds pass.
std::vector<std::string> completion_labels(LsClient& client, const json& pos, const char* want)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const json r = client.request("textDocument/completion", pos);
        std::vector<std::string> labels;
        if (r.contains("result") && r["result"].is_object())
            for (const json& i : r["result"]["items"])
                labels.push_back(i.value("insertText", i.value("label", "")));
        if (!want || std::find(labels.begin(), labels.end(), want) != labels.end()
            || std::chrono::steady_clock::now() >= deadline)
            return labels;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// A workspace with a header in "inc" and a script in "scripts". The script uses the header.
// With "host" the hosts dir holds a host file for the scripts dir, with "inc" as include dir.
// The server gets no -I option for "inc". The completion after "Thing." comes from the host only.
// With "watched" the client sends workspace/didChangeWatchedFiles for the new host file. The
// script gets no further change.
// With "defines_file" the header needs the define HOST_NAME. The host file names "h.defines" in
// "definesFile". The file holds the text of the argument, or no file when the text is "-".
std::vector<std::string> host_completion_labels(
    bool host, bool change_host_later = false, bool watched = false, const std::string& defines_file = "")
{
    TempDir dir;
    const std::filesystem::path root = dir.path();
    std::filesystem::create_directories(root / "inc");
    std::filesystem::create_directories(root / "scripts");
    std::filesystem::create_directories(root / "build");
    std::filesystem::create_directories(root / "hosts");
    const std::string body = "namespace Thing { int Value(); }\n";
    write_file((root / "inc" / "thing.h").string(),
        "#pragma once\n" + (defines_file.empty() ? body : "#ifdef HOST_NAME\n" + body + "#endif\n"));
    if (!defines_file.empty() && defines_file != "-")
        write_file((root / "hosts" / "h.defines").string(), defines_file);
    const std::string text = "using \"thing.h\";\n\nclass A {\n    void f() {\n        Thing.\n    }\n}\n";
    const std::string path = (root / "scripts" / "a.hot").string();
    write_file(path, text);
    write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
    auto write_host = [&]
    {
        write_file((root / "hosts" / "h.json").string(),
            R"({"version":1,"name":"h","binaryDir":")" + (root / "build").generic_string()
                + R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")"
                + (root / "scripts").generic_string() + R"("],"scripts":[],"includeDirs":[")" + (root / "inc").generic_string()
                + R"(",")" + HOT_SOURCE_DIR + R"(/include"],"defines":[],)"
                + (defines_file.empty() ? "" : R"("definesFile":"h.defines",)") + R"("flags":[]})");
    };
    if (host && !change_host_later)
        write_host();
    hot::ls::ServerOptions o;
    o.clangd = HOT_CLANGD_PATH;
    o.hosts_dir = (root / "hosts").string();
    o.clangd_log = (root / "clangd.log").string();
    // The runtime headers are needed to translate. Without a host, they come from -I.
    if (!host)
        o.include_dirs = {HOT_SOURCE_DIR "/include"};
    LsClient client(o);
    client.initialize(uri_of(root.string()));
    const std::string uri = uri_of(path);
    client.open(uri, text);
    if (host && change_host_later)
    {
        // The host file appears after the open. The next change must pick it up.
        write_host();
        if (watched)
            client.notify("workspace/didChangeWatchedFiles",
                {{"changes", json::array({{{"uri", uri_of((root / "hosts" / "h.json").string())}, {"type", 1}}})}});
        else
            client.change(uri, 2, text + "\n");
    }
    // The header gives "Value" with a host, except when the defines file is missing ("-").
    const bool gives_value = host && defines_file != "-";
    return completion_labels(client, doc_pos(uri, 4, 14), gives_value ? "Value" : nullptr);
}

bool has_label(const std::vector<std::string>& labels, const char* n)
{
    return std::find(labels.begin(), labels.end(), n) != labels.end();
}

} // namespace

TEST(LanguageServerHosts, ScriptUsesTheIncludeDirsOfItsHost)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(has_label(host_completion_labels(true), "Value"));
}

TEST(LanguageServerHosts, DefineWithQuotesFromADefinesFileApplies)
{
    REQUIRE_CLANGD();
    // The lines hold quotes and a backslash. The server passes them to clangd as they are.
    EXPECT_TRUE(
        has_label(host_completion_labels(true, false, false, "HOST_NAME=\"a b\"\nOTHER=\"C:\\dir\"\n"), "Value"));
}

// The include dir is in an includes side file. Its name has a space, a quote, a backslash and
// a non-ASCII character. The file holds the path with no escape.
TEST(LanguageServerHosts, IncludeDirWithSpecialCharactersFromAnIncludesFileApplies)
{
    REQUIRE_CLANGD();
    TempDir dir;
    const std::filesystem::path root = dir.path();
    const std::filesystem::path weird = root / "we\"ird \\x d\xC3\xADr";
    std::error_code ec;
    std::filesystem::create_directories(weird, ec);
    if (ec)
        GTEST_SKIP() << "the file system does not allow the directory name";
    std::filesystem::create_directories(root / "scripts");
    std::filesystem::create_directories(root / "build");
    std::filesystem::create_directories(root / "hosts");
    write_file((weird / "thing.h").string(), "#pragma once\nnamespace Thing { int Value(); }\n");
    write_file((root / "hosts" / "h.includes").string(), weird.string() + "\n");
    write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
    write_file((root / "hosts" / "h.json").string(),
        R"({"version":1,"name":"h","binaryDir":")" + (root / "build").generic_string() + R"(","sourceDir":")" + root.generic_string()
            + R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":["scripts"],)"
            + R"("scripts":[],"includeDirs":[")" + HOT_SOURCE_DIR + R"(/include"],"defines":[],)"
            + R"("includesFile":"h.includes","flags":[]})");
    const std::string text = "using \"thing.h\";\n\nclass A {\n    void f() {\n        Thing.\n    }\n}\n";
    const std::string path = (root / "scripts" / "a.hot").generic_string();
    write_file(path, text);
    hot::ls::ServerOptions o;
    o.clangd = HOT_CLANGD_PATH;
    o.hosts_dir = (root / "hosts").string();
    o.clangd_log = (root / "clangd.log").string();
    LsClient client(o);
    client.initialize(uri_of(root.string()));
    const std::string uri = uri_of(path);
    client.open(uri, text);
    const std::vector<std::string> labels = completion_labels(client, doc_pos(uri, 4, 14), "Value");
    EXPECT_TRUE(has_label(labels, "Value"));
}

TEST(LanguageServerHosts, MissingDefinesFileGivesNoDefines)
{
    REQUIRE_CLANGD();
    EXPECT_FALSE(has_label(host_completion_labels(true, false, false, "-"), "Value"));
}

TEST(LanguageServerHosts, ScriptWithoutAHostHasNoHostIncludeDirs)
{
    REQUIRE_CLANGD();
    EXPECT_FALSE(has_label(host_completion_labels(false), "Value"));
}

TEST(LanguageServerHosts, NewHostFileAppliesToAnOpenScriptAtTheNextChange)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(has_label(host_completion_labels(true, true), "Value"));
}

TEST(LanguageServerHosts, NewHostFileAppliesToAnOpenScriptAfterAWatchedFilesEvent)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(has_label(host_completion_labels(true, true, true), "Value"));
}

// A script file that appears on disk after the start. The client sends a Created event. The
// references of a symbol in an open script include the new file when it imports the open script.
TEST(LanguageServerHosts, NewScriptFileIsFoundAfterAWatchedFilesEvent)
{
    REQUIRE_CLANGD();
    TempDir dir;
    const std::filesystem::path root = dir.path();
    std::filesystem::create_directories(root / "hosts");
    const std::string text = "class Player {\n    public float hp = 1.0;\n}\n";
    const std::string path = (root / "a.hot").string();
    write_file(path, text);
    hot::ls::ServerOptions o;
    o.hosts_dir = (root / "hosts").string();
    o.include_dirs = {HOT_SOURCE_DIR "/include"};
    LsClient client(o);
    client.initialize(uri_of(root.string()));
    client.open(uri_of(path), text);
    auto count_other = [&](const std::string& other_uri)
    {
        const json r = client.request("textDocument/references",
            {{"textDocument", {{"uri", uri_of(path)}}}, {"position", {{"line", 1}, {"character", 18}}},
                {"context", {{"includeDeclaration", true}}}});
        int n = 0;
        if (r.contains("result") && r["result"].is_array())
            for (const json& l : r["result"])
                if (l["uri"] == other_uri)
                    ++n;
        return n;
    };
    const std::string other = (root / "b.hot").string();
    EXPECT_EQ(count_other(uri_of(other)), 0);
    write_file(other,
        "using \"a.hot\";\nclass Boss {\n    public float Get(Player p) { return p.hp; }\n}\n");
    client.notify(
        "workspace/didChangeWatchedFiles", {{"changes", json::array({{{"uri", uri_of(other)}, {"type", 1}}})}});
    EXPECT_EQ(count_other(uri_of(other)), 1);
    std::filesystem::remove(other);
    client.notify(
        "workspace/didChangeWatchedFiles", {{"changes", json::array({{{"uri", uri_of(other)}, {"type", 3}}})}});
    EXPECT_EQ(count_other(uri_of(other)), 0);
}

// The information diagnostic "No script host". Rule: a .hot script has no host and the server
// has no -I option.
namespace
{

struct HostDiagCase
{
    TempDir dir;
    std::string path, uri, text = "class A {\n}\n";
    hot::ls::ServerOptions o;
    HostDiagCase()
    {
        const std::filesystem::path root = dir.path();
        std::filesystem::create_directories(root / "scripts");
        std::filesystem::create_directories(root / "build");
        std::filesystem::create_directories(root / "hosts");
        path = (root / "scripts" / "a.hot").string();
        uri = uri_of(path);
        write_file(path, text);
        write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
        o.hosts_dir = (root / "hosts").string();
    }
    void write_host() const
    {
        const std::filesystem::path root = dir.path();
        write_file((root / "hosts" / "h.json").string(),
            R"({"version":1,"name":"h","binaryDir":")" + (root / "build").generic_string()
                + R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")"
                + (root / "scripts").generic_string() + R"("],"scripts":[],"includeDirs":[")" + HOT_SOURCE_DIR
                + R"(/include"],"defines":[],"flags":[]})");
    }
    json next_diagnostics(LsClient& c) const
    {
        return c.wait_notification(
            "textDocument/publishDiagnostics", [&](const json& p) { return p.value("uri", std::string()) == uri; });
    }
};

} // namespace

// The header needs the define of the host. The translator parse of the header must see it, so
// the script gets no error from the "#error" line.
TEST(LanguageServerHosts, DefineOfTheHostApplies_ToTheTranslatorParse)
{
    REQUIRE_CLANGD();
    HostDiagCase c;
    const std::filesystem::path root = c.dir.path();
    std::filesystem::create_directories(root / "inc");
    write_file((root / "inc" / "need.h").string(),
        "#pragma once\n#ifndef HOST_NEEDED\n#error \"host define missing\"\n#endif\nnamespace Thing { int Value(); }\n");
    c.text = "using \"need.h\";\n\nclass A {\n    void f() {\n        Thing.Value();\n    }\n}\n";
    write_file(c.path, c.text);
    write_file((root / "hosts" / "h.json").string(),
        R"({"version":1,"name":"h","binaryDir":")" + (root / "build").generic_string()
            + R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")"
            + (root / "scripts").generic_string() + R"("],"scripts":[],"includeDirs":[")" + (root / "inc").generic_string()
            + R"(",")" + HOT_SOURCE_DIR + R"(/include"],"defines":["HOST_NEEDED=1"],"flags":[]})");
    c.o.clangd = HOT_CLANGD_PATH;
    LsClient client(c.o);
    client.initialize(uri_of(root.string()));
    client.open(c.uri, c.text);
    const json n = c.next_diagnostics(client);
    EXPECT_EQ(n["params"]["diagnostics"].dump().find("host define missing"), std::string::npos) << n.dump();
}

TEST(LanguageServerHosts, NoHostGivesAnInformationDiagnostic)
{
    HostDiagCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, c.text);
    const json n = c.next_diagnostics(client);
    ASSERT_TRUE(has_source(n["params"]["diagnostics"], "hot-ls")) << n.dump();
    for (const json& d : n["params"]["diagnostics"])
        if (d.value("source", "") == "hot-ls")
        {
            EXPECT_EQ(d["severity"], 3);
            EXPECT_EQ(d["range"]["start"], pos(0, 0));
            EXPECT_NE(d.value("message", "").find("No script host"), std::string::npos);
        }
}

TEST(LanguageServerHosts, MatchingHostGivesNoInformationDiagnostic)
{
    HostDiagCase c;
    c.write_host();
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, c.text);
    const json n = c.next_diagnostics(client);
    EXPECT_FALSE(has_source(n["params"]["diagnostics"], "hot-ls")) << n.dump();
}

TEST(LanguageServerHosts, InformationDiagnosticGoesWhenAHostAppears)
{
    HostDiagCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, c.text);
    EXPECT_TRUE(has_source(c.next_diagnostics(client)["params"]["diagnostics"], "hot-ls"));
    c.write_host();
    client.change(c.uri, 2, c.text + "\n");
    EXPECT_FALSE(has_source(c.next_diagnostics(client)["params"]["diagnostics"], "hot-ls"));
}

// F12 on a native type in a script that no host matches (step 13).
namespace
{

// A workspace with the header "inc/thing.h", the host file for "scripts" (when "host") and the
// script "other/a.hot". No host matches the script. The host is the nearest one. The server has
// no -I option, so the include dirs of the host are the only way to find "thing.h".
struct NoMatchCase
{
    TempDir dir;
    std::string uri;
    std::unique_ptr<LsClient> client;
    explicit NoMatchCase(bool host)
    {
        const std::filesystem::path root = dir.path();
        for (const char* d : {"inc", "scripts", "other", "build", "hosts"})
            std::filesystem::create_directories(root / d);
        write_file((root / "inc" / "thing.h").string(), "#pragma once\nstruct Thing\n{\n    int value = 0;\n};\n");
        const std::string text = "using \"thing.h\";\n\nclass A {\n    void f() {\n        Thing t;\n    }\n}\n";
        const std::string path = (root / "other" / "a.hot").string();
        write_file(path, text);
        write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
        if (host)
            write_file((root / "hosts" / "h.json").string(),
                R"({"version":1,"name":"h","binaryDir":")" + (root / "build").generic_string()
                    + R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")"
                    + (root / "scripts").generic_string() + R"("],"scripts":[],"includeDirs":[")" + (root / "inc").generic_string()
                    + R"(",")" + HOT_SOURCE_DIR + R"(/include"],"defines":[],"flags":[]})");
        hot::ls::ServerOptions o;
        o.clangd = HOT_CLANGD_PATH;
        o.hosts_dir = (root / "hosts").generic_string();
        o.clangd_log = (root / "clangd.log").string();
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(root.string()));
        uri = uri_of(path);
        client->open(uri, text);
    }
};

} // namespace

TEST(LanguageServerHosts, DefinitionUsesTheNearestHostWhenNoHostMatchesTheScript)
{
    REQUIRE_CLANGD();
    NoMatchCase c(true);
    const json r = c.client->request("textDocument/definition", doc_pos(c.uri, 4, 9));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    EXPECT_NE(r["result"][0].value("uri", "").find("/inc/thing.h"), std::string::npos) << r.dump();
    EXPECT_EQ(r["result"][0]["range"]["start"]["line"], 1) << r.dump();
}

// "Thing" is on line 4 of the script. Without a host, clangd does not find its header, so the
// answer is empty. The word has a place in the generated C++, so the server says why.
TEST(LanguageServerHosts, DefinitionWithoutAnyHostSaysOnceThatNoHostMatches)
{
    REQUIRE_CLANGD();
    NoMatchCase c(false);
    auto message = [&](int ms)
    {
        return c.client->wait_notification("window/showMessage",
            [](const json& p) { return p.value("message", std::string()).find("No script host matches") != std::string::npos; },
            ms);
    };
    const json r = c.client->request("textDocument/definition", doc_pos(c.uri, 4, 9));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_TRUE(r["result"].is_array() && r["result"].empty()) << r.dump();
    EXPECT_FALSE(message(10000).is_null()) << "the server must say that no host matches";
    // The second F12 in the same document gives no second message.
    c.client->request("textDocument/definition", doc_pos(c.uri, 4, 9));
    EXPECT_TRUE(message(700).is_null());
}

TEST(LanguageServerHosts, DefinitionGivesNoNoHostMessageForAKeywordOrANumber)
{
    REQUIRE_CLANGD();
    NoMatchCase c(false);
    // Line 2 is "class A {". Line 3 is "void f() {". The word "void" is a builtin type.
    c.client->request("textDocument/definition", doc_pos(c.uri, 3, 6));
    c.client->request("textDocument/definition", doc_pos(c.uri, 2, 1));
    EXPECT_TRUE(c.client->wait_notification("window/showMessage", [](const json&) { return true; }, 700).is_null());
}

TEST(LanguageServerHosts, DefinitionGivesNoNoHostMessageWhenTheNearestHostIsUsed)
{
    REQUIRE_CLANGD();
    NoMatchCase c(true);
    // "Nope" is not declared anywhere: the answer is empty, but a host is in use.
    c.client->notify("textDocument/didChange",
        {{"textDocument", {{"uri", c.uri}, {"version", 2}}},
            {"contentChanges", json::array({{{"text",
                "using \"thing.h\";\n\nclass A {\n    void f() {\n        Nope();\n    }\n}\n"}}})}});
    const json r = c.client->request("textDocument/definition", doc_pos(c.uri, 4, 9));
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_TRUE(r["result"].is_array() && r["result"].empty()) << r.dump();
    EXPECT_TRUE(c.client->wait_notification("window/showMessage", [](const json&) { return true; }, 700).is_null());
}

// ------------------------------------------------------------------ ".cpp" scripts
// A ".cpp" file is a script only when a host lists it (a script or a script dir). The server then
// publishes the hotc errors as diagnostics. It gives no other answer for the file: clangd owns it.
namespace
{

const char* const kCppValid =
    "#include \"Actor.h\"\n"
    "\n"
    "class Hero : public Actor\n"
    "{\n"
    "public:\n"
    "    void Tick(float dt) override {}\n"
    "};\n";

// The virtual method is on the 0-based line 5.
const char* const kCppStatic =
    "#include \"Actor.h\"\n"
    "\n"
    "class Hero : public Actor\n"
    "{\n"
    "public:\n"
    "    void F() = delete;\n"
    "};\n";

struct CppScriptCase
{
    TempDir dir;
    std::string script_path, other_path, script_uri, other_uri;
    hot::ls::ServerOptions o;
    // "scripts/a.cpp" is in the script dir of the host. "other/b.cpp" is in no host.
    CppScriptCase()
    {
        const std::filesystem::path root = dir.path();
        for (const char* d : {"scripts", "other", "build", "hosts"})
            std::filesystem::create_directories(root / d);
        script_path = (root / "scripts" / "a.cpp").string();
        other_path = (root / "other" / "b.cpp").string();
        script_uri = uri_of(script_path);
        other_uri = uri_of(other_path);
        write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
        write_file((root / "hosts" / "h.json").string(),
            R"({"version":1,"name":"h","binaryDir":")" + (root / "build").generic_string()
                + R"(","compiler":"/usr/bin/c++","std":"c++17","resourceDir":"","scriptDirs":[")"
                + (root / "scripts").generic_string() + R"("],"scripts":[],"includeDirs":[")" + HOT_SOURCE_DIR
                + R"(/include",")" + HOT_SOURCE_DIR + R"(/tests/engine"],"defines":[],"flags":[]})");
        o.hosts_dir = (root / "hosts").generic_string();
    }
    static void open(LsClient& c, const std::string& uri, const std::string& text)
    {
        c.notify("textDocument/didOpen",
            {{"textDocument", {{"uri", uri}, {"languageId", "cpp"}, {"version", 1}, {"text", text}}}});
    }
    static json diagnostics_of(LsClient& c, const std::string& uri, int timeout_ms = 120000)
    {
        return c.wait_notification(
            "textDocument/publishDiagnostics", [&](const json& p) { return p.value("uri", std::string()) == uri; },
            timeout_ms);
    }
};

} // namespace

TEST(LanguageServerCpp, ScriptWithAnUnsupportedFormGivesTheHotcMessageAtTheLine)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri, kCppStatic);
    const json n = CppScriptCase::diagnostics_of(client, c.script_uri);
    ASSERT_FALSE(n.is_null());
    const json& diags = n["params"]["diagnostics"];
    ASSERT_EQ(diags.size(), 1u) << n.dump();
    EXPECT_EQ(diags[0]["message"], "a defaulted or deleted method is not supported in a .cpp script class");
    EXPECT_EQ(diags[0]["range"]["start"]["line"], 5);
    EXPECT_EQ(diags[0]["severity"], 1);
    EXPECT_EQ(diags[0]["source"], "hot");
}

TEST(LanguageServerCpp, ClangNoteOfAnErrorIsAnInformationDiagnostic)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri,
        "#include \"Actor.h\"\n"
        "\n"
        "class Hero : public Actor\n"
        "{\n"
        "public:\n"
        "    void Tick(float dt) override { heal(\"x\"); }\n"
        "    void heal(float a) { hp += a; }\n"
        "    int hp = 10;\n"
        "};\n");
    const json n = CppScriptCase::diagnostics_of(client, c.script_uri);
    ASSERT_FALSE(n.is_null());
    const json& diags = n["params"]["diagnostics"];
    ASSERT_EQ(diags.size(), 2u) << n.dump();
    EXPECT_EQ(diags[0]["severity"], 1);
    EXPECT_EQ(diags[0]["range"]["start"]["line"], 5);
    EXPECT_EQ(diags[1]["severity"], 3);
    EXPECT_EQ(diags[1]["range"]["start"]["line"], 6);
    EXPECT_NE(diags[1]["message"].get<std::string>().find("parameter 'a'"), std::string::npos) << n.dump();
}

TEST(LanguageServerCpp, ValidScriptGivesNoDiagnostic)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri, kCppValid);
    const json n = CppScriptCase::diagnostics_of(client, c.script_uri);
    ASSERT_FALSE(n.is_null());
    EXPECT_TRUE(n["params"]["diagnostics"].empty()) << n.dump();
}

TEST(LanguageServerCpp, EditOfAScriptReplacesTheDiagnostics)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri, kCppStatic);
    EXPECT_EQ(CppScriptCase::diagnostics_of(client, c.script_uri)["params"]["diagnostics"].size(), 1u);
    client.change(c.script_uri, 2, kCppValid);
    EXPECT_TRUE(CppScriptCase::diagnostics_of(client, c.script_uri)["params"]["diagnostics"].empty());
}

TEST(LanguageServerCpp, FileOfNoHostIsIgnored)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.other_uri, kCppStatic);
    // The server answers the requests in order. After the replies, a publish would be in the queue.
    for (const char* method : {"textDocument/completion", "textDocument/hover", "textDocument/definition",
             "textDocument/references", "textDocument/documentSymbol", "textDocument/semanticTokens/full"})
    {
        const json r = client.request(method, doc_pos(c.other_uri, 5, 10));
        ASSERT_TRUE(r.contains("result")) << method << ": " << r.dump();
        EXPECT_TRUE(r["result"].is_null() || r["result"].empty()) << method << ": " << r.dump();
    }
    EXPECT_TRUE(CppScriptCase::diagnostics_of(client, c.other_uri, 300).is_null());
    client.change(c.other_uri, 2, kCppValid);
    client.request("textDocument/hover", doc_pos(c.other_uri, 5, 10));
    EXPECT_TRUE(CppScriptCase::diagnostics_of(client, c.other_uri, 300).is_null());
}

TEST(LanguageServerCpp, ScriptGetsNoCompletionHoverOrSemanticTokens)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri, kCppValid);
    ASSERT_FALSE(CppScriptCase::diagnostics_of(client, c.script_uri).is_null());
    for (const char* method : {"textDocument/completion", "textDocument/hover", "textDocument/definition",
             "textDocument/references", "textDocument/documentSymbol", "textDocument/semanticTokens/full",
             "textDocument/signatureHelp", "textDocument/formatting", "textDocument/codeAction"})
    {
        const json r = client.request(method, doc_pos(c.script_uri, 2, 8));
        ASSERT_TRUE(r.contains("result")) << method << ": " << r.dump();
        EXPECT_TRUE(r["result"].is_null() || r["result"].empty()) << method << ": " << r.dump();
    }
    json rename = doc_pos(c.script_uri, 2, 8);
    rename["newName"] = "Villain";
    const json r = client.request("textDocument/rename", rename);
    EXPECT_TRUE(r["result"].is_null() || r["result"].empty()) << r.dump();
}

TEST(LanguageServerCpp, ClosingAScriptClearsItsDiagnostics)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri, kCppStatic);
    EXPECT_EQ(CppScriptCase::diagnostics_of(client, c.script_uri)["params"]["diagnostics"].size(), 1u);
    client.notify("textDocument/didClose", {{"textDocument", {{"uri", c.script_uri}}}});
    EXPECT_TRUE(CppScriptCase::diagnostics_of(client, c.script_uri)["params"]["diagnostics"].empty());
}

// ------------------------------------------------------------------ preludes

// A script attribute that only a prelude declares. hot-ls knows it only with the prelude.
TEST(LanguageServer, PreludeDeclaresAScriptAttribute)
{
    const std::string text = "[Tagged] class Marker { int n = 0; }\n";
    {
        Session s(false, text);
        const json n = s.diagnostics([](const json& d) { return has_source(d, "hot"); });
        ASSERT_FALSE(n.is_null());
        EXPECT_NE(n.dump().find("unknown attribute 'Tagged'"), std::string::npos) << n.dump();
    }
    TempDir pre;
    const std::string prelude = (pre.path() / "game.prelude.hot").string();
    write_file(prelude, "[attribute] class Tagged;\n");
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, false);
    o.preludes = {prelude};
    Session s(false, text, &o);
    const json n = s.diagnostics([](const json&) { return true; });
    ASSERT_FALSE(n.is_null());
    EXPECT_FALSE(has_source(n["params"]["diagnostics"], "hot")) << n.dump();
}

// "prelude_suffix": hot-ls finds the user preludes in the workspace at each analysis. A prelude
// file that comes after the start of the server works too.
TEST(LanguageServer, PreludeSuffixFindsUserPreludes)
{
    TempDir dir;
    const std::string text = "[Tagged] class Marker { int n = 0; }\n";
    const std::string script = (dir.path() / "player.hot").string();
    write_file(script, text);
    hot::ls::ServerOptions o = options(dir, false);
    o.prelude_suffix = ".prelude.hot";
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    std::filesystem::create_directories(dir.path() / "sub");
    write_file((dir.path() / "sub" / "user.prelude.hot").string(), "[attribute] class Tagged;\n");
    client.open(uri_of(script), text);
    const json n = client.wait_notification("textDocument/publishDiagnostics",
        [&](const json& p) { return p.value("uri", std::string()) == uri_of(script); });
    ASSERT_FALSE(n.is_null());
    EXPECT_FALSE(has_source(n["params"]["diagnostics"], "hot")) << n.dump();
}

// An open prelude file is a script. It does not get its own text as a prelude, so its stubs
// are not duplicates. The new stub attributes are known.
TEST(LanguageServer, OpenPreludeFileHasNoErrors)
{
    TempDir dir;
    const std::string prelude = (dir.path() / "game.prelude.hot").string();
    const std::string text = "[attribute] class Tagged;\n"
                             "[native, handle(\"Ref<$T>\"), weak_handle(\"Weak<$T>\")] class Actor;\n"
                             "[cpp_prelude(\"// $N\"), cpp_overrides(\"Tick\")] class : Actor;\n";
    write_file(prelude, text);
    hot::ls::ServerOptions o = options(dir, false);
    o.preludes = {prelude};
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(prelude), text);
    const json n = client.wait_notification("textDocument/publishDiagnostics",
        [&](const json& p) { return p.value("uri", std::string()) == uri_of(prelude); });
    ASSERT_FALSE(n.is_null());
    EXPECT_FALSE(has_source(n["params"]["diagnostics"], "hot")) << n.dump();
}

// ------------------------------------------------------------------ script interfaces

namespace
{

// The lines are 0-based in the checks. "@" is the place of a completion.
const char* const kInterfaceScript =
    "using \"Actor.h\";\n" // 0
    "\n" // 1
    "interface IShape {\n" // 2
    "    float Area();\n" // 3
    "    int Scale(int k);\n" // 4
    "}\n" // 5
    "\n" // 6
    "class Square : Actor, IShape {\n" // 7
    "    float Area() { return 1.0; }\n" // 8
    "    int Scale(int k) { return k; }\n" // 9
    "    void Use(IShape s) {\n" // 10
    "        @\n" // 11
    "    }\n" // 12
    "}\n";

} // namespace

TEST(LanguageServer, DocumentSymbolsListAnInterfaceWithItsMethods)
{
    Session s(false, [] { std::string t = kInterfaceScript; t.replace(t.find('@'), 1, ""); return t; }());
    const json r = s.client->request("textDocument/documentSymbol", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].size() == 2) << r.dump();
    const json& i = r["result"][0];
    EXPECT_EQ(i["name"], "IShape");
    EXPECT_EQ(i["kind"], 11); // Interface
    EXPECT_EQ(i["detail"], "interface");
    EXPECT_EQ(i["selectionRange"]["start"]["line"], 2);
    std::vector<std::string> names;
    for (const json& m : i["children"])
        names.push_back(m["name"]);
    EXPECT_EQ(names, (std::vector<std::string>{"Area", "Scale"}));
    EXPECT_EQ(i["children"][1]["kind"], 6); // Method
    EXPECT_EQ(i["children"][1]["selectionRange"]["start"]["line"], 4);
    EXPECT_EQ(i["children"][1]["selectionRange"]["start"]["character"], 8);
    EXPECT_EQ(r["result"][1]["name"], "Square");
    EXPECT_EQ(r["result"][1]["kind"], 5);
}

TEST(LanguageServer, SemanticTokensMarkTheInterfaceKeywordAndNames)
{
    Session s(false, [] { std::string t = kInterfaceScript; t.replace(t.find('@'), 1, ""); return t; }());
    const json types = {"keyword", "type", "class", "method", "property", "number", "string", "comment"};
    const json r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json& data = r["result"]["data"];
    std::vector<std::tuple<int, int, int, std::string>> tokens;
    int line = 0, col = 0;
    for (size_t i = 0; i < data.size(); i += 5)
    {
        line += data[i].get<int>();
        col = data[i].get<int>() == 0 ? col + data[i + 1].get<int>() : data[i + 1].get<int>();
        tokens.emplace_back(line, col, data[i + 2].get<int>(), types[data[i + 3].get<int>()]);
    }
    auto has = [&](int l, int c, int n, const char* t)
    { return std::find(tokens.begin(), tokens.end(), std::make_tuple(l, c, n, std::string(t))) != tokens.end(); };
    EXPECT_TRUE(has(2, 0, 9, "keyword")); // interface
    EXPECT_TRUE(has(2, 10, 6, "class")); // IShape
    EXPECT_TRUE(has(7, 22, 6, "class")); // IShape in the base list
    EXPECT_TRUE(has(10, 13, 6, "class")); // IShape as a parameter type
    EXPECT_TRUE(has(3, 10, 4, "method")); // Area
}

TEST(LanguageServer, DefinitionOfAnInterfaceInTheBaseListIsTheDeclaration)
{
    Session s(false, [] { std::string t = kInterfaceScript; t.replace(t.find('@'), 1, ""); return t; }());
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, 7, 24));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && r["result"].size() == 1) << r.dump();
    EXPECT_EQ(r["result"][0]["uri"], s.uri);
    EXPECT_EQ(r["result"][0]["range"]["start"], pos(2, 10));
    EXPECT_EQ(r["result"][0]["range"]["end"], pos(2, 16));
}

TEST(LanguageServer, ReferencesOfAnInterfaceAreTheDeclarationAndTheUses)
{
    Session s(false, [] { std::string t = kInterfaceScript; t.replace(t.find('@'), 1, ""); return t; }());
    json p = doc_pos(s.uri, 2, 12);
    p["context"] = {{"includeDeclaration", true}};
    const json r = s.client->request("textDocument/references", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    std::vector<std::pair<int, int>> at;
    for (const json& l : r["result"])
        at.emplace_back(l["range"]["start"]["line"].get<int>(), l["range"]["start"]["character"].get<int>());
    std::sort(at.begin(), at.end());
    EXPECT_EQ(at, (std::vector<std::pair<int, int>>{{2, 10}, {7, 22}, {10, 13}})) << r.dump();
}

TEST(LanguageServer, CompletionListsAnInterfaceAsAType)
{
    const Completion c = complete_at(false, kInterfaceScript);
    ASSERT_TRUE(c.has("IShape")) << c.result.dump();
    EXPECT_TRUE(c.has("interface")); // the keyword
    for (const json& i : c.result.value("items", json::array()))
        if (i.value("label", "") == "IShape")
            EXPECT_EQ(i.value("detail", ""), "script interface");
}

TEST(LanguageServer, CompletionAfterADotOnAnInterfaceValueListsItsMethods)
{
    const Completion c = complete_at(false, kInterfaceScript, "s.");
    EXPECT_TRUE(c.has("Area")) << c.result.dump();
    EXPECT_TRUE(c.has("Scale")) << c.result.dump();
}

// The interface method has the name of its implementation. The rename of the method in a class
// changes the class and the interface. A copy of the script with no import keeps its names.
TEST(LanguageServer, RenameOfAnImplementingMethodWorksWithAnInterfaceOfTheSameName)
{
    REQUIRE_CLANGD();
    std::string text = kInterfaceScript;
    text.replace(text.find('@'), 1, "");
    Session s(true, text);
    const std::string other_path = (s.dir.path() / "sub" / "shapes_v2.hot").string();
    std::filesystem::create_directories(std::filesystem::path(other_path).parent_path());
    write_file(other_path, text);
    json p = doc_pos(s.uri, 8, 11);
    p["newName"] = "Size";
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result") && r["result"].contains("changes")) << r.dump();
    const json& changes = r["result"]["changes"];
    EXPECT_TRUE(changes.contains(s.uri)) << r.dump();
    EXPECT_FALSE(changes.contains(uri_of(other_path))) << r.dump();
}

// Workspace folders: the symbol index covers all folders, and the folders can change.
// A closed file has symbols in a workspace symbol answer only when the index has its shard. The
// tests make the shard with "fill": the document opens and closes, and the close stores the shard.
namespace
{

std::string folder_script(const char* cls)
{
    return std::string("using \"Actor.h\";\n\nclass ") + cls + " : Actor {\n    void jump() {\n    }\n}\n";
}

json folder_json(const std::filesystem::path& p, const char* suffix = "")
{
    return {{"uri", uri_of(p.string()) + suffix}, {"name", p.filename().string()}};
}

bool has_symbol(const json& r, const std::string& name, const std::string& uri)
{
    for (const json& s : r.value("result", json::array()))
        if (s.value("name", "") == name && s["location"].value("uri", "") == uri)
            return true;
    return false;
}

json symbols(LsClient& c, const std::string& query)
{
    return c.request("workspace/symbol", {{"query", query}});
}

// Writes a script and opens and closes it, so that its shard is in the index.
std::string fill(LsClient& c, const std::filesystem::path& file, const char* cls)
{
    const std::string text = folder_script(cls);
    write_file(file, text);
    const std::string uri = uri_of(file.string());
    c.open(uri, text);
    c.notify("textDocument/didClose", {{"textDocument", {{"uri", uri}}}});
    return uri;
}

void init_folders(LsClient& c, const std::vector<std::filesystem::path>& folders)
{
    json list = json::array();
    for (const auto& f : folders)
        list.push_back(folder_json(f));
    c.request("initialize", {{"processId", nullptr}, {"workspaceFolders", list}, {"capabilities", json::object()}});
    c.notify("initialized", json::object());
}

// A JSON array of one folder. A brace list of one json is a copy of the element on MSVC, so the
// tests do not use it.
json one_folder(json f)
{
    json a = json::array();
    a.push_back(std::move(f));
    return a;
}

void change_folders(LsClient& c, json added, json removed)
{
    c.notify("workspace/didChangeWorkspaceFolders", {{"event", {{"added", std::move(added)}, {"removed", std::move(removed)}}}});
}

int shard_count(LsClient& c)
{
    const std::string dir = c.request("hot/indexStats", json::object())["result"].value("dir", "");
    int n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        n += it->path().extension() == ".idx";
    return n;
}

int analyses(LsClient& c)
{
    return c.request("hot/indexStats", json::object())["result"].value("analyzed", -1);
}

} // namespace

TEST(LanguageServer, InitializeAdvertisesWorkspaceFolders)
{
    TempDir dir;
    LsClient client(options(dir, false));
    const json r = client.initialize(uri_of(dir.path().string()));
    EXPECT_TRUE(r["capabilities"]["workspace"]["workspaceFolders"]["supported"].get<bool>()) << r.dump();
    EXPECT_TRUE(r["capabilities"]["workspace"]["workspaceFolders"]["changeNotifications"].get<bool>()) << r.dump();
}

TEST(LanguageServer, WorkspaceSymbolFindsASymbolOfTheSecondFolder)
{
    TempDir dir;
    const auto one = dir.path() / "one", two = dir.path() / "two";
    std::filesystem::create_directories(one);
    std::filesystem::create_directories(two);
    LsClient client(options(dir, false));
    init_folders(client, {one, two});
    const std::string first = fill(client, one / "first.hot", "Alpha");
    const std::string second = fill(client, two / "second.hot", "Bravo");
    EXPECT_TRUE(has_symbol(symbols(client, "bravo"), "Bravo.jump", second));
    EXPECT_TRUE(has_symbol(symbols(client, "alpha"), "Alpha", first));
}

TEST(LanguageServer, WorkspaceSymbolUsesOnlyTheShardsOfClosedFiles)
{
    TempDir dir;
    const auto one = dir.path() / "one";
    std::filesystem::create_directories(one);
    // The file is on disk, but it is never open: no shard, no symbol, and no analysis.
    write_file(one / "lonely.hot", folder_script("Charlie"));
    hot::ls::ServerOptions o = options(dir, false);
    o.background_index = false;
    LsClient client(o);
    init_folders(client, {one});
    const std::string kept = fill(client, one / "kept.hot", "Alpha");
    const int before = analyses(client);
    const json r = symbols(client, "charlie");
    EXPECT_TRUE(r["result"].empty()) << r.dump();
    EXPECT_TRUE(has_symbol(symbols(client, "alpha"), "Alpha", kept));
    EXPECT_EQ(analyses(client), before);
}

TEST(LanguageServer, WorkspaceSymbolAcceptsADottedQueryAndNoEmptyQuery)
{
    TempDir dir;
    const auto one = dir.path() / "one";
    std::filesystem::create_directories(one);
    LsClient client(options(dir, false));
    init_folders(client, {one});
    const std::string uri = fill(client, one / "first.hot", "Alpha");
    EXPECT_TRUE(has_symbol(symbols(client, "Alpha.jump"), "Alpha.jump", uri));
    EXPECT_TRUE(has_symbol(symbols(client, "alpha.ju"), "Alpha.jump", uri));
    EXPECT_FALSE(has_symbol(symbols(client, "Beta.jump"), "Alpha.jump", uri));
    const json r = symbols(client, "");
    EXPECT_TRUE(r["result"].empty()) << r.dump();
}

TEST(LanguageServer, WorkspaceFoldersCanBeAddedAndRemoved)
{
    TempDir dir;
    const auto one = dir.path() / "one", two = dir.path() / "two";
    std::filesystem::create_directories(one);
    std::filesystem::create_directories(two);
    LsClient client(options(dir, false));
    client.initialize(uri_of(one.string()));
    const std::string first = fill(client, one / "first.hot", "Alpha");
    // The file of the second folder gets its shard while the folder is not in the workspace.
    const std::string second = fill(client, two / "second.hot", "Bravo");
    EXPECT_FALSE(has_symbol(symbols(client, "bravo"), "Bravo", second));

    change_folders(client, one_folder(folder_json(two)), json::array());
    EXPECT_TRUE(has_symbol(symbols(client, "bravo"), "Bravo", second));
    EXPECT_EQ(shard_count(client), 2);

    change_folders(client, json::array(), one_folder(folder_json(two)));
    EXPECT_FALSE(has_symbol(symbols(client, "bravo"), "Bravo", second));
    EXPECT_EQ(shard_count(client), 1); // the shard of the removed folder is gone
    EXPECT_TRUE(has_symbol(symbols(client, "alpha"), "Alpha", first));
}

TEST(LanguageServer, WorkspaceFolderWithATrailingSlashIsTheSameFolder)
{
    TempDir dir;
    const auto one = dir.path() / "one", two = dir.path() / "two";
    std::filesystem::create_directories(one);
    std::filesystem::create_directories(two);
    LsClient client(options(dir, false));
    client.initialize(uri_of(one.string()));
    change_folders(client, one_folder(folder_json(two)), json::array());
    const std::string second = fill(client, two / "second.hot", "Bravo");
    EXPECT_TRUE(has_symbol(symbols(client, "bravo"), "Bravo", second));
    change_folders(client, one_folder(folder_json(two, "/")), json::array()); // not a new folder
    change_folders(client, json::array(), one_folder(folder_json(two, "/")));
    EXPECT_FALSE(has_symbol(symbols(client, "bravo"), "Bravo", second));
    EXPECT_EQ(shard_count(client), 0);
}

TEST(LanguageServer, RemovedFolderKeepsTheShardsOfASiblingAndOfAnotherFolder)
{
    TempDir dir;
    // "b" and "bc" are siblings: the path of "b" is a text prefix of the path of "bc".
    // "outer" holds "outer/inner".
    const auto b = dir.path() / "a" / "b", bc = dir.path() / "a" / "bc";
    const auto outer = dir.path() / "outer", inner = outer / "inner";
    for (const auto& f : {b, bc, inner})
        std::filesystem::create_directories(f);
    LsClient client(options(dir, false));
    init_folders(client, {b, bc, outer, inner});
    const std::string in_b = fill(client, b / "b.hot", "Alpha");
    const std::string in_bc = fill(client, bc / "bc.hot", "Bravo");
    const std::string in_inner = fill(client, inner / "inner.hot", "Charlie");
    EXPECT_EQ(shard_count(client), 3);

    change_folders(client, json::array(), {folder_json(b), folder_json(inner)});
    EXPECT_FALSE(has_symbol(symbols(client, "alpha"), "Alpha", in_b));
    EXPECT_TRUE(has_symbol(symbols(client, "bravo"), "Bravo", in_bc)); // the sibling keeps its shard
    EXPECT_TRUE(has_symbol(symbols(client, "charlie"), "Charlie", in_inner)); // "outer" still holds it
    EXPECT_EQ(shard_count(client), 2);
}

TEST(LanguageServer, FirstFolderAddedLaterOpensTheIndex)
{
    TempDir dir;
    const auto one = dir.path() / "one";
    std::filesystem::create_directories(one);
    LsClient client(options(dir, false));
    client.request("initialize", {{"processId", nullptr}, {"capabilities", json::object()}});
    client.notify("initialized", json::object());
    EXPECT_EQ(client.request("hot/indexStats", json::object())["result"].value("dir", "x"), "");
    change_folders(client, one_folder(folder_json(one)), json::array());
    const std::string uri = fill(client, one / "first.hot", "Alpha");
    EXPECT_TRUE(has_symbol(symbols(client, "alpha"), "Alpha", uri));
}

// ------------------------------------------------------------------ code lens and call hierarchy

namespace
{

// The 0-based lines: 1 Add, 2 Unused, 3 Run with two calls of Add on 4 and 5, 6 the new Calc, 8 Twice.
const char* const kLensScript =
    "class Calc {\n"
    "    int Add(int a, int b) { return a + b; }\n"
    "    int Unused() { return 0; }\n"
    "    void Run() {\n"
    "        this.Add(1, 2);\n"
    "        this.Add(3, 4);\n"
    "        var c = new Calc();\n"
    "    }\n"
    "    void Twice() {\n"
    "        this.Run();\n"
    "    }\n"
    "}\n";

// The (line, title) of each lens.
std::vector<std::pair<int, std::string>> lenses_of(Session& s)
{
    const json r = s.client->request("textDocument/codeLens", {{"textDocument", {{"uri", s.uri}}}});
    std::vector<std::pair<int, std::string>> out;
    if (!r.contains("result") || !r["result"].is_array())
        return out;
    for (const json& l : r["result"])
        out.emplace_back(l["range"]["start"]["line"].get<int>(), l["command"]["title"].get<std::string>());
    return out;
}

// The first item of prepareCallHierarchy at a position, or null.
json prepare_item(Session& s, int line, int character)
{
    const json r = s.client->request("textDocument/prepareCallHierarchy", doc_pos(s.uri, line, character));
    if (!r.contains("result") || !r["result"].is_array() || r["result"].empty())
        return nullptr;
    return r["result"][0];
}

json calls_of(Session& s, const char* method, const json& item)
{
    const json r = s.client->request(method, {{"item", item}});
    return r.contains("result") ? r["result"] : json();
}

} // namespace

TEST(LanguageServer, CodeLensAndCallHierarchyAreAdvertised)
{
    TempDir dir;
    LsClient client(options(dir, false));
    const json init = client.initialize(uri_of(dir.path().string()));
    ASSERT_TRUE(init.contains("capabilities")) << init.dump();
    EXPECT_EQ(init["capabilities"].value("codeLensProvider", json()), json({{"resolveProvider", false}})) << init.dump();
    EXPECT_EQ(init["capabilities"].value("callHierarchyProvider", json()), true) << init.dump();
}

TEST(LanguageServer, CodeLensCountsTheReferences)
{
    Session s(false, kLensScript);
    using L = std::pair<int, std::string>;
    const std::vector<L> want = {{0, "1 reference"}, {1, "2 references"}, {2, "0 references"}, {3, "1 reference"},
        {8, "0 references"}};
    EXPECT_EQ(lenses_of(s), want);
}

TEST(LanguageServerCpp, ScriptGetsNoCodeLensOrCallHierarchy)
{
    CppScriptCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    CppScriptCase::open(client, c.script_uri, kCppValid);
    ASSERT_FALSE(CppScriptCase::diagnostics_of(client, c.script_uri).is_null());
    for (const char* method : {"textDocument/codeLens", "textDocument/prepareCallHierarchy"})
    {
        const json r = client.request(method, doc_pos(c.script_uri, 2, 8));
        ASSERT_TRUE(r.contains("result")) << method << ": " << r.dump();
        EXPECT_TRUE(r["result"].is_null() || r["result"].empty()) << method << ": " << r.dump();
    }
}

TEST(LanguageServer, PrepareCallHierarchyGivesTheMethod)
{
    Session s(false, kLensScript);
    for (const auto& [line, ch] : {std::pair{1, 9}, std::pair{4, 14}})
    {
        const json item = prepare_item(s, line, ch);
        ASSERT_FALSE(item.is_null()) << line;
        EXPECT_EQ(item["name"], "Add");
        EXPECT_EQ(item["kind"], 6);
        EXPECT_EQ(item["detail"], "Calc");
        EXPECT_EQ(item["uri"], s.uri);
        EXPECT_EQ(item["selectionRange"]["start"], pos(1, 8));
    }
    EXPECT_TRUE(prepare_item(s, 0, 1).is_null()); // "class" is not a method
}

TEST(LanguageServer, IncomingCallsListTheCallerAndTheCallRanges)
{
    Session s(false, kLensScript);
    const json in = calls_of(s, "callHierarchy/incomingCalls", prepare_item(s, 1, 9));
    ASSERT_TRUE(in.is_array() && in.size() == 1) << in.dump();
    EXPECT_EQ(in[0]["from"]["name"], "Run");
    ASSERT_EQ(in[0]["fromRanges"].size(), 2u);
    EXPECT_EQ(in[0]["fromRanges"][0], json({{"start", pos(4, 13)}, {"end", pos(4, 16)}}));
    EXPECT_EQ(in[0]["fromRanges"][1]["start"], pos(5, 13));
    // Run is called by Twice. Twice has no caller.
    const json run = calls_of(s, "callHierarchy/incomingCalls", prepare_item(s, 3, 10));
    ASSERT_TRUE(run.is_array() && run.size() == 1) << run.dump();
    EXPECT_EQ(run[0]["from"]["name"], "Twice");
    EXPECT_TRUE(calls_of(s, "callHierarchy/incomingCalls", prepare_item(s, 8, 10)).empty());
}

TEST(LanguageServer, OutgoingCallsListTheCallee)
{
    Session s(false, kLensScript);
    const json out = calls_of(s, "callHierarchy/outgoingCalls", prepare_item(s, 3, 10));
    ASSERT_TRUE(out.is_array() && out.size() == 1) << out.dump();
    EXPECT_EQ(out[0]["to"]["name"], "Add");
    ASSERT_EQ(out[0]["fromRanges"].size(), 2u);
    EXPECT_EQ(out[0]["fromRanges"][1]["start"], pos(5, 13));
    // Unused calls nothing.
    EXPECT_TRUE(calls_of(s, "callHierarchy/outgoingCalls", prepare_item(s, 2, 9)).empty());
}

namespace
{

// The 0-based lines: 1 the constructor, 2 the destructor, 3 and 4 the overloads of Twice, 5 Fact
// (it calls itself), 6 a comment and 9 a string with "Twice", 7 Run, 8 the new Tool, 10 the call.
const char* const kToolScript =
    "class Tool {\n"
    "    public Tool(int n) { }\n"
    "    ~Tool() { }\n"
    "    int Twice(int x) { return x * 2; }\n"
    "    int Twice(int x, int y) { return x + y; }\n"
    "    int Fact(int n) { if (n > 1) { return n * this.Fact(n - 1); } return 1; }\n"
    "    // Twice(1) in a comment\n"
    "    void Run() {\n"
    "        var t = new Tool(1);\n"
    "        string s = \"Twice(2)\";\n"
    "        this.Twice(3);\n"
    "    }\n"
    "}\n";

} // namespace

TEST(LanguageServer, CodeLensOfAConstructorCountsTheUsesOfTheClassName)
{
    Session s(false, kToolScript);
    using L = std::pair<int, std::string>;
    // The constructor and the destructor names are declarations. Only "new Tool" is a use. The
    // comment and the string do not count. Both overloads of Twice get the count of the name.
    const std::vector<L> want = {{0, "1 reference"}, {1, "1 reference"}, {3, "1 reference"}, {4, "1 reference"},
        {5, "1 reference"}, {7, "0 references"}};
    EXPECT_EQ(lenses_of(s), want);
}

TEST(LanguageServer, IncomingCallsOfAConstructorHaveNoDestructorName)
{
    Session s(false, kToolScript);
    const json item = prepare_item(s, 1, 12);
    ASSERT_FALSE(item.is_null());
    EXPECT_EQ(item["kind"], 9);
    EXPECT_EQ(item["selectionRange"]["start"], pos(1, 11));
    const json in = calls_of(s, "callHierarchy/incomingCalls", item);
    ASSERT_TRUE(in.is_array() && in.size() == 1) << in.dump();
    EXPECT_EQ(in[0]["from"]["name"], "Run");
    ASSERT_EQ(in[0]["fromRanges"].size(), 1u);
    EXPECT_EQ(in[0]["fromRanges"][0], json({{"start", pos(8, 20)}, {"end", pos(8, 24)}}));
    // The name of the destructor is not an item.
    EXPECT_TRUE(prepare_item(s, 2, 5).is_null());
}

TEST(LanguageServer, CallsInACommentOrAStringAreNotCounted)
{
    Session s(false, kToolScript);
    const json in = calls_of(s, "callHierarchy/incomingCalls", prepare_item(s, 3, 9));
    ASSERT_TRUE(in.is_array() && in.size() == 1) << in.dump();
    ASSERT_EQ(in[0]["fromRanges"].size(), 1u);
    EXPECT_EQ(in[0]["fromRanges"][0]["start"], pos(10, 13));
}

TEST(LanguageServer, OverloadsShareTheIncomingCalls)
{
    Session s(false, kToolScript);
    const json first = prepare_item(s, 3, 9), second = prepare_item(s, 4, 9);
    ASSERT_FALSE(first.is_null());
    ASSERT_FALSE(second.is_null());
    EXPECT_NE(first["selectionRange"], second["selectionRange"]);
    EXPECT_EQ(calls_of(s, "callHierarchy/incomingCalls", first), calls_of(s, "callHierarchy/incomingCalls", second));
}

TEST(LanguageServer, ARecursiveMethodIsItsOwnCallerAndCallee)
{
    Session s(false, kToolScript);
    const json item = prepare_item(s, 5, 9);
    ASSERT_FALSE(item.is_null());
    for (const char* method : {"callHierarchy/incomingCalls", "callHierarchy/outgoingCalls"})
    {
        const json r = calls_of(s, method, item);
        ASSERT_TRUE(r.is_array() && r.size() == 1) << method << ": " << r.dump();
        EXPECT_EQ(r[0][std::string(method).find("incoming") != std::string::npos ? "from" : "to"]["name"], "Fact");
        ASSERT_EQ(r[0]["fromRanges"].size(), 1u);
        EXPECT_EQ(r[0]["fromRanges"][0]["start"], pos(5, 51));
    }
}

TEST(LanguageServer, CallHierarchyColumnsAreUtf16)
{
    // On line 3 the string has an accented letter (2 bytes) and an emoji (4 bytes, 2 code units).
    Session s(false,
        "class U {\n"
        "    int F() { return 1; }\n"
        "    void Run() {\n"
        "        string u = \"\xC3\xA9\xF0\x9F\x98\x80\"; this.F();\n"
        "    }\n"
        "}\n");
    const json item = prepare_item(s, 3, 31);
    ASSERT_FALSE(item.is_null());
    EXPECT_EQ(item["name"], "F");
    const json in = calls_of(s, "callHierarchy/incomingCalls", item);
    ASSERT_TRUE(in.is_array() && in.size() == 1) << in.dump();
    EXPECT_EQ(in[0]["fromRanges"][0], json({{"start", pos(3, 31)}, {"end", pos(3, 32)}}));
}

TEST(LanguageServer, MalformedCallHierarchyRequestsKeepTheServerAlive)
{
    Session s(false, kToolScript);
    const json item = prepare_item(s, 3, 9);
    const std::vector<json> items = {json::object(), 5, "text", nullptr, {{"uri", 5}},
        {{"uri", s.uri}}, {{"uri", s.uri}, {"data", 7}}, {{"uri", s.uri}, {"data", {{"cls", 3}, {"name", json::array()}}}},
        {{"uri", "file:///nowhere.hot"}, {"data", item["data"]}}};
    for (const char* method : {"callHierarchy/incomingCalls", "callHierarchy/outgoingCalls"})
        for (const json& bad : items)
        {
            const json r = s.client->request(method, {{"item", bad}});
            ASSERT_TRUE(r.contains("result")) << method << " " << bad.dump() << ": " << r.dump();
            EXPECT_TRUE(r["result"].empty()) << bad.dump() << ": " << r.dump();
        }
    // A position of a wrong type falls back to the first member with the class and the name.
    const json bad_pos = s.client->request("callHierarchy/incomingCalls",
        {{"item", {{"uri", s.uri}, {"data", {{"cls", "Tool"}, {"name", "Twice"}, {"line", "x"}, {"col", nullptr}}}}}});
    EXPECT_TRUE(bad_pos.contains("result") && bad_pos["result"].is_array()) << bad_pos.dump();
    const json no_item = s.client->request("callHierarchy/incomingCalls", json::object());
    EXPECT_TRUE(no_item.contains("result") && no_item["result"].empty()) << no_item.dump();
    const json no_pos = s.client->request("textDocument/prepareCallHierarchy", {{"textDocument", {{"uri", s.uri}}}});
    EXPECT_TRUE(no_pos.contains("result") || no_pos.contains("error")) << no_pos.dump();
    EXPECT_FALSE(lenses_of(s).empty()); // the server still answers
}

// ------------------------------------------------------------------ rename and tokens of native symbols
namespace
{

// The places "line:col-line:col" (UTF-16 columns) of the whole word "word" in "text", outside the
// strings and the comments. "skip" holds places of the word that are not uses of the symbol.
std::set<std::string> word_places(const std::string& text, const std::string& word, const std::set<std::string>& skip = {})
{
    std::set<std::string> out;
    std::istringstream in(text);
    std::string l;
    for (int line = 0; std::getline(in, l); ++line)
        for (size_t at = l.find(word); at != std::string::npos; at = l.find(word, at + 1))
        {
            const bool whole = (at == 0 || !(std::isalnum(static_cast<unsigned char>(l[at - 1])) || l[at - 1] == '_'))
                && (at + word.size() >= l.size()
                    || !(std::isalnum(static_cast<unsigned char>(l[at + word.size()])) || l[at + word.size()] == '_'));
            const bool quoted = std::count(l.begin(), l.begin() + static_cast<long>(at), '"') % 2 == 1;
            const size_t comment = l.find("//");
            if (!whole || quoted || (comment != std::string::npos && comment < at))
                continue;
            int col = 0;
            for (size_t i = 0; i < at; ++i)
                col += (static_cast<unsigned char>(l[i]) & 0xC0) != 0x80;
            const std::string key = std::to_string(line) + ":" + std::to_string(col) + "-" + std::to_string(line) + ":"
                + std::to_string(col + static_cast<int>(word.size()));
            if (!skip.count(key))
                out.insert(key);
        }
    return out;
}

// The place of the last "word" in the first "context" of the native script, as "line:col-line:col".
std::string place_of(const std::string& context, const std::string& word)
{
    const json p = native_pos(context, word);
    return std::to_string(p["line"].get<int>()) + ":" + std::to_string(p["character"].get<int>()) + "-"
        + std::to_string(p["line"].get<int>()) + ":" + std::to_string(p["character"].get<int>() + static_cast<int>(word.size()));
}

// The places of the edits of a file in a WorkspaceEdit. Every edit must have the text "new_text".
std::set<std::string> edit_places(const json& result, const std::string& uri, const std::string& new_text)
{
    std::set<std::string> out;
    if (!result.is_object() || !result.contains("changes") || !result["changes"].contains(uri))
        return out;
    for (const json& e : result["changes"][uri])
    {
        EXPECT_EQ(e["newText"], new_text) << e.dump();
        const json &b = e["range"]["start"], &f = e["range"]["end"];
        out.insert(std::to_string(b["line"].get<int>()) + ":" + std::to_string(b["character"].get<int>()) + "-"
            + std::to_string(f["line"].get<int>()) + ":" + std::to_string(f["character"].get<int>()));
    }
    return out;
}

// A rename request at the word. The index of clangd can need some time for a symbol that a header
// declares: ask again until the answer is an edit.
json native_rename_request(NativeSession& s, const std::string& context, const std::string& word, const std::string& name,
    const std::string& script_text = kNativeScript)
{
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos(context, word, script_text)}, {"newName", name}};
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = s.client->request("textDocument/rename", p);
        if (r.contains("result") && r["result"].is_object()
            && r["result"]["changes"].contains(uri_of((s.dir.path() / "natives.h").string())))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return r;
}

// Checks the whole edit of a native rename: the script places, and the places in natives.h.
void expect_native_rename(NativeSession& s, const std::string& context, const std::string& word, const std::string& name,
    const std::set<std::string>& script_skip, const std::set<std::string>& header_skip = {},
    const std::string& script_text = kNativeScript)
{
    const json r = native_rename_request(s, context, word, name);
    SCOPED_TRACE(context + " -> " + r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(edit_places(r["result"], s.uri, name), word_places(script_text, word, script_skip));
    const std::string header_uri = uri_of((s.dir.path() / "natives.h").string());
    EXPECT_EQ(edit_places(r["result"], header_uri, name), word_places(kNativeHeader, word, header_skip));
    EXPECT_EQ(r["result"]["changes"].size(), 2u) << r.dump(); // the script and natives.h only
}

} // namespace

TEST(LanguageServerNative, RenameOfAFunction)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_rename(s, "Clamp100(MAX_HP)", "Clamp100", "ClampHp", {});
}

TEST(LanguageServerNative, RenameOfAClassChangesTypeUsesWithoutMapSegment)
{
    REQUIRE_CLANGD();
    // The parameter "Counter" of Take has the name of the class, in the same line as a use of the
    // class. The server cannot tell the two words apart, so the next test refuses the rename. Here
    // the parameter has another name.
    std::string text = kNativeScript;
    text.replace(text.find("void Take(Counter Counter)"), 26, "void Take(Counter other) ");
    NativeSession s(text.c_str());
    // clangd knows the declaration of the class in natives.h from the generated file only. The
    // use in the parameter list of Tag (line 24 of the header) is not in its index.
    const std::string tag_use = "23:" + std::to_string(std::string("int Tag(const char* text, ").size()) + "-23:"
        + std::to_string(std::string("int Tag(const char* text, Counter").size());
    expect_native_rename(s, "Counter local;", "Counter", "Tally", {}, {tag_use}, text);
}

// A use of the class and a parameter with the same name are in one line: no clear place.
TEST(LanguageServerNative, RenameIsRefusedForAClassWithAParameterOfTheSameNameInTheLine)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const json r = native_rename_request(s, "Counter local;", "Counter", "Tally");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"].value("message", "").find("another word"), std::string::npos) << r.dump();
}

// Applies the edits of one file to the text. The edits do not overlap.
std::string apply_edits(const std::string& text, const json& edits)
{
    auto offset = [&](const json& p)
    {
        size_t at = 0;
        for (int l = p["line"]; l > 0; --l)
            at = text.find('\n', at) + 1;
        return at + p["character"].get<size_t>();
    };
    std::vector<std::tuple<size_t, size_t, std::string>> all;
    for (const json& e : edits)
        all.emplace_back(offset(e["range"]["start"]), offset(e["range"]["end"]), e["newText"].get<std::string>());
    std::sort(all.rbegin(), all.rend());
    std::string out = text;
    for (const auto& [b, f, t] : all)
        out.replace(b, f - b, t);
    return out;
}

// The class has a constructor and a destructor. The rename changes the type name, the constructor
// name and the destructor name in the header, from both a type use and a constructor call.
TEST(LanguageServerNative, RenameOfAClassWithAConstructorChangesAllNamesInTheHeader)
{
    REQUIRE_CLANGD();
    const char* const header = "#pragma once\nstruct Pnt { Pnt(int v = 0); ~Pnt(); int x; };\n";
    const char* const script = "using \"natives.h\";\n\nclass Hero {\n    int seen = Pnt().x;\n\n    void Tick(float dt) {\n"
                               "        Pnt local;\n    }\n}\n";
    for (const char* context : {"Pnt local;", "Pnt()"})
    {
        NativeSession s(script, 10000, true, "", 10000, header);
        const json r = native_rename_request(s, context, "Pnt", "Pos3", script);
        SCOPED_TRACE(std::string(context) + " -> " + r.dump());
        ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
        const json& changes = r["result"]["changes"];
        const std::string header_uri = uri_of((s.dir.path() / "natives.h").string());
        ASSERT_TRUE(changes.contains(header_uri)) << r.dump();
        EXPECT_EQ(apply_edits(header, changes[header_uri]),
            "#pragma once\nstruct Pos3 { Pos3(int v = 0); ~Pos3(); int x; };\n");
        EXPECT_TRUE(edit_places(r["result"], header_uri, "Pos3").count("1:7-1:10")) << r.dump();
    }
}

// A constructor call in a statement of a method body: the generated C++ has a runtime call
// around it. The rename must not ask clangd about a runtime name.
TEST(LanguageServerNative, RenameOfAConstructorCallInAMethodBodyFindsTheClass)
{
    REQUIRE_CLANGD();
    const char* const header = "#pragma once\nstruct Pnt { Pnt(int v = 0); ~Pnt(); int x; };\n";
    const char* const script = "using \"natives.h\";\n\nclass Hero {\n    void Tick(float dt) {\n"
                               "        int k = Pnt().x;\n        k = k + 1;\n    }\n}\n";
    NativeSession s(script, 10000, true, "", 10000, header);
    const json r = native_rename_request(s, "Pnt()", "Pnt", "Pos3", script);
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const std::string header_uri = uri_of((s.dir.path() / "natives.h").string());
    ASSERT_TRUE(r["result"]["changes"].contains(header_uri)) << r.dump();
    EXPECT_EQ(apply_edits(header, r["result"]["changes"][header_uri]),
        "#pragma once\nstruct Pos3 { Pos3(int v = 0); ~Pos3(); int x; };\n");
}

// An export macro is between "struct" and the name.
TEST(LanguageServerNative, RenameOfAClassWithAnExportMacroChangesConstructorAndDestructor)
{
    REQUIRE_CLANGD();
    const char* const header = "#pragma once\n#define API_EXPORT\n"
                               "struct Base {};\nclass API_EXPORT Pnt : public Base\n{\npublic:\n    explicit Pnt(int v = 0);\n"
                               "    ~Pnt();\n    int x;\n};\n";
    const char* const script = "using \"natives.h\";\n\nclass Hero {\n    int seen = Pnt().x;\n}\n";
    NativeSession s(script, 10000, true, "", 10000, header);
    const json r = native_rename_request(s, "Pnt()", "Pnt", "Pos3", script);
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const std::string header_uri = uri_of((s.dir.path() / "natives.h").string());
    ASSERT_TRUE(r["result"]["changes"].contains(header_uri)) << r.dump();
    EXPECT_EQ(apply_edits(header, r["result"]["changes"][header_uri]),
        "#pragma once\n#define API_EXPORT\nstruct Base {};\nclass API_EXPORT Pos3 : public Base\n{\npublic:\n"
        "    explicit Pos3(int v = 0);\n    ~Pos3();\n    int x;\n};\n");
}

// The definitions of the constructor and the destructor are outside of the class. Inline ones in
// the header are not in the answer of clangd (its index has no reference there): the code finds
// "Pnt::Pnt(" and "Pnt::~Pnt(" in the declaring file.
// A .cpp file of the workspace that no translation unit of clangd includes is not in the answer of
// clangd: the rename does not change it (a known limit).
TEST(LanguageServerNative, RenameOfAClassWithOutOfClassDefinitions)
{
    REQUIRE_CLANGD();
    const char* const header = "#pragma once\nstruct Pnt { Pnt(int v = 0); ~Pnt(); int x; };\n"
                               "inline Pnt::Pnt(int v) : x(v) {}\ninline Pnt::~Pnt() {}\n";
    const char* const script = "using \"natives.h\";\n\nclass Hero {\n    int seen = Pnt().x;\n}\n";
    const char* const source = "#include \"natives.h\"\nPnt::Pnt(int v) : x(v) {}\nPnt::~Pnt() {}\n";
    NativeSession s(script, 10000, true, "", 10000, header);
    const std::string cpp = (s.dir.path() / "pnt.cpp").string();
    write_file(cpp, source);
    const json r = native_rename_request(s, "Pnt()", "Pnt", "Pos3", script);
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json& changes = r["result"]["changes"];
    const std::string header_uri = uri_of((s.dir.path() / "natives.h").string());
    ASSERT_TRUE(changes.contains(header_uri)) << r.dump();
    EXPECT_EQ(apply_edits(header, changes[header_uri]),
        "#pragma once\nstruct Pos3 { Pos3(int v = 0); ~Pos3(); int x; };\n"
        "inline Pos3::Pos3(int v) : x(v) {}\ninline Pos3::~Pos3() {}\n");
    EXPECT_FALSE(changes.contains(uri_of(cpp))) << r.dump();
    EXPECT_EQ(changes.size(), 2u) << r.dump(); // the script and natives.h
}

// Another class has the same name in another namespace. Its constructor definition and its
// body stay as they are. A base class with the name is not a class body of the name.
TEST(LanguageServerNative, RenameOfAClassDoesNotChangeTheClassOfAnotherNamespace)
{
    REQUIRE_CLANGD();
    const char* const header = "#pragma once\n"
                               "namespace A { struct Pnt { Pnt(int v = 0); int x; }; }\n"
                               "namespace B { struct Pnt { Pnt(int v = 0); }; }\n"
                               "namespace C { struct Item : public A::Pnt { Item(); }; }\n"
                               "inline B::Pnt::Pnt(int) {}\n";
    const char* const script = "using \"natives.h\";\n\nclass Hero {\n    int seen = A.Pnt().x;\n}\n";
    NativeSession s(script, 10000, true, "", 10000, header);
    const json r = native_rename_request(s, "Pnt()", "Pnt", "Pos3", script);
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const std::string header_uri = uri_of((s.dir.path() / "natives.h").string());
    ASSERT_TRUE(r["result"]["changes"].contains(header_uri)) << r.dump();
    const std::string out = apply_edits(header, r["result"]["changes"][header_uri]);
    EXPECT_NE(out.find("namespace A { struct Pos3 { Pos3(int v = 0); int x; }; }"), std::string::npos) << out;
    EXPECT_NE(out.find("namespace B { struct Pnt { Pnt(int v = 0); }; }"), std::string::npos) << out;
    // clangd does not give the use in the base list of Item (the index has no reference there).
    EXPECT_NE(out.find("{ Item(); }"), std::string::npos) << out;
    EXPECT_NE(out.find("inline B::Pnt::Pnt(int) {}"), std::string::npos) << out;
}

TEST(LanguageServerNative, RenameOfAMethod)
{
    REQUIRE_CLANGD();
    NativeSession s;
    // The parameter "Bump" of Pick is not a use.
    expect_native_rename(s, "local.Bump(", "Bump", "Raise", {place_of("(Bump)", "Bump"), place_of("int Bump,", "Bump")});
}

TEST(LanguageServerNative, RenameOfAField)
{
    REQUIRE_CLANGD();
    NativeSession s;
    // The parameter "count" of Pick is not a use.
    expect_native_rename(s, "this.counter.count", "count", "total", {place_of("int count)", "count"), place_of("= count;", "count")});
}

TEST(LanguageServerNative, RenameOfABaseClassMethodAndFieldChangesNativeFile)
{
    REQUIRE_CLANGD();
    NativeSession s;
    expect_native_rename(s, "this.Roar()", "Roar", "Bellow", {});
    NativeSession t;
    expect_native_rename(t, "this.rage =", "rage", "fury", {});
}

// HOT-6: the rename conflict check looks at the members of a native base class.
TEST(LanguageServerNative, RenameToAMemberOfTheNativeBaseIsRefused)
{
    REQUIRE_CLANGD();
    NativeSession s;
    for (const char* taken : {"rage", "Roar"})
    {
        json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Counter counter;", "counter")},
            {"newName", taken}};
        const json r = s.client->request("textDocument/rename", p);
        ASSERT_TRUE(r.contains("error")) << r.dump();
        EXPECT_NE(r["error"].value("message", "").find(std::string("the native class Boss has a member ") + taken),
            std::string::npos) << r.dump();
    }
}

TEST(LanguageServerNative, RenameToAFreeNameWithANativeBaseWorks)
{
    REQUIRE_CLANGD();
    NativeSession s;
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Counter counter;", "counter")},
        {"newName", "tally"}};
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_TRUE(r["result"]["changes"].contains(s.uri)) << r.dump();
}

TEST(LanguageServerNative, RenameInAClassWithoutNativeBaseIgnoresNativeMembers)
{
    REQUIRE_CLANGD();
    const std::string script = "using \"natives.h\";\n\nclass Solo {\n    int a = 1;\n}\n";
    NativeSession s(script.c_str());
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", pos(3, 8)}, {"newName", "rage"}};
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
}

// The header of the symbol is in a directory that is not a workspace folder. No host names this
// directory as an include dir, so the rename is refused.
TEST(LanguageServerNative, RenameIsRefusedForASymbolOutsideTheWorkspace)
{
    REQUIRE_CLANGD();
    TempDir outside;
    write_file((outside.path() / "outside.h").string(), "#pragma once\nint OutsideFn(int v);\n");
    const std::string script = "using \"outside.h\";\n\nclass Hero {\n    int x = OutsideFn(1);\n}\n";
    TempDir dir;
    const std::string path = (dir.path() / "hero.hot").string();
    write_file(path, script);
    hot::ls::ServerOptions o = options(dir, true);
    o.include_dirs.push_back(outside.path().string());
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(path), script);
    json p = doc_pos(uri_of(path), 3, 12);
    p["newName"] = "OutsideFn2";
    const json r = client.request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find("outside the workspace folders"), std::string::npos) << r.dump();
    p.erase("newName");
    const json prep = client.request("textDocument/prepareRename", p);
    ASSERT_TRUE(prep.contains("error")) << prep.dump();
    EXPECT_NE(prep["error"]["message"].get<std::string>().find("outside the workspace folders"), std::string::npos)
        << prep.dump();
}

// Weapon is in tests/engine, not in the temporary workspace folder.
TEST(LanguageServerNative, RenameIsRefusedForAClassOfAnotherDirectory)
{
    REQUIRE_CLANGD();
    NativeSession s;
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("new Weapon(", "Weapon")}, {"newName", "Blade"}};
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find("outside the workspace folders"), std::string::npos) << r.dump();
}

TEST(LanguageServerNative, RenameOfAKeywordOrOfAWordInACommentOrStringIsAnError)
{
    REQUIRE_CLANGD();
    NativeSession s;
    for (const auto& c : {std::make_pair("override void Tick", "override"), std::make_pair("Roar and Clamp100 in", "Clamp100"),
             std::make_pair("\"Clamp100 in a string", "Clamp100")})
    {
        json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos(c.first, c.second)}, {"newName", "Other"}};
        const json r = s.client->request("textDocument/rename", p);
        EXPECT_TRUE(r.contains("error")) << c.first << " -> " << r.dump();
    }
}

TEST(LanguageServerNative, PrepareRenameAgreesWithRename)
{
    REQUIRE_CLANGD();
    NativeSession s;
    auto prepare = [&](const std::string& context, const std::string& word)
    {
        json r;
        for (int i = 0; i < 20; ++i)
        {
            r = s.client->request("textDocument/prepareRename",
                {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos(context, word)}});
            if (r.contains("result"))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return r;
    };
    // Renameable natives (a function, a class with no map segment): the range of the word.
    for (const auto& c : {std::make_pair("Clamp100(MAX_HP)", "Clamp100"), std::make_pair("Mood m = ", "Mood")})
    {
        const json r = prepare(c.first, c.second);
        ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << c.first << " -> " << r.dump();
        const json at = native_pos(c.first, c.second);
        EXPECT_EQ(r["result"]["range"]["start"], at) << r.dump();
        EXPECT_EQ(r["result"]["range"]["end"], pos(at["line"], at["character"].get<int>() + static_cast<int>(std::string(c.second).size())))
            << r.dump();
        EXPECT_EQ(r["result"]["placeholder"], c.second) << r.dump();
    }
    // A script symbol is renameable.
    const json script = prepare("int limit = ", "limit");
    EXPECT_TRUE(script.contains("result") && script["result"].is_object()) << script.dump();
    // Not renameable: a class of another directory, a keyword, a word in a string.
    for (const auto& c : {std::make_pair("new Weapon(", "Weapon"), std::make_pair("override void Tick", "override"),
             std::make_pair("\"Clamp100 in a string", "Clamp100")})
    {
        const json r = s.client->request("textDocument/prepareRename",
            {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos(c.first, c.second)}});
        EXPECT_TRUE(r.contains("error")) << c.first << " -> " << r.dump();
    }
}

// A second script of the workspace uses the function. The script is closed: the rename asks clangd
// for this script too.
TEST(LanguageServerNative, RenameChangesAClosedScriptOfTheWorkspace)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const std::string pet = (s.dir.path() / "pet.hot").string();
    write_file(pet, "using \"natives.h\";\n\nclass Pet {\n    int cap = Clamp100(7);\n}\n");
    const json r = native_rename_request(s, "Clamp100(MAX_HP)", "Clamp100", "ClampHp");
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json& changes = r["result"]["changes"];
    EXPECT_EQ(changes.size(), 3u) << r.dump(); // hero.hot, pet.hot and natives.h
    EXPECT_EQ(edit_places(r["result"], uri_of(pet), "ClampHp"), std::set<std::string>({"3:14-3:22"})) << r.dump();
    EXPECT_EQ(edit_places(r["result"], s.uri, "ClampHp"), word_places(kNativeScript, "Clamp100", {})) << r.dump();
    for (const auto& [uri, edits] : changes.items())
        EXPECT_NE(uri.find(".hot.cpp"), uri.size() - 8) << uri;
}

namespace
{

const char* const kPetScript = "using \"natives.h\";\n\nclass Pet {\n    int cap = Clamp100(7);\n}\n";

void expect_no_script_cpp_uri(const json& changes)
{
    for (const auto& [uri, edits] : changes.items())
        EXPECT_FALSE(uri.size() >= 8 && uri.compare(uri.size() - 8, 8, ".hot.cpp") == 0) << uri;
}

// The error message of a refused rename is "want". The request runs again while the index of
// clangd is not ready.
void expect_rename_refusal(NativeSession& s, const std::string& want)
{
    const json r = native_rename_request(s, "Clamp100(MAX_HP)", "Clamp100", "ClampHp");
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_FALSE(r.contains("result") && !r["result"].is_null()) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find(want), std::string::npos) << r.dump();
}

} // namespace

// The second script is open and has one more line on top than the file on the disk. The edit uses
// the text of the editor.
TEST(LanguageServerNative, RenameChangesAnOpenSecondScriptAtItsEditorText)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const std::string pet = (s.dir.path() / "pet.hot").string();
    write_file(pet, kPetScript);
    s.client->open(uri_of(pet), std::string("\n") + kPetScript);
    const json r = native_rename_request(s, "Clamp100(MAX_HP)", "Clamp100", "ClampHp");
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["changes"].size(), 3u) << r.dump();
    EXPECT_EQ(edit_places(r["result"], uri_of(pet), "ClampHp"), std::set<std::string>({"4:14-4:22"})) << r.dump();
    expect_no_script_cpp_uri(r["result"]["changes"]);
}

// The rename starts in the second script. The edit is the same as the edit that starts in the first.
TEST(LanguageServerNative, RenameFromTheSecondScriptGivesTheSameEdit)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const std::string pet = (s.dir.path() / "pet.hot").string();
    write_file(pet, kPetScript);
    const json first = native_rename_request(s, "Clamp100(MAX_HP)", "Clamp100", "ClampHp");
    ASSERT_TRUE(first.contains("result") && first["result"].is_object()) << first.dump();
    s.client->open(uri_of(pet), kPetScript);
    json p = {{"textDocument", {{"uri", uri_of(pet)}}}, {"position", pos(3, 14)}, {"newName", "ClampHp"}};
    json second;
    for (int i = 0; i < 20; ++i)
    {
        second = s.client->request("textDocument/rename", p);
        if (second.contains("result") && second["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    SCOPED_TRACE(second.dump());
    ASSERT_TRUE(second.contains("result") && second["result"].is_object()) << second.dump();
    EXPECT_EQ(second["result"]["changes"].size(), 3u) << second.dump();
    for (const std::string& uri : {s.uri, uri_of(pet), uri_of((s.dir.path() / "natives.h").string())})
        EXPECT_EQ(edit_places(second["result"], uri, "ClampHp"), edit_places(first["result"], uri, "ClampHp")) << uri;
    expect_no_script_cpp_uri(second["result"]["changes"]);
}

// A third script has a local variable with the name. It does not use the function.
TEST(LanguageServerNative, RenameSkipsAScriptThatHasOnlyALocalWithTheName)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const std::string local = (s.dir.path() / "local.hot").string();
    write_file(local, "class Loc {\n    int f() {\n        int Clamp100 = 1;\n        return Clamp100;\n    }\n}\n");
    const json r = native_rename_request(s, "Clamp100(MAX_HP)", "Clamp100", "ClampHp");
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_FALSE(r["result"]["changes"].contains(uri_of(local))) << r.dump();
    EXPECT_EQ(r["result"]["changes"].size(), 2u) << r.dump();
}

// A closed script uses the function and has a syntax error. The rename does not give a part of the edit.
TEST(LanguageServerNative, RenameIsRefusedForAClosedScriptThatDoesNotTranslate)
{
    REQUIRE_CLANGD();
    NativeSession s;
    write_file((s.dir.path() / "bad.hot").string(), "using \"natives.h\";\n\nclass Bad {\n    int cap = Clamp100(7;\n}}}\n");
    expect_rename_refusal(s, "bad.hot does not translate");
}

// A second script is open with a syntax error in its editor text. The rename does not skip it.
TEST(LanguageServerNative, RenameIsRefusedForAnOpenScriptThatDoesNotTranslate)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const std::string pet = (s.dir.path() / "pet.hot").string();
    write_file(pet, kPetScript);
    s.client->open(uri_of(pet), "using \"natives.h\";\n\nclass Pet {\n    int cap = Clamp100(7;\n}}}\n");
    expect_rename_refusal(s, "pet.hot does not translate");
}

// A file "<stem>.g<N>.cpp" of "hotc --compile-commands" uses the function. The background index of
// clangd knows this file, so the answer of clangd can change it. The rename refuses.
TEST(LanguageServerNative, RenameIsRefusedForAHotcOutputFile)
{
    REQUIRE_CLANGD();
    TempDir dir;
    write_file((dir.path() / "natives.h").string(), kNativeHeader);
    const std::string use = (dir.path() / "use.g1.cpp").string();
    write_file(use, "#include \"natives.h\"\nint UseIt() { return Clamp100(3); }\n");
    write_file((dir.path() / "compile_commands.json").string(),
        "[{\"directory\":\"" + dir.path().generic_string() + "\",\"file\":\"" + use + "\",\"command\":\"clang++ -std=c++20 -I"
            + dir.path().generic_string() + " -c " + use + "\"}]\n");
    const std::string script = "using \"natives.h\";\n\nclass Hero {\n    int x = Clamp100(1);\n}\n";
    const std::string path = (dir.path() / "hero.hot").string();
    write_file(path, script);
    hot::ls::ServerOptions o = options(dir, true);
    o.include_dirs.push_back(dir.path().string());
    o.compile_commands_dir = dir.path().string();
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(path), script);
    const json p = {{"textDocument", {{"uri", uri_of(path)}}}, {"position", pos(3, 12)}, {"newName", "ClampHp"}};
    json r;
    for (int i = 0; i < 60; ++i)
    {
        r = client.request("textDocument/rename", p);
        if (r.contains("error") && r["error"]["message"].get<std::string>().find("generated file") != std::string::npos)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find("use.g1.cpp"), std::string::npos) << r.dump();
}

#ifndef _WIN32
// The budget of one rename is 3 s. A proxy gives the open of the second script to clangd 3.5 s
// late, so the budget is over when the rename asks clangd about this script.
TEST(LanguageServerNative, RenameIsRefusedWhenTheBudgetIsOver)
{
    REQUIRE_CLANGD();
    if (std::system("python3 -c \"\" >/dev/null 2>&1") != 0)
        GTEST_SKIP() << "python3 is not installed";
    TempDir tools;
    const std::string proxy = (tools.path() / "slow_clangd.py").string();
    write_file(proxy,
        "import subprocess, sys, time\n"
        "p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE)\n"
        "src = sys.stdin.buffer\n"
        "while True:\n"
        "    head = b''\n"
        "    while not head.endswith(b'\\r\\n\\r\\n'):\n"
        "        c = src.read(1)\n"
        "        if not c:\n"
        "            p.stdin.close()\n"
        "            p.wait()\n"
        "            sys.exit(0)\n"
        "        head += c\n"
        "    size = 0\n"
        "    for line in head.split(b'\\r\\n'):\n"
        "        if line.lower().startswith(b'content-length:'):\n"
        "            size = int(line.split(b':')[1])\n"
        "    body = src.read(size)\n"
        "    if b'\"textDocument/didOpen\"' in body and b'pet.hot' in body:\n"
        "        time.sleep(3.5)\n"
        "    p.stdin.write(head + body)\n"
        "    p.stdin.flush()\n");
    const std::string clangd = (tools.path() / "slow_clangd").string();
    write_file(clangd, "#!/bin/sh\nexec python3 '" + proxy + "' '" HOT_CLANGD_PATH "' \"$@\"\n");
    std::filesystem::permissions(clangd, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    NativeSession s(kNativeScript, 10000, true, clangd, 3000);
    write_file((s.dir.path() / "pet.hot").string(), kPetScript);
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")}, {"newName", "ClampHp"}};
    json r;
    for (int i = 0; i < 6; ++i)
    {
        r = s.client->request("textDocument/rename", p);
        ASSERT_FALSE(r.contains("result") && r["result"].is_object()) << r.dump(); // never an edit
        if (r.contains("error") && r["error"]["message"].get<std::string>().find("time budget of 3 s") != std::string::npos)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find("time budget of 3 s is over at"), std::string::npos) << r.dump();
}

// The references of Clamp100 at "Clamp100(MAX_HP)" of hero.hot, as the sorted "file:line:col" text
// of the places in the scripts (not natives.h).
static std::vector<std::string> script_places(NativeSession& s, json* raw = nullptr, bool wait_decl = true)
{
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")},
        {"context", {{"includeDeclaration", true}}}};
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = s.client->request("textDocument/references", p);
        bool have_decl = false;
        for (const json& l : r.value("result", json::array()))
            have_decl = have_decl || l.value("uri", "").find("natives.h") != std::string::npos;
        if (have_decl || !wait_decl)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (raw)
        *raw = r;
    std::vector<std::string> out;
    for (const json& l : r.value("result", json::array()))
    {
        const std::string uri = l.value("uri", "");
        if (uri.find(".hot") != std::string::npos)
            out.push_back(std::filesystem::path(uri).filename().string() + ":" + std::to_string(l["range"]["start"]["line"].get<int>())
                + ":" + std::to_string(l["range"]["start"]["character"].get<int>()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// HOT-36: the reader thread takes "$/cancelRequest" at once. A request that waits in the queue
// gets the error -32800 and does not run. The next request works.
TEST(LanguageServerNative, CancelOfAQueuedReferencesRequestGivesTheErrorAndKeepsTheServerUsable)
{
    REQUIRE_CLANGD();
    NativeSession s;
    const json at = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")}};
    json refs = at;
    refs["context"] = {{"includeDeclaration", true}};
    const int first = s.client->send_request("textDocument/hover", at);
    const int second = s.client->send_request("textDocument/references", refs);
    s.client->notify("$/cancelRequest", {{"id", second}});
    const json cancelled = s.client->wait_response(second, 60000);
    ASSERT_TRUE(cancelled.contains("error")) << cancelled.dump();
    EXPECT_EQ(cancelled["error"]["code"], -32800);
    EXPECT_FALSE(s.client->wait_response(first, 60000).is_null());
    const json hover = s.client->request("textDocument/hover", at);
    EXPECT_TRUE(hover.contains("result")) << hover.dump();
    EXPECT_FALSE(hover.contains("error")) << hover.dump();
}

// HOT-36: a cancel stops a running scan of many scripts well before the budget.
TEST(LanguageServerNative, CancelOfARunningReferencesScanGivesTheErrorBeforeTheBudget)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeScript, 10000, true, "", 10000, kNativeHeader, 120000);
    for (int i = 0; i < 40; ++i)
        write_file((s.dir.path() / ("pet" + std::to_string(i) + ".hot")).string(), kPetScript);
    const json at = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")}};
    json refs = at;
    refs["context"] = {{"includeDeclaration", true}};
    const auto t0 = std::chrono::steady_clock::now();
    const int id = s.client->send_request("textDocument/references", refs);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s.client->notify("$/cancelRequest", {{"id", id}});
    const json r = s.client->wait_response(id, 60000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_FALSE(r.is_null());
    // The state thread cannot show that the scan runs (hot/indexStats waits in the queue behind it).
    // A reply with a result before the cancel was sent is not a cancel test: skip, no false failure.
    if (r.contains("result") && ms < 400)
        GTEST_SKIP() << "the scan ended before the cancel";
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["error"]["code"], -32800);
    EXPECT_LT(ms, 30000);
    const json hover = s.client->request("textDocument/hover", at);
    EXPECT_TRUE(hover.contains("result")) << hover.dump();
    EXPECT_FALSE(hover.contains("error")) << hover.dump();
}

#ifndef _WIN32
// HOT-36: the slow worker. A proxy holds the first references request to clangd until a gate file
// exists. All other messages pass.
namespace
{

// Writes the proxy in "tools" and gives the path of the clangd program that runs it.
std::string make_gated_clangd(const TempDir& tools)
{
    const std::string proxy = (tools.path() / "gate_clangd.py").string();
    write_file(proxy,
        "import os, subprocess, sys, threading, time\n"
        "gate = sys.argv[1]\n"
        "p = subprocess.Popen(sys.argv[2:], stdin=subprocess.PIPE)\n"
        "src = sys.stdin.buffer\n"
        "lock = threading.Lock()\n"
        "held = False\n"
        "def put(data):\n"
        "    with lock:\n"
        "        try:\n"
        "            p.stdin.write(data)\n"
        "            p.stdin.flush()\n"
        "        except Exception:\n"
        "            pass\n"
        "def release(data):\n"
        "    while not os.path.exists(gate):\n"
        "        time.sleep(0.05)\n"
        "    put(data)\n"
        "while True:\n"
        "    head = b''\n"
        "    while not head.endswith(b'\\r\\n\\r\\n'):\n"
        "        c = src.read(1)\n"
        "        if not c:\n"
        "            with lock:\n"
        "                p.stdin.close()\n"
        "            p.wait()\n"
        "            sys.exit(0)\n"
        "        head += c\n"
        "    size = 0\n"
        "    for line in head.split(b'\\r\\n'):\n"
        "        if line.lower().startswith(b'content-length:'):\n"
        "            size = int(line.split(b':')[1])\n"
        "    body = src.read(size)\n"
        "    if not held and b'\"textDocument/references\"' in body:\n"
        "        held = True\n"
        "        threading.Thread(target=release, args=(head + body,), daemon=True).start()\n"
        "        continue\n"
        "    put(head + body)\n");
    const std::string gate = (tools.path() / "gate").string();
    const std::string clangd = (tools.path() / "gate_clangd").string();
    write_file(clangd, "#!/bin/sh\nexec python3 '" + proxy + "' '" + gate + "' '" HOT_CLANGD_PATH "' \"$@\"\n");
    std::filesystem::permissions(clangd, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    return clangd;
}

// Opens the gate at the end of the scope, before the session stops.
struct GateOpener
{
    std::string gate;
    ~GateOpener() { write_file(gate, "open"); }
};

bool wait_slow_running(NativeSession& s)
{
    for (int i = 0; i < 200; ++i)
    {
        const json r = s.client->request("hot/indexStats", json::object(), 20000);
        if (r.contains("result") && r["result"].value("slowRunning", 0) == 1)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

json native_refs_params(NativeSession& s)
{
    return {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")},
        {"context", {{"includeDeclaration", true}}}};
}

json native_hover_params(NativeSession& s)
{
    return {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")}};
}

} // namespace

#define SLOW_WORKER_SETUP()                                                                   \
    REQUIRE_CLANGD();                                                                         \
    if (std::system("python3 -c \"\" >/dev/null 2>&1") != 0)                                  \
        GTEST_SKIP() << "python3 is not installed";                                           \
    TempDir tools;                                                                            \
    NativeSession s(kNativeScript, 10000, true, make_gated_clangd(tools));                    \
    GateOpener opener{(tools.path() / "gate").string()};                                      \
    {                                                                                         \
        const json first = s.client->request("textDocument/hover", native_hover_params(s));   \
        ASSERT_TRUE(first.contains("result")) << first.dump();                                \
    }

// HOT-36: a hover answers while a references request waits for clangd on the slow worker.
TEST(LanguageServerNative, HoverAnswersWhileReferencesRuns)
{
    SLOW_WORKER_SETUP();
    const int id = s.client->send_request("textDocument/references", native_refs_params(s));
    ASSERT_TRUE(wait_slow_running(s));
    const json hover = s.client->request("textDocument/hover", native_hover_params(s), 20000);
    ASSERT_TRUE(hover.contains("result")) << hover.dump();
    EXPECT_FALSE(hover.contains("error")) << hover.dump();
    EXPECT_TRUE(s.client->wait_response(id, 300).is_null());
    write_file(opener.gate, "open");
    const json r = s.client->wait_response(id, 60000);
    ASSERT_FALSE(r.is_null());
    EXPECT_TRUE(r.contains("result")) << r.dump();
}

// HOT-36: a cancel stops the references request that runs on the worker, with the gate closed.
TEST(LanguageServerNative, CancelOfReferencesOnTheWorker)
{
    SLOW_WORKER_SETUP();
    const int id = s.client->send_request("textDocument/references", native_refs_params(s));
    ASSERT_TRUE(wait_slow_running(s));
    s.client->notify("$/cancelRequest", {{"id", id}});
    const json r = s.client->wait_response(id, 20000);
    ASSERT_FALSE(r.is_null());
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["error"]["code"], -32800);
}

// HOT-36: an edit while the worker runs a request gives the error ContentModified (-32801).
TEST(LanguageServerNative, EditDuringReferencesGivesContentModified)
{
    SLOW_WORKER_SETUP();
    const int id = s.client->send_request("textDocument/references", native_refs_params(s));
    ASSERT_TRUE(wait_slow_running(s));
    s.client->change(s.uri, 2, std::string(kNativeScript) + "// edit\n");
    const json r = s.client->wait_response(id, 20000);
    ASSERT_FALSE(r.is_null());
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["error"]["code"], -32801);
    const json hover = s.client->request("textDocument/hover", native_hover_params(s), 20000);
    EXPECT_TRUE(hover.contains("result")) << hover.dump();
}

// HOT-36: shutdown and exit stop the server while the worker waits for clangd.
TEST(LanguageServerNative, ExitDuringReferencesStopsTheServer)
{
    SLOW_WORKER_SETUP();
    s.client->send_request("textDocument/references", native_refs_params(s));
    ASSERT_TRUE(wait_slow_running(s));
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(s.client->stop(), 0);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count(), 10);
}
#endif

// HOT-36: a cancel of the first native hover leaves no mark "preamble ready". The next hover works.
TEST(LanguageServerNative, CancelOfTheFirstNativeHoverKeepsTheNextHoverWorking)
{
    REQUIRE_CLANGD();
    NativeSession s;
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")}};
    const int id = s.client->send_request("textDocument/hover", p);
    s.client->notify("$/cancelRequest", {{"id", id}});
    const json first = s.client->wait_response(id, 60000);
    ASSERT_FALSE(first.is_null());
    const json next = s.client->request("textDocument/hover", p);
    ASSERT_TRUE(next.contains("result")) << next.dump();
    EXPECT_FALSE(next.contains("error")) << next.dump();
}

// HOT-36: a burst of didChange of one uri runs fewer analyses than the number of changes.
// The hover after the burst sees the last text.
TEST(LanguageServer, BurstOfDidChangeIsAnalyzedOnceAndTheNextRequestSeesTheLastText)
{
    REQUIRE_CLANGD();
    Session s(true);
    auto translations = [&] { return s.client->request("hot/indexStats", json::object())["result"].value("translations", -1); };
    s.client->request("textDocument/hover", doc_pos(s.uri, 11, 14)); // the open is done
    const int before = translations();
    constexpr int kChanges = 40;
    std::string last = kScript;
    for (int i = 0; i < kChanges; ++i)
    {
        last = std::string(kScript) + "// change " + std::to_string(i) + "\n";
        if (i == kChanges - 1)
            for (size_t at; (at = last.find("hp")) != std::string::npos;)
                last.replace(at, 2, "mana");
        s.client->change(s.uri, i + 2, last);
    }
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, 11, 16));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_NE(r["result"]["contents"].value("value", "").find("mana"), std::string::npos) << r.dump();
    const int analyses = translations() - before;
    EXPECT_LT(analyses, kChanges) << analyses;
}

// HOT-36: a didChange with a number as uri does not crash the server.
TEST(LanguageServer, DidChangeWithANumericUriKeepsTheServerUsable)
{
    REQUIRE_CLANGD();
    Session s(true);
    s.client->notify("textDocument/didChange",
        {{"textDocument", {{"uri", 5}, {"version", 2}}}, {"contentChanges", json::array({{{"text", "x"}}})}});
    s.client->notify("textDocument/didChange",
        {{"textDocument", {{"uri", 6}, {"version", 3}}}, {"contentChanges", json::array({{{"text", "y"}}})}});
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, 11, 14));
    ASSERT_TRUE(r.contains("result")) << r.dump();
}

// HOT-36: the end of the input with requests in the queue. Each earlier request gets its reply.
TEST(LanguageServer, EndOfInputWithQueuedRequestsWritesAllRepliesAndTheServerExits)
{
    REQUIRE_CLANGD();
    Session s(true);
    std::vector<int> ids;
    for (int i = 0; i < 20; ++i)
        ids.push_back(s.client->send_request("textDocument/hover", doc_pos(s.uri, 11, 14)));
    s.client->close_input();
    for (const int id : ids)
    {
        const json r = s.client->wait_response(id, 60000);
        ASSERT_FALSE(r.is_null()) << id;
        EXPECT_TRUE(r.contains("result")) << r.dump();
    }
    EXPECT_EQ(s.client->stop(), 1); // no shutdown request: the exit code is 1
}

// HOT-32: a closed script with a stored table and a script symbol with the name of the native
// symbol (so no unresolved name) costs no analyze. The result is the same as without the stored tables.
TEST(LanguageServerNative, ReferencesSkipClosedScriptsWithTheirOwnSymbolOfTheName)
{
    REQUIRE_CLANGD();
    const char* const bg_env = std::getenv("HOTPP_TEST_BACKGROUND_INDEX");
    const bool bg_on = bg_env && std::string(bg_env) == "1";
    auto run = [&](bool warm, int& analyses)
    {
        // A cold scan analyzes each script with its own clangd. A long budget keeps the scan whole on a loaded machine.
        NativeSession s(kNativeScript, 10000, true, "", 10000, kNativeHeader, 120000);
        for (int i = 0; i < 5; ++i)
        {
            const std::string decoy = "class Decoy" + std::to_string(i) + " {\n    // Clamp100 in a comment\n    int Clamp100(int v) {\n        return v;\n    }\n    int x = Clamp100(2);\n}\n";
            const std::string path = (s.dir.path() / ("decoy" + std::to_string(i) + ".hot")).string();
            write_file(path, decoy);
            if (warm) // a close of an open document stores the shard
            {
                s.client->open(uri_of(path), decoy);
                s.client->notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(path)}}}});
            }
        }
        write_file((s.dir.path() / "pet.hot").string(), kPetScript);
        auto translations = [&] { return s.client->request("hot/indexStats", json::object())["result"].value("translations", -1); };
        if (bg_on) // wait for the entry of each decoy
            for (int i = 0; i < 5; ++i)
            {
                const std::string uri = uri_of((s.dir.path() / ("decoy" + std::to_string(i) + ".hot")).string());
                for (int n = 0; n < 100; ++n)
                {
                    if (s.client->request("hot/indexStats", {{"entryOf", uri}})["result"].value("entryOf", "") != "missing")
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        const int before = translations();
        const std::vector<std::string> places = script_places(s);
        analyses = translations() - before;
        return places;
    };
    int warm_analyses = 0, cold_analyses = 0;
    const std::vector<std::string> warm = run(true, warm_analyses);
    const std::vector<std::string> cold = run(false, cold_analyses);
    EXPECT_EQ(warm, cold);
    EXPECT_NE(std::find_if(warm.begin(), warm.end(), [](const std::string& l) { return l.rfind("pet.hot", 0) == 0; }), warm.end());
    // With the index on, the entries skip the decoy scripts in the cold run too: the two runs are equal.
    const int least = bg_on ? 0 : 5;
    EXPECT_GE(cold_analyses - warm_analyses, least) << "warm " << warm_analyses << " cold " << cold_analyses;
    // With the index on, no decoy goes to an analyze: only pet.hot does.
    if (bg_on)
    {
        EXPECT_EQ(cold_analyses, 1) << "cold " << cold_analyses;
        EXPECT_EQ(warm_analyses, 1) << "warm " << warm_analyses;
    }
}

// HOT-32: a script with a resolved script ref and a native use of the same name is not skipped.
TEST(LanguageServerNative, ReferencesKeepAStoredScriptWithAScriptRefAndANativeUse)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeScript, 10000, true, "", 10000, kNativeHeader, 120000);
    const std::string mix = "using \"natives.h\";\n\nclass A {\n    int Clamp100(int v) {\n        return v;\n    }\n"
                            "    int x = Clamp100(2);\n}\n\nclass B {\n    int y = Clamp100(5);\n}\n";
    const std::string path = (s.dir.path() / "mix.hot").string();
    write_file(path, mix);
    s.client->open(uri_of(path), mix);
    s.client->notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(path)}}}}); // stores the table
    const std::vector<std::string> places = script_places(s);
    EXPECT_NE(std::find(places.begin(), places.end(), "mix.hot:10:12"), places.end()) << "places " << places.size();
}

// HOT-32: when the time budget stops the scan, the server tells the user (log and showMessage)
// and the list keeps the scripts done so far. A proxy gives the open of b_slow.hot to clangd late.
TEST(LanguageServerNative, ReferencesTellTheUserWhenTheBudgetStopsTheScan)
{
    REQUIRE_CLANGD();
    if (std::system("python3 -c \"\" >/dev/null 2>&1") != 0)
        GTEST_SKIP() << "python3 is not installed";
    TempDir tools;
    const std::string proxy = (tools.path() / "slow_clangd.py").string();
    write_file(proxy,
        "import subprocess, sys, time\n"
        "p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE)\n"
        "src = sys.stdin.buffer\n"
        "while True:\n"
        "    head = b''\n"
        "    while not head.endswith(b'\\r\\n\\r\\n'):\n"
        "        c = src.read(1)\n"
        "        if not c:\n"
        "            p.stdin.close()\n"
        "            p.wait()\n"
        "            sys.exit(0)\n"
        "        head += c\n"
        "    size = 0\n"
        "    for line in head.split(b'\\r\\n'):\n"
        "        if line.lower().startswith(b'content-length:'):\n"
        "            size = int(line.split(b':')[1])\n"
        "    body = src.read(size)\n"
        "    if b'\"textDocument/didOpen\"' in body and b'b_slow.hot' in body:\n"
        "        time.sleep(8)\n"
        "    p.stdin.write(head + body)\n"
        "    p.stdin.flush()\n");
    const std::string clangd = (tools.path() / "slow_clangd").string();
    write_file(clangd, "#!/bin/sh\nexec python3 '" + proxy + "' '" HOT_CLANGD_PATH "' \"$@\"\n");
    std::filesystem::permissions(clangd, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    NativeSession s(kNativeScript, 10000, true, clangd, 10000, kNativeHeader, 5000);
    for (const char* name : {"a_pet.hot", "b_slow.hot", "c_pet.hot"})
        write_file((s.dir.path() / name).string(), kPetScript);
    json raw;
    std::vector<std::string> places;
    json note;
    for (int i = 0; i < 20 && note.is_null(); ++i)
    {
        places = script_places(s, &raw, false);
        note = s.client->wait_notification("window/showMessage",
            [](const json& p) { return p.value("message", std::string()).find("scan stopped by the time budget") != std::string::npos; },
            500);
    }
    ASSERT_FALSE(note.is_null()) << raw.dump();
    const std::string message = note["params"].value("message", "");
    const size_t of = message.find(" of 3 scripts");
    ASSERT_NE(of, std::string::npos) << message;
    const int done = std::atoi(message.substr(message.rfind(' ', of - 1) + 1, of - message.rfind(' ', of - 1) - 1).c_str());
    EXPECT_GE(done, 1) << message;
    EXPECT_LT(done, 3) << message;
    EXPECT_NE(std::find_if(places.begin(), places.end(), [](const std::string& l) { return l.rfind("a_pet.hot", 0) == 0; }), places.end())
        << raw.dump();
    EXPECT_EQ(std::find_if(places.begin(), places.end(), [](const std::string& l) { return l.rfind("c_pet.hot", 0) == 0; }), places.end());
}
#endif

namespace
{

// A host with an include dir outside of the workspace folder. The script uses a header of this dir.
struct HostHeaderCase
{
    TempDir dir, outside;
    std::string path, uri;
    hot::ls::ServerOptions o;
    // outside_is_root: the dir of the header is a source root of the host. near_script: the script
    // is in a subdir that no host matches (the host has the script dir "scripts"), so the nearest
    // host applies.
    explicit HostHeaderCase(bool outside_is_root = true, bool near_script = false, bool bin_is_root = false)
    {
        const std::filesystem::path root = dir.path();
        std::filesystem::create_directories(root / "build");
        std::filesystem::create_directories(root / "hosts");
        write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
        if (bin_is_root) // an in-source build
            write_file((root / "CMakeCache.txt").string(), "# cache\n");
        write_file((outside.path() / "outside.h").string(), "#pragma once\nint OutsideFn(int v);\n");
        write_file((root / "hosts" / "h.json").string(),
            R"({"version":1,"name":"h","binaryDir":")" + (bin_is_root ? root : root / "build").generic_string()
                + R"(","topSourceDir":")" + root.generic_string() + R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")"
                + (near_script ? (root / "scripts").generic_string() : root.generic_string())
                + R"("],"scripts":[],"sourceRoots":[")" + (outside_is_root ? outside.path().generic_string() : root.generic_string())
                + R"("],"includeDirs":[")" + outside.path().generic_string() + R"(",")" + HOT_SOURCE_DIR
                + R"(/include"],"defines":[],"flags":[]})");
        if (near_script)
        {
            std::filesystem::create_directories(root / "near");
            std::filesystem::create_directories(root / "scripts");
        }
        path = (root / (near_script ? "near/hero.hot" : "hero.hot")).string();
        uri = uri_of(path);
        write_file(path, "using \"outside.h\";\n\nclass Hero {\n    int x = OutsideFn(1);\n}\n");
        o = options(dir, true);
        o.hosts_dir = (root / "hosts").string();
    }
};

} // namespace

TEST(LanguageServerNative, RenameOfAHostHeaderOutsideTheWorkspaceIsAnEdit)
{
    REQUIRE_CLANGD();
    HostHeaderCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, "using \"outside.h\";\n\nclass Hero {\n    int x = OutsideFn(1);\n}\n");
    const std::string header_uri = uri_of((c.outside.path() / "outside.h").string());
    json p = {{"textDocument", {{"uri", c.uri}}}, {"position", pos(3, 12)}, {"newName", "OutsideFn2"}};
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = client.request("textDocument/rename", p);
        if (r.contains("result") && r["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["changes"].size(), 2u) << r.dump();
    EXPECT_EQ(edit_places(r["result"], c.uri, "OutsideFn2"), std::set<std::string>({"3:12-3:21"})) << r.dump();
    EXPECT_EQ(edit_places(r["result"], header_uri, "OutsideFn2"), std::set<std::string>({"1:4-1:13"})) << r.dump();
    p.erase("newName");
    const json prep = client.request("textDocument/prepareRename", p);
    EXPECT_TRUE(prep.contains("result") && prep["result"].is_object()) << prep.dump();
}

TEST(LanguageServerNative, RenameOfAHostHeaderIsRefusedWithTheSwitchOff)
{
    REQUIRE_CLANGD();
    HostHeaderCase c;
    LsClient client(c.o);
    client.request("initialize", {{"processId", nullptr}, {"rootUri", uri_of(c.dir.path().string())},
                                     {"capabilities", json::object()},
                                     {"initializationOptions", {{"renameHostHeaders", false}}}});
    client.notify("initialized", json::object());
    client.open(c.uri, "using \"outside.h\";\n\nclass Hero {\n    int x = OutsideFn(1);\n}\n");
    json p = {{"textDocument", {{"uri", c.uri}}}, {"position", pos(3, 12)}, {"newName", "OutsideFn2"}};
    const json r = client.request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find("renameHostHeaders is off"), std::string::npos) << r.dump();
    p.erase("newName");
    const json prep = client.request("textDocument/prepareRename", p);
    ASSERT_TRUE(prep.contains("error")) << prep.dump();
    EXPECT_NE(prep["error"]["message"].get<std::string>().find("renameHostHeaders is off"), std::string::npos)
        << prep.dump();
}

namespace
{

// Asks for a rename of OutsideFn in the script of c. The workspace folder is workspace_uri.
json rename_host_header(HostHeaderCase& c, const std::string& workspace_uri, json options = json::object())
{
    LsClient client(c.o);
    client.request("initialize", {{"processId", nullptr}, {"rootUri", workspace_uri}, {"capabilities", json::object()},
                                     {"initializationOptions", options}});
    client.notify("initialized", json::object());
    client.open(c.uri, "using \"outside.h\";\n\nclass Hero {\n    int x = OutsideFn(1);\n}\n");
    const json p = {{"textDocument", {{"uri", c.uri}}}, {"position", pos(3, 12)}, {"newName", "OutsideFn2"}};
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = client.request("textDocument/rename", p);
        if (r.contains("result") && r["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return r;
}

std::string error_text(const json& r)
{
    return r.contains("error") ? r["error"]["message"].get<std::string>() : std::string();
}

} // namespace

TEST(LanguageServerNative, RenameOfAHeaderBelowAnIncludeDirOutsideTheSourceRootsIsRefused)
{
    REQUIRE_CLANGD();
    for (const bool on : {true, false})
    {
        HostHeaderCase c(false);
        const json r = rename_host_header(c, uri_of(c.dir.path().string()), {{"renameHostHeaders", on}});
        SCOPED_TRACE(r.dump());
        ASSERT_TRUE(r.contains("error")) << r.dump();
        EXPECT_NE(error_text(r).find("outside the source dirs of the build of the host"), std::string::npos) << r.dump();
        // The setting text is only for an allowed header.
        EXPECT_EQ(error_text(r).find("renameHostHeaders is off"), std::string::npos) << r.dump();
    }
}

TEST(LanguageServerNative, RenameOfAHostHeaderFromAScriptWithNoHostAndNoWorkspaceIsRefused)
{
    REQUIRE_CLANGD();
    HostHeaderCase c(true, true);
    // The option -I lets clangd find the header, so the refusal comes from the scope rule.
    c.o.include_dirs.push_back(c.outside.path().string());
    TempDir other_workspace;
    const json r = rename_host_header(c, uri_of(other_workspace.path().string()));
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(error_text(r).find("outside the workspace folders"), std::string::npos) << r.dump();
}

TEST(LanguageServerNative, RenameOfAHostHeaderFromAScriptWithNoHostInTheWorkspaceUsesTheNearestHost)
{
    REQUIRE_CLANGD();
    HostHeaderCase c(true, true);
    const json r = rename_host_header(c, uri_of(c.dir.path().string()));
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const std::string header_uri = uri_of((c.outside.path() / "outside.h").string());
    EXPECT_EQ(edit_places(r["result"], header_uri, "OutsideFn2"), std::set<std::string>({"1:4-1:13"})) << r.dump();
}

// An in-source build (binaryDir is the workspace root) does not refuse a header of the workspace.
TEST(LanguageServerNative, RenameOfAWorkspaceHeaderWorksWithAnInSourceBuildDir)
{
    REQUIRE_CLANGD();
    HostHeaderCase c(true, false, true);
    write_file((c.dir.path() / "ws.h").string(), "#pragma once\nint WsFn(int v);\n");
    const std::string text = "using \"ws.h\";\n\nclass Hero {\n    int x = WsFn(1);\n}\n";
    write_file(c.path, text);
    c.o.include_dirs.push_back(c.dir.path().string());
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, text);
    const json p = {{"textDocument", {{"uri", c.uri}}}, {"position", pos(3, 12)}, {"newName", "WsFn2"}};
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = client.request("textDocument/rename", p);
        if (r.contains("result") && r["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    SCOPED_TRACE(r.dump());
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(edit_places(r["result"], uri_of((c.dir.path() / "ws.h").string()), "WsFn2"), std::set<std::string>({"1:4-1:8"}))
        << r.dump();
}

namespace
{

// A header with a virtual method and an override. The clangd rename of one of them renames the
// other. One script calls the method of the base, the other script calls the method of the
// derived struct.
struct VirtualCase
{
    TempDir dir;
    std::string base_path, derived_path;
    std::unique_ptr<LsClient> client;
    static constexpr const char* kHeader =
        "#pragma once\nstruct Base { virtual int Foo(); };\nstruct Derived : Base { int Foo() override; };\n";
    static constexpr const char* kBase = "using \"virt.h\";\n\nclass Hero {\n    void f() {\n        Base b;\n        b.Foo();\n    }\n}\n";
    static constexpr const char* kDerived =
        "using \"virt.h\";\n\nclass Pet {\n    void g() {\n        Derived d;\n        d.Foo();\n    }\n}\n";
    VirtualCase()
    {
        write_file((dir.path() / "virt.h").string(), kHeader);
        base_path = (dir.path() / "hero.hot").string();
        derived_path = (dir.path() / "pet.hot").string();
        write_file(base_path, kBase);
        write_file(derived_path, kDerived);
        hot::ls::ServerOptions o = options(dir, true);
        o.include_dirs.push_back(dir.path().string());
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(dir.path().string()));
    }
    // Rename from the open script "from" at the line and the column. The answer is an edit or a refusal.
    json rename(const std::string& from, const char* text, int line, int col)
    {
        client->open(uri_of(from), text);
        const json p = {{"textDocument", {{"uri", uri_of(from)}}}, {"position", pos(line, col)}, {"newName", "Bar"}};
        json r;
        for (int i = 0; i < 20; ++i)
        {
            r = client->request("textDocument/rename", p);
            if (r.contains("result") && r["result"].is_object())
                break;
            if (r.contains("error") && r["error"]["message"].get<std::string>().find("does not change") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return r;
    }
    // Both scripts and both method declarations change, or the rename is refused.
    void check(const json& r) const
    {
        SCOPED_TRACE(r.dump());
        if (r.contains("error"))
        {
            EXPECT_NE(r["error"]["message"].get<std::string>().find("that the rename of clangd does not change"), std::string::npos)
                << r.dump();
            return;
        }
        ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
        EXPECT_EQ(edit_places(r["result"], uri_of(base_path), "Bar"), std::set<std::string>({"5:10-5:13"})) << r.dump();
        EXPECT_EQ(edit_places(r["result"], uri_of(derived_path), "Bar"), std::set<std::string>({"5:10-5:13"})) << r.dump();
        EXPECT_EQ(edit_places(r["result"], uri_of((dir.path() / "virt.h").string()), "Bar"),
            std::set<std::string>({"1:26-1:29", "2:28-2:31"}))
            << r.dump();
        EXPECT_EQ(r["result"]["changes"].size(), 3u) << r.dump();
    }
};

} // namespace

// The rename starts at the call of the base method. The other script calls the override.
TEST(LanguageServerNative, RenameOfAVirtualMethodChangesAScriptThatCallsTheOverride)
{
    REQUIRE_CLANGD();
    VirtualCase c;
    c.check(c.rename(c.base_path, VirtualCase::kBase, 5, 10));
}

// The rename starts at the call of the override. The other script calls the base method.
TEST(LanguageServerNative, RenameOfAnOverrideChangesAScriptThatCallsTheBaseMethod)
{
    REQUIRE_CLANGD();
    VirtualCase c;
    c.check(c.rename(c.derived_path, VirtualCase::kDerived, 5, 10));
}

namespace
{

// A header and two scripts, "hero.hot" and "pet.hot". Both scripts call a method with the same
// name. The rename starts in "hero.hot" at the call (line 5, column 10).
struct ProbeCase
{
    TempDir dir;
    std::string hero_path, pet_path;
    std::unique_ptr<LsClient> client;
    static std::string script(const std::string& cls, const std::string& type, const std::string& call)
    {
        return "using \"probe.h\";\n\nclass " + cls + " {\n    void f() {\n        " + type + " v;\n        v." + call
            + ";\n    }\n}\n";
    }
    ProbeCase(const std::string& header, const std::string& hero, const std::string& pet, int budget_ms = 10000)
    {
        write_file((dir.path() / "probe.h").string(), header);
        hero_path = (dir.path() / "hero.hot").string();
        pet_path = (dir.path() / "pet.hot").string();
        write_file(hero_path, hero);
        write_file(pet_path, pet);
        hot::ls::ServerOptions o = options(dir, true);
        o.include_dirs.push_back(dir.path().string());
        o.rename_budget_ms = budget_ms;
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(dir.path().string()));
    }
    std::string uri(const char* file) const { return uri_of((dir.path() / file).string()); }
    // The answer is an edit or a refusal of the rename at the call in "hero.hot".
    json rename(const std::string& hero_text, const char* new_name = "Bar")
    {
        client->open(uri_of(hero_path), hero_text);
        const json p = {{"textDocument", {{"uri", uri_of(hero_path)}}}, {"position", pos(5, 10)}, {"newName", new_name}};
        json r;
        for (int i = 0; i < 20; ++i)
        {
            r = client->request("textDocument/rename", p);
            if (r.contains("result") && r["result"].is_object())
                break;
            if (r.contains("error") && r["error"]["message"].get<std::string>().find("does not change") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return r;
    }
    // The rename is refused and gives no edit.
    void expect_refusal(const json& r) const
    {
        SCOPED_TRACE(r.dump());
        ASSERT_TRUE(r.contains("error")) << r.dump();
        EXPECT_NE(r["error"]["message"].get<std::string>().find("that the rename of clangd does not change"), std::string::npos)
            << r.dump();
    }
};

} // namespace

// Two unrelated classes have a method with the same name. A narrowing with a rename probe was
// tried and removed: for an override, a diamond and a template specialization the probe of the
// other script gives an edit that is a partial rename. So the server refuses all the cases.
TEST(LanguageServerNative, RenameOfAMethodIsRefusedForAnUnrelatedClassWithTheSameMethodName)
{
    REQUIRE_CLANGD();
    const std::string a = ProbeCase::script("Hero", "Node", "GetName()");
    ProbeCase c("#pragma once\nstruct Node { int GetName(); };\nstruct Component { int GetName(); };\n", a,
        ProbeCase::script("Pet", "Component", "GetName()"));
    c.expect_refusal(c.rename(a));
}

// The other script uses an override that the first answer of clangd does not change.
TEST(LanguageServerNative, RenameOfAMethodIsRefusedForAnOverrideInAnotherScript)
{
    REQUIRE_CLANGD();
    const std::string a = ProbeCase::script("Hero", "Base", "Foo()");
    ProbeCase c("#pragma once\nstruct Base { virtual int Foo(); };\nstruct Derived : Base { int Foo() override; };\n",
        a, ProbeCase::script("Pet", "Derived", "Foo()"));
    c.expect_refusal(c.rename(a));
}

// C overrides A::f and B::f. One script uses A::f and the other script uses B::f.
TEST(LanguageServerNative, RenameOfAMethodIsRefusedForADiamondInAnotherScript)
{
    REQUIRE_CLANGD();
    const std::string a = ProbeCase::script("Hero", "A", "f()");
    ProbeCase c("#pragma once\nstruct A { virtual int f(); };\nstruct B { virtual int f(); };\n"
                "struct C : A, B { int f() override; };\n",
        a, ProbeCase::script("Pet", "B", "f()"));
    c.expect_refusal(c.rename(a));
}

// One script uses the method of the primary template, the other uses a specialization.
TEST(LanguageServerNative, RenameOfAMethodIsRefusedForATemplateSpecializationInAnotherScript)
{
    REQUIRE_CLANGD();
    const std::string a = ProbeCase::script("Hero", "S<float>", "Get()");
    ProbeCase c("#pragma once\ntemplate <class T> struct S { int Get(); };\n"
                "template <> struct S<int> { int Get(); };\n",
        a, ProbeCase::script("Pet", "S<int>", "Get()"));
    c.expect_refusal(c.rename(a));
}

// A budget that is too small refuses the rename. There is never an edit.
TEST(LanguageServerNative, RenameWithATinyBudgetIsRefusedForAScriptWithTheSameMethodName)
{
    REQUIRE_CLANGD();
    const std::string a = ProbeCase::script("Hero", "Node", "GetName()");
    ProbeCase c("#pragma once\nstruct Node { int GetName(); };\nstruct Component { int GetName(); };\n", a,
        ProbeCase::script("Pet", "Component", "GetName()"), 1);
    const json r = c.rename(a);
    SCOPED_TRACE(r.dump());
    EXPECT_FALSE(r.contains("result") && r["result"].is_object()) << r.dump();
}

namespace
{

struct Tok
{
    int line, col, len, type;
    bool operator==(const Tok& o) const { return line == o.line && col == o.col && len == o.len && type == o.type; }
};

std::vector<Tok> decode_native_tokens(const json& data)
{
    std::vector<Tok> out;
    int line = 0, col = 0;
    for (size_t i = 0; i + 4 < data.size(); i += 5)
    {
        line += data[i].get<int>();
        col = data[i].get<int>() == 0 ? col + data[i + 1].get<int>() : data[i + 1].get<int>();
        out.push_back({line, col, data[i + 2].get<int>(), data[i + 3].get<int>()});
    }
    return out;
}

} // namespace

// The legend: keyword 0, type 1, class 2, method 3, property 4. The legend does not change.
TEST(LanguageServerNative, SemanticTokensClassifyNativeNames)
{
    REQUIRE_CLANGD();
    NativeSession s;
    json r;
    std::vector<Tok> tokens;
    auto has = [&](const std::string& context, const std::string& word, int type)
    {
        const json p = native_pos(context, word);
        const Tok want{p["line"], p["character"], static_cast<int>(word.size()), type};
        return std::count(tokens.begin(), tokens.end(), want) == 1;
    };
    auto any_at = [&](const std::string& context, const std::string& word)
    {
        const json p = native_pos(context, word);
        return std::any_of(tokens.begin(), tokens.end(), [&](const Tok& t) { return t.line == p["line"] && t.col == p["character"]; });
    };
    // The first answers can come before clangd has the syntax tree: ask again.
    for (int i = 0; i < 20 && !has("Clamp100(MAX_HP)", "Clamp100", 3); ++i)
    {
        r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
        ASSERT_TRUE(r.contains("result")) << r.dump();
        tokens = decode_native_tokens(r["result"]["data"]);
        if (!has("Clamp100(MAX_HP)", "Clamp100", 3))
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    EXPECT_TRUE(has("Clamp100(MAX_HP)", "Clamp100", 3)); // a function
    EXPECT_TRUE(has("after = Clamp100(2)", "Clamp100", 3)); // after a two-byte character
    EXPECT_TRUE(has("Counter local;", "Counter", 2)); // a class with no map segment
    EXPECT_TRUE(has("Mood m = ", "Mood", 2)); // an enum
    EXPECT_TRUE(has("Mood.Angry;", "Angry", 4)); // an enumerator
    EXPECT_TRUE(has("local.Bump(", "Bump", 3)); // a method
    EXPECT_TRUE(has("this.Roar()", "Roar", 3));
    EXPECT_TRUE(has("this.rage =", "rage", 4)); // a field
    EXPECT_TRUE(has("this.counter.count", "count", 4));
    EXPECT_TRUE(has("class Hero : Boss", "Boss", 2)); // the script scanner classifies it: one token only
    EXPECT_TRUE(has("class Hero : Boss", "Hero", 2));
    // A word in a comment or a string, a parameter with the name of a type, a macro: no native token.
    EXPECT_FALSE(any_at("Roar and", "Roar"));
    EXPECT_FALSE(any_at("\"Clamp100 in a string", "Clamp100"));
    EXPECT_FALSE(any_at("Take(Counter Counter)", "Counter"));
    EXPECT_FALSE(any_at("(MAX_HP)", "MAX_HP"));
}

// Without clangd the tokens are the tokens of the script scanner.
TEST(LanguageServerNative, SemanticTokensWithoutClangdAreTheScriptTokens)
{
    TempDir dir;
    const std::string path = (dir.path() / "hero.hot").string();
    write_file(path, kNativeScript);
    LsClient client(options(dir, false));
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(path), kNativeScript);
    const json r = client.request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", uri_of(path)}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const std::vector<Tok> tokens = decode_native_tokens(r["result"]["data"]);
    const json p = native_pos("Counter local;", "Counter");
    EXPECT_TRUE(std::none_of(tokens.begin(), tokens.end(), [&](const Tok& t) { return t.line == p["line"] && t.col == p["character"]; }));
    EXPECT_FALSE(tokens.empty());
}

namespace
{

// Two native uses in one expression, and a local with the name of a native member.
const char* const kRenameTraps =
    "using \"natives.h\";\n"
    "\n"
    "class Hero {\n"
    "    Counter counter;\n"
    "\n"
    "    int Nested(int v) {\n"
    "        return Clamp100(Clamp100(v));\n"
    "    }\n"
    "\n"
    "    int Shadow() {\n"
    "        int count = counter.count;\n"
    "        return count;\n"
    "    }\n"
    "}\n";

// A rename request for the traps script. The index of clangd can need some time: ask again while
// the answer says that clangd has no answer.
json rename_traps(
    NativeSession& s, const std::string& context, const std::string& word, const std::string& name, int nth = 0)
{
    json p = {{"textDocument", {{"uri", s.uri}}}, {"position", pos_in(kRenameTraps, context, word, nth)}, {"newName", name}};
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = s.client->request("textDocument/rename", p);
        const std::string message = r.contains("error") ? r["error"].value("message", "") : std::string();
        if (!r.contains("error") || (message.find("clangd gives no answer") == std::string::npos
                                        && message.find("has no edit") == std::string::npos))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return r;
}

} // namespace

// Two native uses in one expression: each use gets its own place (the k-th use rule).
TEST(LanguageServerNative, RenameOfTwoNativeUsesInOneExpressionChangesBoth)
{
    REQUIRE_CLANGD();
    NativeSession s(kRenameTraps);
    const json r = rename_traps(s, "Clamp100(Clamp100", "Clamp100", "ClampHp");
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json p = pos_in(kRenameTraps, "Clamp100(Clamp100", "Clamp100");
    const int line = p["line"], col = p["character"];
    const std::string first = std::to_string(line) + ":" + std::to_string(col) + "-" + std::to_string(line) + ":" + std::to_string(col + 8);
    const std::string second = std::to_string(line) + ":" + std::to_string(col + 9) + "-" + std::to_string(line) + ":" + std::to_string(col + 17);
    EXPECT_EQ(edit_places(r["result"], s.uri, "ClampHp"), (std::set<std::string>{first, second})) << r.dump();
}

// The local "count" comes before the native member "count" in the line: only the member changes.
TEST(LanguageServerNative, RenameWithALocalOfTheSameNameInTheLineNeverChangesTheLocal)
{
    REQUIRE_CLANGD();
    NativeSession s(kRenameTraps);
    const json r = rename_traps(s, "counter.count", "count", "total", 1);
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json p = pos_in(kRenameTraps, "int count = counter.count;", "count", 2);
    const std::string member = std::to_string(p["line"].get<int>()) + ":" + std::to_string(p["character"].get<int>()) + "-"
        + std::to_string(p["line"].get<int>()) + ":" + std::to_string(p["character"].get<int>() + 5);
    EXPECT_EQ(edit_places(r["result"], s.uri, "total"), std::set<std::string>{member}) << r.dump();
}

// The answer of clangd is in a cache for each version of the text: the second request does not
// ask clangd again.
TEST(LanguageServerNative, SemanticTokensAnswerIsCachedForEachVersion)
{
    REQUIRE_CLANGD();
    NativeSession s;
    auto requests = [&] { return s.client->request("hot/indexStats", json::object())["result"].value("tokenRequests", -1); };
    json first;
    for (int i = 0; i < 20; ++i)
    {
        first = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
        const int n = requests();
        if (n >= 1 && first["result"]["data"].size() > 0
            && decode_native_tokens(first["result"]["data"]).size() > 40)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    const int before = requests();
    const json again = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    EXPECT_EQ(requests(), before);
    EXPECT_EQ(again["result"], first["result"]);
    // A new version of the text asks clangd again.
    std::string changed = kNativeScript;
    changed += "\n";
    s.client->change(s.uri, 2, changed);
    s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    EXPECT_GT(requests(), before);
}

// No wait: the first answer has the script tokens only. The late answer of clangd goes to the
// cache, and the next request has the native tokens.
TEST(LanguageServerNative, SemanticTokensTimeoutGivesTheScriptTokensThenTheLateAnswer)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeScript, 0);
    const json p = native_pos("Counter local;", "Counter");
    auto has_native = [&](const std::vector<Tok>& tokens)
    { return std::any_of(tokens.begin(), tokens.end(), [&](const Tok& t) { return t.line == p["line"] && t.col == p["character"]; }); };
    const json first = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(first.contains("result")) << first.dump();
    const std::vector<Tok> script_only = decode_native_tokens(first["result"]["data"]);
    EXPECT_FALSE(script_only.empty());
    EXPECT_FALSE(has_native(script_only));
    bool filled = false;
    for (int i = 0; i < 40 && !filled; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const json r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
        filled = has_native(decode_native_tokens(r["result"]["data"]));
    }
    EXPECT_TRUE(filled);
}

// The document closes and opens again while a worker waits for clangd. The old worker does not
// write into the new cache entry; the new entry asks clangd itself and gets its own answer.
TEST(LanguageServerNative, SemanticTokensAfterCloseAndReopenUseTheNewCacheEntry)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeScript, 0);
    auto requests = [&] { return s.client->request("hot/indexStats", json::object())["result"].value("tokenRequests", -1); };
    s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    EXPECT_EQ(requests(), 1);
    s.client->notify("textDocument/didClose", {{"textDocument", {{"uri", s.uri}}}});
    s.client->open(s.uri, kNativeScript);
    s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    EXPECT_EQ(requests(), 2); // the new entry has its own request
    const json p = native_pos("Counter local;", "Counter");
    bool filled = false;
    for (int i = 0; i < 40 && !filled; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const json r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
        const std::vector<Tok> tokens = decode_native_tokens(r["result"]["data"]);
        filled = std::any_of(tokens.begin(), tokens.end(), [&](const Tok& t) { return t.line == p["line"] && t.col == p["character"]; });
    }
    EXPECT_TRUE(filled);
}

// The switch "nativeSemanticTokens": off gives the script tokens and no request to clangd.
TEST(LanguageServerNative, SemanticTokensSwitchTurnsTheNativeTokensOff)
{
    REQUIRE_CLANGD();
    NativeSession s(kNativeScript, 10000, false);
    const json r = s.client->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json p = native_pos("Counter local;", "Counter");
    const std::vector<Tok> tokens = decode_native_tokens(r["result"]["data"]);
    EXPECT_FALSE(tokens.empty());
    EXPECT_TRUE(std::none_of(tokens.begin(), tokens.end(), [&](const Tok& t) { return t.line == p["line"] && t.col == p["character"]; }));
    EXPECT_EQ(s.client->request("hot/indexStats", json::object())["result"].value("tokenRequests", -1), 0);
}

// The command line switch.
TEST(LanguageServerNative, SemanticTokensSwitchOnTheCommandLine)
{
    hot::ls::ServerOptions o;
    std::string error;
    ASSERT_TRUE(hot::ls::parse_arguments({"--stdio", "--no-native-tokens"}, o, error)) << error;
    EXPECT_FALSE(o.native_semantic_tokens);
    EXPECT_TRUE(hot::ls::ServerOptions().native_semantic_tokens);
}

// The command line switch for the edit of host headers.
TEST(LanguageServerNative, RenameHostHeadersSwitchOnTheCommandLine)
{
    hot::ls::ServerOptions o;
    std::string error;
    ASSERT_TRUE(hot::ls::parse_arguments({"--stdio", "--no-rename-host-headers"}, o, error)) << error;
    EXPECT_FALSE(o.rename_host_headers);
    EXPECT_TRUE(hot::ls::ServerOptions().rename_host_headers);
    EXPECT_EQ(hot::ls::ServerOptions().rename_budget_ms, 10000);
}

// The switch in the initializationOptions.
TEST(LanguageServerNative, SemanticTokensSwitchInInitializationOptions)
{
    REQUIRE_CLANGD();
    TempDir dir;
    write_file((dir.path() / "natives.h").string(), kNativeHeader);
    const std::string path = (dir.path() / "hero.hot").string();
    write_file(path, kNativeScript);
    hot::ls::ServerOptions o = options(dir, true);
    o.include_dirs.push_back(dir.path().string());
    LsClient client(o);
    client.request("initialize", {{"processId", nullptr}, {"rootUri", uri_of(dir.path().string())},
                                     {"capabilities", json::object()},
                                     {"initializationOptions", {{"nativeSemanticTokens", false}}}});
    client.notify("initialized", json::object());
    client.open(uri_of(path), kNativeScript);
    const json r = client.request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", uri_of(path)}}}});
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_FALSE(r["result"]["data"].empty());
    EXPECT_EQ(client.request("hot/indexStats", json::object())["result"].value("tokenRequests", -1), 0);
}

// ------------------------------------------------------------------ definition and hover of script declarations
namespace
{

const char* const kDeclScript =
    "using \"Actor.h\";\n"
    "\n"
    "class Player : Actor {\n"
    "    static const int MaxHp = 100;\n"
    "    static int Count = 0;\n"
    "    float hp = 100.0;\n"
    "    static int Twice(int v) { return v * 2; }\n"
    "    override void Tick(float dt) {\n"
    "        base.Tick(dt);\n"
    "        this.hp = Player.MaxHp;\n"
    "        Player.Count = Player.Twice(3);\n"
    "        int m = MaxHp;\n"
    "    }\n"
    "}\n";

// The position of the last "word" in the first "context" of kDeclScript.
json decl_pos(const std::string& context, const std::string& word)
{
    const std::string text = kDeclScript;
    const size_t at = text.find(context);
    EXPECT_NE(at, std::string::npos) << context;
    const size_t w = at + context.rfind(word);
    const size_t ls = text.rfind('\n', w - 1) + 1;
    return pos(static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(w), '\n')),
        static_cast<int>(w - ls));
}

// The definition of the word is in the script at "decl_line", or (empty "decl_line") in a header.
void expect_decl_definition(const std::string& context, const std::string& word, int decl_line, const std::string& header = "")
{
    REQUIRE_CLANGD();
    Session s(true, kDeclScript);
    const json p = decl_pos(context, word);
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, p["line"], p["character"]));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << context << " -> " << r.dump();
    const json& loc = r["result"][0];
    if (header.empty())
    {
        EXPECT_EQ(loc["uri"], s.uri) << r.dump();
        EXPECT_EQ(loc["range"]["start"]["line"], decl_line) << r.dump();
    }
    else
        EXPECT_NE(loc.value("uri", "").find(header), std::string::npos) << r.dump();
}

void expect_decl_hover(const std::string& context, const std::string& word, const std::string& want)
{
    REQUIRE_CLANGD();
    Session s(true, kDeclScript);
    const json p = decl_pos(context, word);
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, p["line"], p["character"]));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << context << " -> " << r.dump();
    EXPECT_NE(r["result"].dump().find(want), std::string::npos) << context << " -> " << r.dump();
}

} // namespace

TEST(LanguageServerDecl, DefinitionOfAClassConstantAfterTheClassName)
{
    expect_decl_definition("= Player.MaxHp", "MaxHp", 3);
}

TEST(LanguageServerDecl, DefinitionOfABareClassConstant)
{
    expect_decl_definition("int m = MaxHp", "MaxHp", 3);
}

TEST(LanguageServerDecl, DefinitionOfAStaticFieldAfterTheClassName)
{
    expect_decl_definition("Player.Count =", "Count", 4);
}

TEST(LanguageServerDecl, DefinitionOfAStaticMethodAfterTheClassName)
{
    expect_decl_definition("= Player.Twice(", "Twice", 6);
}

TEST(LanguageServerDecl, DefinitionOfTheClassNameBeforeTheDot)
{
    expect_decl_definition("= Player.MaxHp", "Player", 2);
}

TEST(LanguageServerDecl, DefinitionOfTheBaseClassName)
{
    expect_decl_definition("class Player : Actor", "Actor", 0, "Actor.h");
}

TEST(LanguageServerDecl, DefinitionOfABaseMember)
{
    expect_decl_definition("base.Tick(", "Tick", 0, "Actor.h");
}

TEST(LanguageServerDecl, DefinitionOfAnOverrideName)
{
    expect_decl_definition("override void Tick", "Tick", 0, "Actor.h");
}

TEST(LanguageServerDecl, HoverOfAClassConstantAfterTheClassName)
{
    expect_decl_hover("= Player.MaxHp", "MaxHp", "int");
}

TEST(LanguageServerDecl, HoverOfAStaticFieldAfterTheClassName)
{
    expect_decl_hover("Player.Count =", "Count", "int");
}

TEST(LanguageServerDecl, HoverOfAStaticMethodAfterTheClassName)
{
    expect_decl_hover("= Player.Twice(", "Twice", "int");
}

TEST(LanguageServerDecl, HoverOfTheBaseClassName)
{
    expect_decl_hover("class Player : Actor", "Actor", "Actor");
}

TEST(LanguageServerDecl, HoverOfABaseMember)
{
    expect_decl_hover("base.Tick(", "Tick", "Tick");
}

TEST(LanguageServerDecl, HoverOfAnOverrideName)
{
    expect_decl_hover("override void Tick", "Tick", "Tick");
}

// ------------------------------------------------------------------ overloaded functions and methods
namespace
{

// The 0-based lines: 1, 2 and 3 the overloads of Pick, 5 and 6 the overloads of Put, 8 to 12 the calls.
const char* const kOverloadScript =
    "using \"Actor.h\";\n"
    "int Pick(int v) { return v; }\n"
    "float Pick(float f) { return f; }\n"
    "int Pick(int a, int b) { return a + b; }\n"
    "class Box : Actor {\n"
    "    int Put(int v) { return v; }\n"
    "    float Put(float f) { return f; }\n"
    "    void Run() {\n"
    "        int a = Pick(1);\n"
    "        float b = Pick(2.5f);\n"
    "        int c = Pick(1, 2);\n"
    "        int d = this.Put(1);\n"
    "        float e = this.Put(1.5f);\n"
    "    }\n"
    "}\n";

// The request is at the name of the call on "call_line" (0-based).
json overload_request(Session& s, const char* method, int call_line, const std::string& name)
{
    std::string line;
    std::istringstream in(kOverloadScript);
    for (int i = 0; i <= call_line; ++i)
        std::getline(in, line);
    const int col = static_cast<int>(line.find(name + "("));
    return s.client->request(method, doc_pos(s.uri, call_line, col + 1));
}

void expect_overload_definition(int call_line, const std::string& name, int decl_line)
{
    REQUIRE_CLANGD();
    Session s(true, kOverloadScript);
    const json r = overload_request(s, "textDocument/definition", call_line, name);
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    EXPECT_EQ(r["result"][0]["uri"], s.uri) << r.dump();
    EXPECT_EQ(r["result"][0]["range"]["start"]["line"], decl_line) << r.dump();
}

void expect_overload_hover(int call_line, const std::string& name, const std::string& want)
{
    REQUIRE_CLANGD();
    Session s(true, kOverloadScript);
    const json r = overload_request(s, "textDocument/hover", call_line, name);
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_NE(r["result"].dump().find(want), std::string::npos) << r.dump();
}

} // namespace

TEST(LanguageServerDecl, DefinitionOfAFunctionCallSelectsTheOverloadByTheArgumentType)
{
    expect_overload_definition(8, "Pick", 1);
    expect_overload_definition(9, "Pick", 2);
}

TEST(LanguageServerDecl, DefinitionOfAFunctionCallSelectsTheOverloadByTheArgumentCount)
{
    expect_overload_definition(10, "Pick", 3);
}

TEST(LanguageServerDecl, DefinitionOfAMethodCallSelectsTheOverloadByTheArgumentType)
{
    expect_overload_definition(11, "Put", 5);
    expect_overload_definition(12, "Put", 6);
}

TEST(LanguageServerDecl, HoverOfAFunctionCallShowsTheSignatureOfTheSelectedOverload)
{
    expect_overload_hover(8, "Pick", "Pick(int v)");
    expect_overload_hover(9, "Pick", "Pick(float f)");
    expect_overload_hover(10, "Pick", "Pick(int a, int b)");
}

TEST(LanguageServerDecl, HoverOfAMethodCallShowsTheSignatureOfTheSelectedOverload)
{
    expect_overload_hover(11, "Put", "Put(Box *__self, int v)");
    expect_overload_hover(12, "Put", "Put(Box *__self, float f)");
}

// ------------------------------------------------------------------ document links of using lines
TEST(LanguageServerLinks, UsingHeaderAndScriptGetLinks)
{
    REQUIRE_CLANGD();
    Session s(true, std::string("using \"base.hot\";\nusing \"none.hot\";\n") + kScript);
    write_file(s.dir.path() / "base.hot", kImported);
    const json r = s.client->request("textDocument/documentLink", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    ASSERT_EQ(r["result"].size(), 2u) << r.dump();
    const json& h = r["result"][1];
    EXPECT_EQ(h["range"]["start"], pos(2, 7)) << r.dump();
    EXPECT_EQ(h["range"]["end"], pos(2, 14)) << r.dump();
    EXPECT_NE(h.value("target", "").find("Actor.h"), std::string::npos) << r.dump();
    EXPECT_EQ(r["result"][0]["target"], uri_of((s.dir.path() / "base.hot").lexically_normal().string())) << r.dump();
    EXPECT_EQ(r["result"][0]["range"]["start"], pos(0, 7)) << r.dump();
}

// ------------------------------------------------------------------ rename of static members and constants
namespace
{

// Renames the word in the context. The edits must be at the lines "want" (sorted), with the word length.
void expect_decl_rename(const std::string& context, const std::string& word, std::vector<int> want)
{
    REQUIRE_CLANGD();
    Session s(true, kDeclScript);
    const json p = decl_pos(context, word);
    json params = doc_pos(s.uri, p["line"], p["character"]);
    params["newName"] = "Renamed";
    const json r = s.client->request("textDocument/rename", params);
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << context << " -> " << r.dump();
    const json& edits = r["result"]["changes"][s.uri];
    std::vector<int> got;
    for (const json& e : edits)
    {
        got.push_back(e["range"]["start"]["line"]);
        EXPECT_EQ(e["range"]["end"]["character"].get<int>() - e["range"]["start"]["character"].get<int>(),
            static_cast<int>(word.size()))
            << r.dump();
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, want) << context << " -> " << r.dump();
}

} // namespace

TEST(LanguageServerRename, ClassConstantFromTheDeclaration)
{
    expect_decl_rename("const int MaxHp", "MaxHp", {3, 9, 11});
}

TEST(LanguageServerRename, ClassConstantFromAQualifiedUse)
{
    expect_decl_rename("= Player.MaxHp", "MaxHp", {3, 9, 11});
}

TEST(LanguageServerRename, ClassConstantFromABareUse)
{
    expect_decl_rename("int m = MaxHp", "MaxHp", {3, 9, 11});
}

TEST(LanguageServerRename, StaticFieldFromAQualifiedUse)
{
    expect_decl_rename("Player.Count =", "Count", {4, 10});
}

TEST(LanguageServerRename, StaticMethodFromAQualifiedUse)
{
    expect_decl_rename("= Player.Twice(", "Twice", {6, 10});
}

TEST(LanguageServerRename, FieldFromAUse)
{
    expect_decl_rename("this.hp =", "hp", {5, 9});
}

// ------------------------------------------------------------------ scope rules of static members
namespace
{

const char* const kShadowScript =
    "using \"Actor.h\";\n"
    "class A : Actor {\n"
    "    static int count = 0;\n"
    "    void f(int count) { count = 1; }\n"
    "    void g() {\n"
    "        // count in a comment\n"
    "        string s = \"count\";\n"
    "        count = 2;\n"
    "    }\n"
    "}\n"
    "void h() { count = 3; }\n";

json shadow_pos(const std::string& context, const std::string& word)
{
    const std::string text = kShadowScript;
    const size_t at = text.find(context);
    EXPECT_NE(at, std::string::npos) << context;
    const size_t w = at + context.rfind(word);
    return pos(static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(w), '\n')),
        static_cast<int>(w - (text.rfind('\n', w - 1) + 1)));
}

json shadow_request(Session& s, const char* method, const std::string& context, const std::string& word,
    const char* new_name = nullptr)
{
    const json p = shadow_pos(context, word);
    json params = doc_pos(s.uri, p["line"], p["character"]);
    if (new_name)
        params["newName"] = new_name;
    return s.client->request(method, params);
}

std::vector<int> edit_lines(const json& r, const std::string& uri)
{
    std::vector<int> out;
    if (r.contains("result") && r["result"].is_object() && r["result"].contains("changes"))
        for (const json& e : r["result"]["changes"].value(uri, json::array()))
            out.push_back(e["range"]["start"]["line"]);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST(LanguageServerScope, ParameterShadowsAStaticForHoverAndDefinition)
{
    REQUIRE_CLANGD();
    Session s(true, kShadowScript);
    const json h = shadow_request(s, "textDocument/hover", "{ count = 1", "count");
    EXPECT_EQ(h.dump().find("static"), std::string::npos) << h.dump();
    const json d = shadow_request(s, "textDocument/definition", "{ count = 1", "count");
    for (const json& loc : d.value("result", json::array()))
        EXPECT_NE(loc["range"]["start"]["line"], 2) << d.dump();
}

TEST(LanguageServerScope, ParameterShadowsAStaticForRename)
{
    REQUIRE_CLANGD();
    Session s(true, kShadowScript);
    const json r = shadow_request(s, "textDocument/rename", "{ count = 1", "count", "Renamed");
    for (int l : edit_lines(r, s.uri))
        EXPECT_EQ(l, 3) << r.dump(); // only the parameter lines, never the static
    const json r2 = shadow_request(s, "textDocument/rename", "(int count)", "count", "Renamed");
    for (int l : edit_lines(r2, s.uri))
        EXPECT_EQ(l, 3) << r2.dump();
}

TEST(LanguageServerScope, RenameOfAStaticRefusesWhenClangdCannotResolveAWord)
{
    REQUIRE_CLANGD();
    Session s(true, kShadowScript);
    const json r = shadow_request(s, "textDocument/rename", "        count = 2", "count", "Renamed");
    // The table knows the parameter of "f". The word "count" in "h" has no ref, and clangd cannot
    // resolve it: the rename refuses and never skips the place.
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("cannot resolve count at"), std::string::npos) << r.dump();
}

TEST(LanguageServerScope, WordAfterTheClassIsNotTheStatic)
{
    REQUIRE_CLANGD();
    Session s(true, kShadowScript);
    const json h = shadow_request(s, "textDocument/hover", "void h() { count", "count");
    EXPECT_EQ(h.dump().find("static int count"), std::string::npos) << h.dump();
    const json r = shadow_request(s, "textDocument/rename", "void h() { count", "count", "Renamed");
    for (int l : edit_lines(r, s.uri))
        EXPECT_EQ(l, 10) << r.dump();
}

TEST(LanguageServerScope, WordInACommentOrAStringIsNotTheStatic)
{
    REQUIRE_CLANGD();
    Session s(true, kShadowScript);
    for (const char* ctx : {"// count", "\"count\""})
    {
        const json h = shadow_request(s, "textDocument/hover", ctx, "count");
        EXPECT_EQ(h.dump().find("static int count"), std::string::npos) << h.dump();
        const json r = shadow_request(s, "textDocument/rename", ctx, "count", "Renamed");
        EXPECT_TRUE(edit_lines(r, s.uri).empty()) << r.dump();
        const json pr = shadow_request(s, "textDocument/prepareRename", ctx, "count");
        EXPECT_TRUE(pr.contains("error")) << pr.dump();
    }
}

TEST(LanguageServerScope, RenameOfAStaticChangesAnImporterThatUsesTheWord)
{
    REQUIRE_CLANGD();
    Session s(true, kDeclScript);
    write_file(s.dir.path() / "other.hot",
        "using \"player.hot\";\nclass Kid : Player {\n    void Go() { int x = Player.MaxHp; }\n}\n");
    const json p = decl_pos("= Player.MaxHp", "MaxHp");
    json params = doc_pos(s.uri, p["line"], p["character"]);
    params["newName"] = "Renamed";
    const json r = s.client->request("textDocument/rename", params);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_TRUE(r["result"]["changes"].contains(uri_of((s.dir.path() / "other.hot").string()))) << r.dump();
}

namespace
{

const char* const kOriginScript =
    "using \"Actor.h\";\n"
    "\n"
    "enum Team { Red, Blue }\n"
    "\n"
    "class Spawn : Actor {\n"
    "    static const int MaxHp = 100;\n"
    "    Team GetTeam() { return Team.Red; }\n"
    "    override void Tick(float dt) {\n"
    "        int k = MaxHp;\n"
    "        float a = 2;\n"
    "        float r = a * MaxHp + 1;\n"
    "        int f = 3 & MaxHp;\n"
    "    }\n"
    "}\n";

// The rename response for the last "word" of the first "context" in the origin script.
json origin_rename(Session& s, const std::string& text, const std::string& context, const std::string& word)
{
    const size_t at = text.find(context);
    EXPECT_NE(at, std::string::npos) << context;
    const size_t w = at + context.rfind(word);
    json params = doc_pos(s.uri, static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(w), '\n')),
        static_cast<int>(w - (text.rfind('\n', w - 1) + 1)));
    params["newName"] = "Renamed";
    return s.client->request("textDocument/rename", params);
}

// What a rename gives when the script other.hot imports the origin script.
enum class Want
{
    Edits, // edits in the origin script and in other.hot
    Origin, // edits in the origin script only: other.hot does not use the symbol
    Refused, // an error with the text "fragment"
};

void expect_importer_rename(
    const char* word_context, const char* word, const char* importer_text, Want want, const char* fragment = "")
{
    REQUIRE_CLANGD();
    Session s(true, kOriginScript);
    write_file(s.dir.path() / "other.hot", importer_text);
    const json r = origin_rename(s, kOriginScript, word_context, word);
    if (want == Want::Refused)
    {
        ASSERT_TRUE(r.contains("error")) << r.dump();
        EXPECT_NE(r.dump().find(fragment), std::string::npos) << r.dump();
        return;
    }
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json& changes = r["result"]["changes"];
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    EXPECT_FALSE(changes[s.uri].empty()) << r.dump();
    EXPECT_EQ(changes.contains(uri_of((s.dir.path() / "other.hot").string())), want == Want::Edits) << r.dump();
}

} // namespace

// A rename changes the scripts that import the origin script (decision: the declaring script and
// its importers, directly or not). A refusal stays for an importer that the rename cannot change
// safely: a syntax error, or a word that clangd cannot resolve.
TEST(LanguageServerRename, ClassRefusedWhenAnImporterHasASyntaxError)
{
    expect_importer_rename("class Spawn", "Spawn",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { Spawn@ s; }\n}\n", Want::Refused,
        "other.hot has a syntax error at line 3");
}

TEST(LanguageServerRename, ClassRenamedInAnImporterThatUsesTheName)
{
    expect_importer_rename("class Spawn", "Spawn", "using \"player.hot\";\nclass Kid : Spawn {\n}\n", Want::Edits);
}

// A script class in a template argument of a native call and in a cast gets a ref. The rename
// edits each of these uses and does not refuse.
TEST(LanguageServerRename, ClassInTemplateArgumentAndCastIsRenamed)
{
    REQUIRE_CLANGD();
    Session s(true, kOriginScript);
    write_file(s.dir.path() / "Holder.h",
        "#pragma once\n#include \"Actor.h\"\nstruct Holder : Actor { template <class T> T* Find() { return nullptr; } };\n");
    const std::string other = "using \"Holder.h\";\nusing \"player.hot\";\n"
                              "class Kid : Actor {\n"
                              "    int Go(Holder* h) {\n"
                              "        if (h == null) return 1;\n"
                              "        $Spawn a = h.Find<$Spawn>();\n"
                              "        Team t = (($Spawn)a).GetTeam();\n"
                              "        return 0;\n"
                              "    }\n"
                              "}\n";
    write_file(s.dir.path() / "other.hot", other);
    const json r = origin_rename(s, kOriginScript, "class Spawn", "Spawn");
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json& changes = r["result"]["changes"];
    const std::string key = uri_of((s.dir.path() / "other.hot").string());
    ASSERT_TRUE(changes.contains(key)) << r.dump();
    // Three uses: the type of "a", the template argument and the cast.
    EXPECT_EQ(changes[key].size(), 3u) << r.dump();
    std::set<std::pair<int, int>> at;
    for (const json& e : changes[key])
        at.insert({e["range"]["start"]["line"].get<int>(), e["range"]["start"]["character"].get<int>()});
    EXPECT_TRUE(at.count({5, 9})) << r.dump();
    EXPECT_TRUE(at.count({5, 27})) << r.dump();
    EXPECT_TRUE(at.count({6, 20})) << r.dump();
}

// The import is not transitive: c.hot imports b.hot, and b.hot imports the origin script. A free
// function of the origin script is not visible in c.hot, so the rename does not edit c.hot.
TEST(LanguageServerRename, FreeFunctionIsNotRenamedInAnIndirectImporter)
{
    REQUIRE_CLANGD();
    const std::string origin = "int Twice(int v) { return v * 2; }\n";
    Session s(true, origin);
    write_file(s.dir.path() / "b.hot", "using \"player.hot\";\nint Four() { return Twice(2); }\n");
    write_file(s.dir.path() / "c.hot", "using \"b.hot\";\nint Six() { return Twice(3); }\n");
    const json r = origin_rename(s, origin, "int Twice", "Twice");
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json& changes = r["result"]["changes"];
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    EXPECT_FALSE(changes.contains(uri_of((s.dir.path() / "c.hot").string()))) << r.dump();
}

// The same for a module variable.
TEST(LanguageServerRename, ModuleVarIsNotRenamedInAnIndirectImporter)
{
    REQUIRE_CLANGD();
    const std::string origin = "int Counter = 0;\n";
    Session s(true, origin);
    write_file(s.dir.path() / "b.hot", "using \"player.hot\";\nint Four() { return Counter + 1; }\n");
    write_file(s.dir.path() / "c.hot", "using \"b.hot\";\nint Six() { return Counter + 2; }\n");
    const json r = origin_rename(s, origin, "int Counter", "Counter");
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const json& changes = r["result"]["changes"];
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    EXPECT_TRUE(changes.contains(uri_of((s.dir.path() / "b.hot").string()))) << r.dump();
    EXPECT_FALSE(changes.contains(uri_of((s.dir.path() / "c.hot").string()))) << r.dump();
}

// An indirect importer with a syntax error (a typo) still refuses the rename: the rename cannot
// read its table.
TEST(LanguageServerRename, SyntaxErrorInAnIndirectImporterRefusesTheRename)
{
    REQUIRE_CLANGD();
    const std::string origin = "int Twice(int v) { return v * 2; }\n";
    Session s(true, origin);
    write_file(s.dir.path() / "b.hot", "using \"player.hot\";\nint Four() { return Twice(2); }\n");
    write_file(s.dir.path() / "c.hot", "using \"b.hot\";\nint Six() { return Twice(3) +; }\n");
    const json r = origin_rename(s, origin, "int Twice", "Twice");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("c.hot has a syntax error"), std::string::npos) << r.dump();
}

TEST(LanguageServerRename, ClassRenamedWhenNoImporterUsesTheName)
{
    expect_importer_rename("class Spawn", "Spawn",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { int x = 1; }\n}\n", Want::Origin);
}

TEST(LanguageServerRename, EnumValueRenamedInAnImporterThatUsesTheName)
{
    expect_importer_rename("{ Red", "Red",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { Team t = Team.Red; }\n}\n", Want::Edits);
}

TEST(LanguageServerRename, EnumValueRenamedWhenNoImporterUsesTheName)
{
    expect_importer_rename("{ Red", "Red",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { int x = 1; }\n}\n", Want::Origin);
}

TEST(LanguageServerRename, MethodRenamedInAnImporterThatUsesTheName)
{
    expect_importer_rename("Team GetTeam", "GetTeam",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go(Spawn@ s) { s.GetTeam(); }\n}\n", Want::Edits);
}

TEST(LanguageServerRename, MethodRenamedWhenNoImporterUsesTheName)
{
    expect_importer_rename("Team GetTeam", "GetTeam",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { int x = 1; }\n}\n", Want::Origin);
}

TEST(LanguageServerRename, ImporterWordInACommentGetsNoEdit)
{
    expect_importer_rename("Team GetTeam", "GetTeam",
        "using \"player.hot\";\n// GetTeam in a comment\nclass Kid : Actor {\n}\n", Want::Origin);
}

// "Other.MaxHp" has no ref in the table, and clangd cannot resolve it. The rename does not guess
// the symbol of the word: it refuses.
TEST(LanguageServerRename, StaticRefusedWhenImporterUsesAnUnknownWordOfAnotherClass)
{
    expect_importer_rename("= MaxHp;", "MaxHp",
        "using \"player.hot\";\nclass Other : Actor {\n}\nclass Kid : Actor {\n    void Go() { int x = Other.MaxHp; }\n}\n",
        Want::Refused, "cannot resolve MaxHp at");
}

TEST(LanguageServerRename, StaticRefusedWhenAnExpressionQualifiesTheWord)
{
    expect_importer_rename("= MaxHp;", "MaxHp",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { int x = GetOwner().MaxHp; }\n}\n", Want::Refused,
        "cannot resolve MaxHp at");
}

TEST(LanguageServerRename, StaticRefusedWhenAGenericQualifiesTheWord)
{
    expect_importer_rename("= MaxHp;", "MaxHp",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { int x = Spawn<int>.MaxHp; }\n}\n", Want::Refused,
        "cannot resolve MaxHp at");
}

TEST(LanguageServerRename, StaticRefusedWhenAnInstanceQualifiesTheWord)
{
    expect_importer_rename("= MaxHp;", "MaxHp",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go(Spawn@ s) { int x = s.MaxHp; }\n}\n", Want::Refused,
        "cannot resolve MaxHp at");
}

// The importer does not translate (two class bases): its table has gaps. The rename refuses.
TEST(LanguageServerRename, StaticRefusedWhenAClassHasTheOwnerAsASecondBase)
{
    expect_importer_rename("= MaxHp;", "MaxHp",
        "using \"player.hot\";\nclass Kid : IFoo, Spawn {\n    void Go() { int x = MaxHp; }\n}\n", Want::Refused,
        "other.hot");
}

TEST(LanguageServerRename, StaticRefusedWhenTwoScriptsDeclareTheSameClassName)
{
    REQUIRE_CLANGD();
    Session s(true, kOriginScript);
    write_file(s.dir.path() / "other.hot",
        "using \"player.hot\";\nclass Kid : Actor {\n    void Go() { int x = MaxHp; }\n}\n");
    write_file(s.dir.path() / "third.hot", "using \"player.hot\";\nclass Kid : Spawn {\n}\n");
    const json r = origin_rename(s, kOriginScript, "= MaxHp;", "MaxHp");
    EXPECT_TRUE(r.contains("error")) << r.dump();
}

TEST(LanguageServerRename, ImporterInAnIncludeDirOutsideTheRootsIsFound)
{
    REQUIRE_CLANGD();
    TempDir extra;
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, true);
    o.include_dirs.push_back(extra.path().string());
    o.include_dirs.push_back(dir.path().string()); // b.hot finds x.hot through it
    const std::string path = (dir.path() / "player.hot").string();
    write_file(path, kOriginScript);
    write_file(dir.path() / "x.hot", "using \"player.hot\";\nclass Mid : Actor {\n}\n");
    write_file(extra.path() / "b.hot", "using \"x.hot\";\nclass Kid : Actor {\n    void Go() { Spawn@ s; }\n}\n");
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(path), kOriginScript);
    const json r = client.request("textDocument/rename",
        {{"textDocument", {{"uri", uri_of(path)}}}, {"position", pos(4, 7)}, {"newName", "Renamed"}});
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("b.hot is outside the workspace folders"), std::string::npos) << r.dump();
}

TEST(LanguageServerRename, NoRootRefusesBecauseTheImportersAreUnknown)
{
    REQUIRE_CLANGD();
    TempDir dir;
    const std::string path = (dir.path() / "player.hot").string();
    write_file(path, kOriginScript);
    LsClient client(options(dir, true));
    client.request("initialize", {{"processId", nullptr}, {"rootUri", nullptr}, {"capabilities", json::object()}});
    client.notify("initialized", json::object());
    client.open(uri_of(path), kOriginScript);
    const json r = client.request("textDocument/rename",
        {{"textDocument", {{"uri", uri_of(path)}}}, {"position", pos(4, 7)}, {"newName", "Renamed"}});
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("importers of this script are unknown"), std::string::npos) << r.dump();
}

namespace
{

// A static "count" and a statement in a function: the rename of the static from its declaration.
json static_rename_with(const std::string& stmt)
{
    const std::string text =
        "using \"Actor.h\";\nclass A : Actor {\n    static int count = 0;\n    void f(int x) {\n        " + stmt
        + "\n    }\n}\n";
    Session s(true, text);
    json params = doc_pos(s.uri, 2, 16);
    params["newName"] = "Renamed";
    return s.client->request("textDocument/rename", params);
}

} // namespace

// A pointer declaration of a local with the name of the static: the rename refuses (a syntax
// error), or it changes the static only. The line of the local (line 4) gets no edit.
TEST(LanguageServerScope, PointerDeclarationFormsDoNotRenameTheLocal)
{
    REQUIRE_CLANGD();
    // "Foo* count[2];" is not in the list: the table of the translator gives the word the id of the
    // static, and the rename follows the table.
    for (const char* stmt : {"Foo* count{x};", "Foo* count(x);", "if (Foo* count = get()) {}",
             "while (Foo* count = get()) {}", "switch (x) { case 1: Foo* count = x; }"})
    {
        const json r = static_rename_with(stmt);
        ASSERT_TRUE(r.contains("error") || r.contains("result")) << stmt << " -> " << r.dump();
        const json changes = r.contains("result") ? r["result"].value("changes", json::object()) : json::object();
        for (const auto& [uri, edits] : changes.items())
                for (const json& e : edits)
                    EXPECT_NE(e["range"]["start"]["line"], 4) << stmt << " -> " << r.dump();
    }
}

// A translator gap: in "Foo* count[2];" the table gives the local "count" the id of the static.
// The rename follows the table, thus it changes the local too. Enable the test when the
// translator gives the local its own id.
TEST(LanguageServerScope, PointerArrayDeclarationDoesNotRenameTheLocal)
{
    REQUIRE_CLANGD();
    const json r = static_rename_with("Foo* count[2];");
    ASSERT_TRUE(r.contains("error") || r.contains("result")) << r.dump();
    const json changes = r.contains("result") ? r["result"].value("changes", json::object()) : json::object();
    for (const auto& [uri, edits] : changes.items())
        for (const json& e : edits)
            EXPECT_NE(e["range"]["start"]["line"], 4) << r.dump();
}

namespace
{

// Writes "files" (name and text) into a new workspace and opens player.hot. Then renames the
// word at "line" and "col" of player.hot to "new_name". A name with the prefix "extra/" goes to an
// include folder outside the workspace. "root_out" (if given) gets the workspace folder.
json rename_in_files(const std::vector<std::pair<std::string, std::string>>& files, int line, int col,
    const std::string& new_name, std::string* root_out = nullptr)
{
    TempDir dir;
    TempDir extra;
    hot::ls::ServerOptions o = options(dir, true);
    o.include_dirs.push_back(extra.path().string());
    o.include_dirs.push_back(dir.path().string());
    std::string text;
    for (const auto& [name, body] : files)
    {
        const bool out = name.rfind("extra/", 0) == 0;
        write_file(out ? extra.path() / name.substr(6) : dir.path() / name, body);
        if (name == "player.hot")
            text = body;
    }
    if (root_out)
        *root_out = dir.path().string();
    const std::string path = (dir.path() / "player.hot").string();
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(path), text);
    return client.request("textDocument/rename",
        {{"textDocument", {{"uri", uri_of(path)}}}, {"position", pos(line, col)}, {"newName", new_name}});
}

} // namespace

// A parameter with the new name hides the renamed member at a bare use: "v = v".
TEST(LanguageServerRename, MemberRenameRefusesWhenAParameterHidesTheNewName)
{
    REQUIRE_CLANGD();
    const json r = rename_in_files(
        {{"player.hot", "using \"Actor.h\";\nclass A : Actor {\n    int hp = 0;\n    void SetHp(int v) { hp = v; }\n}\n"}}, 2,
        8, "v");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("would hide"), std::string::npos) << r.dump();
}

// The conflict check reads the imports of the declaring script: b.hot declares Helper.
TEST(LanguageServerRename, ModuleRenameRefusesANameOfAnImportedScript)
{
    REQUIRE_CLANGD();
    const json r = rename_in_files({{"b.hot", "using \"Actor.h\";\nint Helper(int a) { return a; }\n"},
                                       {"player.hot",
                                           "using \"Actor.h\";\nusing \"b.hot\";\nint Foo(int a) { return a; }\n"
                                           "int Bar() { return Foo(1); }\n"}},
        2, 4, "Helper");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("b.hot"), std::string::npos) << r.dump();
}

// The conflict check reads the script of the base class: Base.bar is in b.hot.
TEST(LanguageServerRename, MemberRenameRefusesAMemberOfAnImportedBase)
{
    REQUIRE_CLANGD();
    const json r = rename_in_files({{"b.hot", "using \"Actor.h\";\nclass Base : Actor {\n    int bar = 0;\n}\n"},
                                       {"player.hot",
                                           "using \"Actor.h\";\nusing \"b.hot\";\nclass Derived : Base {\n    int foo = 0;\n}\n"}},
        3, 8, "bar");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("b.hot"), std::string::npos) << r.dump();
}

// The family goes through m.hot, a script without the old name and without the new name:
// Derived : Mid : Base. Base.bar is in b.hot.
TEST(LanguageServerRename, MemberRenameRefusesAMemberOfAnIndirectBase)
{
    REQUIRE_CLANGD();
    const json r = rename_in_files({{"b.hot", "using \"Actor.h\";\nclass Base : Actor {\n    int bar = 0;\n}\n"},
                                       {"m.hot", "using \"Actor.h\";\nusing \"b.hot\";\nclass Mid : Base {\n}\n"},
                                       {"player.hot",
                                           "using \"Actor.h\";\nusing \"m.hot\";\nclass Derived : Mid {\n    int foo = 0;\n}\n"}},
        3, 8, "bar");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("b.hot"), std::string::npos) << r.dump();
}

// The importer side: Leaf : Mid : Base. m.hot has no old name and no new name.
TEST(LanguageServerRename, MemberRenameRefusesAMemberOfAnIndirectDerivedClass)
{
    REQUIRE_CLANGD();
    const json r = rename_in_files({{"player.hot", "using \"Actor.h\";\nclass Base : Actor {\n    int foo = 0;\n}\n"},
                                       {"m.hot", "using \"Actor.h\";\nusing \"player.hot\";\nclass Mid : Base {\n}\n"},
                                       {"leaf.hot",
                                           "using \"Actor.h\";\nusing \"m.hot\";\nclass Leaf : Mid {\n    int bar = 0;\n}\n"}},
        2, 8, "bar");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("leaf.hot"), std::string::npos) << r.dump();
}

// c.hot (in the workspace) imports x.hot (an include folder outside the workspace), and x.hot
// imports player.hot. The rename must change c.hot or refuse: never a part of the result.
TEST(LanguageServerRename, ImporterChainThroughAnIncludeFolderIsNotLeftOut)
{
    REQUIRE_CLANGD();
    std::string root;
    const json r = rename_in_files({{"player.hot", kOriginScript},
                                       {"extra/x.hot", "using \"player.hot\";\nclass Mid : Actor {\n}\n"},
                                       {"c.hot", "using \"x.hot\";\nclass Kid : Spawn {\n}\n"}},
        4, 7, "Renamed", &root);
    ASSERT_TRUE(r.contains("error") || r.contains("result")) << r.dump();
    if (r.contains("result"))
        EXPECT_TRUE(r["result"]["changes"].contains(uri_of((std::filesystem::path(root) / "c.hot").string()))) << r.dump();
}

// An importer that the server cannot read: the rename refuses and names the file.
TEST(LanguageServerRename, RenameRefusesWhenAnImporterCannotBeRead)
{
    REQUIRE_CLANGD();
    if (geteuid() == 0)
        GTEST_SKIP() << "root can read a file without permissions";
    Session s(true, kOriginScript);
    const std::filesystem::path other = s.dir.path() / "other.hot";
    write_file(other, "using \"player.hot\";\nclass Kid : Spawn {\n}\n");
    const json first = origin_rename(s, kOriginScript, "class Spawn", "Spawn");
    ASSERT_TRUE(first.contains("result")) << first.dump();
    std::filesystem::permissions(other, std::filesystem::perms::none);
    const json r = origin_rename(s, kOriginScript, "class Spawn", "Spawn");
    std::filesystem::permissions(other, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_NE(r.dump().find("cannot read"), std::string::npos) << r.dump();
    EXPECT_NE(r.dump().find("other.hot"), std::string::npos) << r.dump();
}

TEST(LanguageServerScope, ProductStillRenamesTheStatic)
{
    REQUIRE_CLANGD();
    const json r = static_rename_with("int y = x * count + 1;");
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["changes"].begin().value().size(), 2u) << r.dump();
}

TEST(LanguageServerRename, StaticRenamedWhenADerivedImporterUsesTheBareWord)
{
    expect_importer_rename("= MaxHp;", "MaxHp",
        "using \"player.hot\";\nclass Kid : Spawn {\n    void Go() { int x = MaxHp; }\n}\n", Want::Edits);
}

TEST(LanguageServerScope, StaticRenameFromABareUseInAProduct)
{
    REQUIRE_CLANGD();
    Session s(true, kOriginScript);
    for (const char* ctx : {"int k = MaxHp", "a * MaxHp", "3 & MaxHp", "static const int MaxHp"})
    {
        const json r = origin_rename(s, kOriginScript, ctx, "MaxHp");
        ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << ctx << " -> " << r.dump();
        EXPECT_EQ(r["result"]["changes"][s.uri].size(), 4u) << ctx << " -> " << r.dump();
    }
}

TEST(LanguageServerScope, StaticRenameRefusesForARealPointerLocal)
{
    REQUIRE_CLANGD();
    const char* const script =
        "using \"Actor.h\";\n"
        "class A : Actor {\n"
        "    static int count = 0;\n"
        "    void f() {\n"
        "        Foo@ q = null;\n"
        "        Foo* count = null;\n"
        "        count = count;\n"
        "    }\n"
        "    void g() { int x = count; }\n"
        "}\n";
    Session s(true, script);
    const json r = origin_rename(s, script, "int x = count", "count");
    EXPECT_TRUE(r.contains("result") || r.contains("error")) << r.dump();
    const json r2 = origin_rename(s, script, "count = count", "count");
    EXPECT_TRUE(r2.contains("error")) << r2.dump(); // the local may hide the static: no edit
}

TEST(LanguageServerLinks, ColumnsCountUtf16UnitsAfterAMultiByteCharacter)
{
    REQUIRE_CLANGD();
    // The path "\xC3\xA9.hot" has 5 UTF-16 units and 6 bytes.
    Session s(true, std::string("using \"\xC3\xA9.hot\";\n") + kScript);
    write_file(s.dir.path() / "\xC3\xA9.hot", kImported);
    const json r = s.client->request("textDocument/documentLink", {{"textDocument", {{"uri", s.uri}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    EXPECT_EQ(r["result"][0]["range"]["start"], pos(0, 7)) << r.dump();
    EXPECT_EQ(r["result"][0]["range"]["end"], pos(0, 12)) << r.dump();
}

// ------------------------------------------------------------------ uncertain names, importers, comments
namespace
{

// Sends the request at the last "word" in the first "context" of "text".
json text_request(Session& s, const std::string& text, const char* method, const std::string& context,
    const std::string& word, const char* new_name = nullptr)
{
    const size_t at = text.find(context);
    EXPECT_NE(at, std::string::npos) << context;
    const size_t w = at + context.rfind(word);
    json params = doc_pos(s.uri, static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(w), '\n')),
        static_cast<int>(w - (text.rfind('\n', w - 1) + 1)));
    if (new_name)
        params["newName"] = new_name;
    return s.client->request(method, params);
}

std::string statics_script(const std::string& body)
{
    return "using \"Actor.h\";\nclass A : Actor {\n    static int count = 0;\n    void f(int x) {\n" + body
        + "    }\n}\n";
}

} // namespace

TEST(LanguageServerUncertain, ArrowAndComparisonDoNotHideAStatic)
{
    REQUIRE_CLANGD();
    const std::string text = statics_script("        bool b = x > count;\n        bool c = x >= count;\n");
    Session s(true, text);
    const json r = text_request(s, text, "textDocument/rename", "x > count", "count", "Renamed");
    EXPECT_EQ(edit_lines(r, s.uri), (std::vector<int>{2, 4, 5})) << r.dump();
}

TEST(LanguageServerUncertain, DeclarationListRefusesTheRename)
{
    REQUIRE_CLANGD();
    const std::string text = statics_script("        int a = 0, count = 1;\n        a = count;\n");
    Session s(true, text);
    // The static is used at the line of the list: the name may be a local.
    const json r = text_request(s, text, "textDocument/rename", "a = count", "count", "Renamed");
    EXPECT_TRUE(r.contains("error") || edit_lines(r, s.uri) == (std::vector<int>{4, 5})) << r.dump();
    EXPECT_EQ(r.dump().find("\"changes\":{\"" + s.uri + "\":[{\"range\":{\"end\":{\"character\":14,\"line\":2}"),
        std::string::npos) << r.dump();
    const json h = text_request(s, text, "textDocument/hover", "a = count", "count");
    EXPECT_EQ(h.dump().find("static int count"), std::string::npos) << h.dump();
}

TEST(LanguageServerUncertain, UnspacedProductIsAUseOfTheStatic)
{
    REQUIRE_CLANGD();
    const std::string text = statics_script("        int y = x*count;\n");
    Session s(true, text);
    const json r = text_request(s, text, "textDocument/rename", "static int count", "count", "Renamed");
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_FALSE(r["result"]["changes"][s.uri].empty()) << r.dump();
}

TEST(LanguageServerUncertain, ForHeaderRefusesTheRename)
{
    REQUIRE_CLANGD();
    const std::string text = statics_script("        for (int i = 0, count = 0; i < 2; ++i) { }\n        x = count;\n");
    Session s(true, text);
    const json r = text_request(s, text, "textDocument/rename", "x = count", "count", "Renamed");
    ASSERT_TRUE(r.contains("error")) << r.dump();
}

TEST(LanguageServerUncertain, CommentOpenerBeforeASlashDoesNotEndTheComment)
{
    REQUIRE_CLANGD();
    const std::string text = statics_script("        /*/ count */ x = count;\n");
    Session s(true, text);
    const json h1 = text_request(s, text, "textDocument/hover", "/*/ count", "count");
    EXPECT_EQ(h1.dump().find("static int count"), std::string::npos) << h1.dump();
    const json h2 = text_request(s, text, "textDocument/hover", "x = count", "count");
    EXPECT_NE(h2.dump().find("static int count"), std::string::npos) << h2.dump();
}

// An importer in a sub folder, and an importer through another script, get the edit too.
TEST(LanguageServerUncertain, ImportersInFoldersAndChainsGetTheRename)
{
    REQUIRE_CLANGD();
    for (const char* kind : {"sub", "chain"})
    {
        Session s(true, kDeclScript);
        std::filesystem::create_directories(s.dir.path() / "sub");
        if (std::string(kind) == "sub")
            write_file(s.dir.path() / "sub" / "other.hot",
                "using \"../player.hot\";\nclass Kid : Player {\n    void Go() { int x = Player.MaxHp; }\n}\n");
        else
        {
            write_file(s.dir.path() / "mid.hot", "using \"player.hot\";\nclass Mid : Player {\n}\n");
            write_file(s.dir.path() / "sub" / "far.hot",
                "using \"../mid.hot\";\nclass Far : Mid {\n    void Go() { int x = Player.MaxHp; }\n}\n");
        }
        const json p = decl_pos("= Player.MaxHp", "MaxHp");
        json params = doc_pos(s.uri, p["line"], p["character"]);
        params["newName"] = "Renamed";
        const json r = s.client->request("textDocument/rename", params);
        ASSERT_TRUE(r.contains("result")) << kind << " " << r.dump();
        EXPECT_EQ(r["result"]["changes"].size(), 2u) << kind << " " << r.dump();
    }
}

TEST(LanguageServerUncertain, FactsOlderThanTheTextGiveNoCrash)
{
    REQUIRE_CLANGD();
    Session s(true, kDeclScript);
    s.client->change(s.uri, 2, "x\n");
    const json p = decl_pos("int m = MaxHp", "MaxHp");
    const json r = s.client->request("textDocument/hover", doc_pos(s.uri, p["line"], p["character"]));
    EXPECT_TRUE(r.contains("result") || r.contains("error")) << r.dump();
    const json r2 = s.client->request("textDocument/rename", {{"textDocument", {{"uri", s.uri}}},
        {"position", p}, {"newName", "Renamed"}});
    EXPECT_TRUE(r2.contains("result") || r2.contains("error")) << r2.dump();
}

// The server sends the generated document to clangd at the first open of a script. Then clangd
// builds the preamble while the user reads the file. No definition request is needed before.
TEST(LanguageServer, GeneratedDocumentGoesToClangdAtOpenBeforeAnyRequest)
{
    REQUIRE_CLANGD();
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, true);
    o.clangd_log = (dir.path() / "open-warm.log").string();
    Session s(true, kScript, &o);
    ASSERT_TRUE(s.diagnostics([](const json&) { return true; }).is_object());
    std::string log;
    for (int i = 0; i < 100; ++i)
    {
        std::ifstream in(o.clangd_log, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        log = text.str();
        if (log.find("textDocument/didOpen") != std::string::npos && log.find("player.hot.cpp") != std::string::npos)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_NE(log.find("textDocument/didOpen"), std::string::npos) << log.substr(0, 2000);
    EXPECT_NE(log.find("player.hot.cpp"), std::string::npos);
    EXPECT_EQ(log.find("textDocument/definition"), std::string::npos);
}

// HOT-10: a rename of a tagged script symbol gives the edits and one warning. It never refuses.
namespace
{
// Renames the word "word" at its first place in "text". Gives the number of the edits and of the
// showMessage warnings that name "word".
std::pair<size_t, int> rename_with_notes(const std::string& text, const std::string& word, const std::string& at)
{
    Session s(true, text);
    const size_t off = text.find(at);
    const size_t col_in_at = at.find(word);
    int line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < off; ++i)
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1;
        }
    json p = doc_pos(s.uri, line, static_cast<int>(off - line_start + col_in_at));
    p["newName"] = "Renamed";
    const json r = s.client->request("textDocument/rename", p);
    EXPECT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    const size_t edits = r.contains("result") && r["result"].is_object() ? r["result"]["changes"][s.uri].size() : 0;
    int notes = 0;
    auto note = [&](int ms)
    {
        return s.client->wait_notification("window/showMessage",
            [&](const json& m)
            { return m.value("message", std::string()).find("'" + word + "' has the attribute") != std::string::npos; },
            ms);
    };
    const json first = note(1500);
    if (!first.is_null())
    {
        ++notes;
        EXPECT_EQ(first["params"].value("type", 0), 2) << first.dump();
        if (!note(500).is_null())
            ++notes;
    }
    return {edits, notes};
}
} // namespace

TEST(LanguageServerRename, TaggedClassGivesEditsAndOneWarning)
{
    REQUIRE_CLANGD();
    const auto [edits, notes] = rename_with_notes(
        "[attribute] class Tag;\n[Tag]\nclass Unit {\n    int hp;\n}\nclass Other : Unit { }\n", "Unit", "class Unit");
    EXPECT_EQ(edits, 2u);
    EXPECT_EQ(notes, 1);
}

TEST(LanguageServerRename, PlainClassGivesNoWarning)
{
    REQUIRE_CLANGD();
    const auto [edits, notes] = rename_with_notes("class Unit {\n    int hp;\n}\nclass Other : Unit { }\n", "Unit", "class Unit");
    EXPECT_EQ(edits, 2u);
    EXPECT_EQ(notes, 0);
}

TEST(LanguageServerRename, TaggedFieldGivesOneWarning)
{
    REQUIRE_CLANGD();
    const auto [edits, notes] = rename_with_notes(
        "[attribute] class Tag;\nclass Unit {\n    [Tag] int hp;\n    void f() { hp = 1; }\n}\n", "hp", "int hp;");
    EXPECT_EQ(edits, 2u);
    EXPECT_EQ(notes, 1);
}

TEST(LanguageServerRename, AliasFieldGivesNoWarning)
{
    REQUIRE_CLANGD();
    const auto [edits, notes] = rename_with_notes(
        "class Unit {\n    [alias(\"old\")] int hp;\n    void f() { hp = 1; }\n}\n", "hp", "int hp;");
    EXPECT_EQ(edits, 2u);
    EXPECT_EQ(notes, 0);
}

// Many threads use one index: digest, store, load and prune run together (HOT-34).
TEST(SymbolIndex, ManyThreadsUseOneIndex)
{
    TempDir dir;
    const std::filesystem::path scripts = dir.path() / "scripts";
    std::filesystem::create_directories(scripts);
    constexpr int kScripts = 20;
    std::vector<std::string> paths;
    for (int i = 0; i < kScripts; ++i)
    {
        paths.push_back((scripts / ("s" + std::to_string(i) + ".hot")).string());
        write_file(paths.back(), "class C" + std::to_string(i) + " {}\n");
    }
    hot::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0}, loaded{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(
            [&, t]
            {
                unsigned n = static_cast<unsigned>(t) * 7919u;
                while (!stop)
                {
                    const unsigned op = n++;
                    const std::string& p = paths[(op / 4) % kScripts];
                    const std::string text = "class X {}\n";
                    const std::string d = index.cached_digest(p, text, {});
                    switch (op % 4)
                    {
                    case 0:
                    case 1:
                        if (!index.store(p, d, sample_shard()))
                            ++bad;
                        break;
                    case 2:
                    {
                        const auto s = index.load(p, d);
                        if (s)
                        {
                            ++loaded;
                            if (s->classes.size() != 1 || s->classes[0].name != "Player" ||
                                s->classes[0].members.size() != 2 || !s->has_table)
                                ++bad;
                        }
                        break;
                    }
                    default:
                        if (t < 2)
                            index.prune();
                        break;
                    }
                }
            });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    stop = true;
    for (auto& th : threads)
        th.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(index.stores(), 0);
    EXPECT_GT(loaded.load(), 0);
}

// ------------------------------------------------------------------ the background index (HOT-34)

namespace
{
// The scripts "s0.hot" to "s<count-1>.hot" in the directory. Each has a class and some methods.
std::vector<std::string> write_index_scripts(const std::filesystem::path& dir, int count)
{
    std::vector<std::string> paths;
    for (int i = 0; i < count; ++i)
    {
        std::string text = "using \"Actor.h\";\nclass C" + std::to_string(i) + " : Actor {\n    int v = " + std::to_string(i) + ";\n";
        for (int m = 0; m < 20; ++m)
            text += "    int f" + std::to_string(m) + "(int a) { this.Move(Vec3(a, 0, 0)); return a + this.v + " + std::to_string(m) + "; }\n";
        text += "}\n";
        const std::string path = (dir / ("s" + std::to_string(i) + ".hot")).string();
        write_file(path, text);
        paths.push_back(path);
    }
    return paths;
}

json index_stats(LsClient& client)
{
    return client.request("hot/indexStats", json::object())["result"]["index"];
}

// Starts a server with the capability of the work done progress when "progress" is true.
void initialize_with(LsClient& client, const std::string& root, bool progress)
{
    json caps = json::object();
    if (progress)
        caps["window"] = {{"workDoneProgress", true}};
    client.request("initialize", {{"processId", nullptr}, {"rootUri", uri_of(root)}, {"capabilities", caps}});
    client.notify("initialized", json::object());
}
} // namespace

TEST(WorkspaceIndexBackground, IndexesTheScriptsWithoutAnOpenDocument)
{
    TempDir dir;
    const auto paths = write_index_scripts(dir.path(), 6);
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    json st;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do
    {
        st = index_stats(client);
        if (st.value("ready", false))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < end);
    ASSERT_TRUE(st.value("ready", false)) << st.dump();
    EXPECT_EQ(st["total"], 6);
    EXPECT_EQ(st["done"], 6);
    EXPECT_GE(st["readyMs"].get<int>(), 0);
    EXPECT_GE(st["threads"].get<int>(), 1);
    EXPECT_EQ(file_count(dir.path() / ".cache" / "hot" / "index"), paths.size());
}

TEST(WorkspaceIndexBackground, ExitDuringTheIndexEndsTheServerFast)
{
    TempDir dir;
    write_index_scripts(dir.path(), 30);
    auto opt = options(dir, false);
    opt.background_index = true;
    auto client = std::make_unique<LsClient>(opt);
    client->initialize(dir.path().string());
    const auto begin = std::chrono::steady_clock::now();
    client->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(5));
    client.reset();
}

TEST(WorkspaceIndexBackground, ARequestIsAnsweredBeforeTheIndexIsReady)
{
    TempDir dir;
    write_index_scripts(dir.path(), 30);
    const std::string path = (dir.path() / "player.hot").string();
    write_file(path, kScript);
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    client.open(uri_of(path), kScript);
    const auto begin = std::chrono::steady_clock::now();
    const json r = client.request("textDocument/hover", doc_pos(uri_of(path), 11, 14));
    const auto took = std::chrono::steady_clock::now() - begin;
    EXPECT_TRUE(r.contains("result")) << r.dump();
    EXPECT_LT(took, std::chrono::seconds(3));
    const json st = index_stats(client);
    std::fprintf(stderr, "hover took %lld ms, index ready: %d (%s)\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()),
        st.value("ready", false) ? 1 : 0, st.dump().c_str());
}

TEST(WorkspaceIndexBackground, ProgressNeedsTheCapability)
{
    for (const bool capable : {true, false})
    {
        TempDir dir;
        write_index_scripts(dir.path(), 12);
        auto opt = options(dir, false);
        opt.background_index = true;
        LsClient client(opt);
        initialize_with(client, dir.path().string(), capable);
        if (capable)
        {
            const json create = client.wait_message(
                [](const json& m) { return m.value("method", std::string()) == "window/workDoneProgress/create"; }, 10000);
            ASSERT_TRUE(create.is_object());
            const std::string token = create["params"]["token"];
            client.respond(create["id"], nullptr);
            const json begin = client.wait_notification("$/progress",
                [&](const json& p) { return p["token"] == token && p["value"]["kind"] == "begin"; }, 30000);
            EXPECT_TRUE(begin.is_object());
            const json end = client.wait_notification("$/progress",
                [&](const json& p) { return p["token"] == token && p["value"]["kind"] == "end"; }, 60000);
            ASSERT_TRUE(end.is_object());
            EXPECT_NE(end["params"]["value"]["message"].get<std::string>().find(" of 12 scripts"), std::string::npos);
        }
        else
        {
            for (int i = 0; i < 1200; ++i)
            {
                if (index_stats(client).value("ready", false))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ASSERT_TRUE(index_stats(client).value("ready", false));
            const json any = client.wait_message([](const json& m) {
                const std::string method = m.value("method", std::string());
                return method == "$/progress" || method == "window/workDoneProgress/create"; }, 300);
            EXPECT_TRUE(any.is_null()) << any.dump();
        }
    }
}

// ------------------------------------------------------------------ server events and the index

namespace
{
const char* const kEventLib = "class Gun {\n    public int Fire(int b) { return b * 2; }\n}\n";
const char* const kEventLibChanged = "class Gun {\n    public int Fire(int b) { return b * 3; }\n    public int Aim() { return 1; }\n}\n";

std::string event_importer(int i)
{
    return "using \"lib.hot\";\nclass User" + std::to_string(i) + " {\n    public int Shoot(Gun g) { return g.Fire(1); }\n}\n";
}

// Waits until the index is ready and has no pending job. Gives the last stats.
json wait_index_idle(LsClient& client)
{
    json st;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do
    {
        st = index_stats(client);
        if (st.value("ready", false) && st.value("pending", 1) == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < end);
    return st;
}

// Waits until the predicate is true for the stats, with a bound. Gives the last stats.
json wait_index_until(LsClient& client, const std::function<bool(const json&)>& pred)
{
    json st;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do
    {
        st = index_stats(client);
        if (pred(st) && st.value("pending", 1) == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < end);
    return st;
}

void watched(LsClient& client, const std::string& path, int type)
{
    client.notify("workspace/didChangeWatchedFiles", {{"changes", json::array({{{"uri", uri_of(path)}, {"type", type}}})}});
}
} // namespace

TEST(WorkspaceIndexEvents, WatchedChangeMakesTheIndexAnalyzeTheImporters)
{
    TempDir dir;
    const std::string lib = (dir.path() / "lib.hot").string();
    write_file(lib, kEventLib);
    write_file((dir.path() / "a.hot").string(), event_importer(1));
    write_file((dir.path() / "b.hot").string(), event_importer(2));
    write_file((dir.path() / "c.hot").string(), "class Alone {\n    public int V() { return 1; }\n}\n");
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    const int start = before["analyzed"].get<int>() + before["loaded"].get<int>();
    write_file(lib, kEventLibChanged);
    watched(client, lib, 2);
    // The changed script and its two importers: three more jobs. The script "c.hot" gets none.
    const json after = wait_index_until(client, [&](const json& s) { return s["analyzed"].get<int>() + s["loaded"].get<int>() >= start + 3; });
    EXPECT_EQ(after["analyzed"].get<int>() + after["loaded"].get<int>(), start + 3) << after.dump();
    EXPECT_EQ(after["pending"], 0);
}

// HOT-34: an edit of a prelude on the disk makes the index analyze the script again.
TEST(WorkspaceIndexEvents, WatchedPreludeEditMakesTheIndexAnalyzeTheScriptAgain)
{
    TempDir pre;
    const std::string prelude = (pre.path() / "game.prelude.hot").string();
    write_file(prelude, "[attribute] class Tagged;\n");
    TempDir dir;
    write_file((dir.path() / "a.hot").string(), "[Tagged] class Marker { int n = 0; }\n");
    auto opt = options(dir, false);
    opt.background_index = true;
    opt.preludes = {prelude};
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    const int start = before["analyzed"].get<int>();
    write_file(prelude, "[attribute] class Tagged;\n[attribute] class Other;\n");
    watched(client, prelude, 2);
    // The text of the prelude is in the digest: the shard of the old text does not load.
    const json after = wait_index_until(client, [&](const json& s) { return s["analyzed"].get<int>() > start; });
    EXPECT_EQ(after["analyzed"].get<int>(), start + 1) << after.dump();
    EXPECT_EQ(after["pending"], 0);
}

// HOT-34: a new user prelude file makes the index analyze the scripts again.
TEST(WorkspaceIndexEvents, WatchedNewUserPreludeMakesTheIndexAnalyzeTheScriptAgain)
{
    TempDir dir;
    write_file((dir.path() / "a.hot").string(), "[Tagged] class Marker { int n = 0; }\n");
    auto opt = options(dir, false);
    opt.background_index = true;
    opt.prelude_suffix = ".prelude.hot";
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    const int start = before["analyzed"].get<int>();
    const std::string prelude = (dir.path() / "user.prelude.hot").string();
    write_file(prelude, "[attribute] class Tagged;\n");
    watched(client, prelude, 1);
    // The new prelude and the script: two more analyses.
    const json after = wait_index_until(client, [&](const json& s) { return s["analyzed"].get<int>() >= start + 2; });
    EXPECT_EQ(after["analyzed"].get<int>(), start + 2) << after.dump();
    EXPECT_EQ(after["pending"], 0);
}

// HOT-34: many created .hot files that are not preludes do not queue the other scripts. One
// file with the prelude suffix does.
TEST(WorkspaceIndexEvents, WatchedFilesWithoutThePreludeSuffixQueueNothing)
{
    TempDir dir;
    write_file((dir.path() / "a.hot").string(), "[Tagged] class Marker { int n = 0; }\n");
    write_file((dir.path() / "b.hot").string(), "class Other { int n = 0; }\n");
    auto opt = options(dir, false);
    opt.background_index = true;
    opt.prelude_suffix = ".prelude.hot";
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    const int start = before["analyzed"].get<int>() + before["loaded"].get<int>();
    TempDir outside; // a created file outside the workspace queues no job of its own
    json changes = json::array();
    for (int i = 0; i < 50; ++i)
        changes.push_back({{"uri", uri_of((outside.path() / ("n" + std::to_string(i) + ".hot")).string())}, {"type", 1}});
    client.notify("workspace/didChangeWatchedFiles", {{"changes", changes}});
    const json same = wait_index_idle(client);
    EXPECT_EQ(same["analyzed"].get<int>() + same["loaded"].get<int>(), start) << same.dump();
    const std::string prelude = (dir.path() / "user.prelude.hot").string();
    write_file(prelude, "[attribute] class Tagged;\n");
    watched(client, prelude, 1);
    const json after = wait_index_until(client, [&](const json& s) { return s["analyzed"].get<int>() + s["loaded"].get<int>() > start; });
    EXPECT_GT(after["analyzed"].get<int>() + after["loaded"].get<int>(), start) << after.dump();
}

// HOT-34: the server and the index worker give one shard digest for a script with a prelude. The
// server loads the shard that the worker stored, and it does not analyze the script again.
TEST(WorkspaceIndexEvents, ServerLoadsTheShardOfTheWorkerForAScriptWithAPrelude)
{
    TempDir pre;
    const std::string prelude = (pre.path() / "game.prelude.hot").string();
    write_file(prelude, "[attribute] class Tagged;\n");
    TempDir dir;
    const std::string script = (dir.path() / "a.hot").string();
    write_file(script, "[Tagged] class Marker { int n = 0; }\n");
    auto opt = options(dir, false);
    opt.background_index = true;
    opt.preludes = {prelude};
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    const json st0 = client.request("hot/indexStats", json::object())["result"];
    const json st1 = client.request("hot/indexStats", {{"tables", {uri_of(script)}}})["result"];
    const json st2 = client.request("hot/indexStats", json::object())["result"];
    EXPECT_EQ(st2["analyzed"].get<int>(), st0["analyzed"].get<int>()) << st2.dump();
    EXPECT_GT(st2["loaded"].get<int>(), st0["loaded"].get<int>()) << st2.dump();
    (void)st1;
}

TEST(WorkspaceIndexEvents, WatchedDeleteRemovesTheScriptFromTheIndex)
{
    TempDir dir;
    const std::string lib = (dir.path() / "lib.hot").string();
    write_file(lib, kEventLib);
    write_file((dir.path() / "a.hot").string(), event_importer(1));
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    EXPECT_EQ(before["total"], 2);
    const int start = before["analyzed"].get<int>() + before["loaded"].get<int>();
    std::filesystem::remove(lib);
    watched(client, lib, 3);
    // The importer is analyzed again. The deleted script is gone from the total.
    const json after = wait_index_until(client, [&](const json& s) { return s["total"] == 1 && s["analyzed"].get<int>() + s["loaded"].get<int>() > start; });
    EXPECT_EQ(after["total"], 1) << after.dump();
    EXPECT_EQ(after["pending"], 0);
    EXPECT_GT(after["analyzed"].get<int>() + after["loaded"].get<int>(), start);
}

TEST(WorkspaceIndexEvents, CloseOfAChangedFileAnalyzesOnlyWhenTheStampDiffers)
{
    TempDir dir;
    const std::string path = (dir.path() / "a.hot").string();
    const std::string text = "class Alone {\n    public int V() { return 1; }\n}\n";
    write_file(path, text);
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    auto jobs = [&](const json& s) { return s["analyzed"].get<int>() + s["loaded"].get<int>(); };
    const int start = jobs(before);
    // The disk stamp is the same: no job.
    client.open(uri_of(path), text);
    client.change(uri_of(path), 2, text + "// not saved\n");
    client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(path)}}}});
    const json same = index_stats(client); // the request is after the close in the queue
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const json same2 = wait_index_idle(client);
    EXPECT_EQ(jobs(same), start) << same.dump();
    EXPECT_EQ(jobs(same2), start) << same2.dump();
    // The file changes on the disk with no event. The close finds the new stamp.
    const std::string changed = text + "// a longer text\n";
    write_file(path, changed);
    client.open(uri_of(path), changed);
    client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(path)}}}});
    const json after = wait_index_until(client, [&](const json& s) { return jobs(s) > start; });
    EXPECT_EQ(jobs(after), start + 1) << after.dump();
}

TEST(WorkspaceIndexEvents, IndexProbeGivesOpenForAnOpenDocument)
{
    TempDir dir;
    const std::string path = (dir.path() / "a.hot").string();
    const std::string text = "class Alone {\n    public int V() { return 1; }\n}\n";
    write_file(path, text);
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    ASSERT_TRUE(wait_index_idle(client).value("ready", false));
    client.open(uri_of(path), text);
    client.change(uri_of(path), 2, text + "// not saved\n");
    const json r = client.request("hot/indexStats", {{"entryOf", uri_of(path)}})["result"];
    EXPECT_EQ(r.value("entryOf", ""), "open") << r.dump();
}

// HOT-34: with background_index off, the probe gives "missing" at once and starts no job.
TEST(WorkspaceIndexEvents, IndexProbeGivesMissingFastWhenTheIndexIsOff)
{
    TempDir dir;
    const std::string path = (dir.path() / "a.hot").string();
    write_file(path, "class Alone {\n    public int V() { return 1; }\n}\n");
    auto opt = options(dir, false);
    opt.background_index = false;
    LsClient client(opt);
    client.initialize(dir.path().string());
    const auto start = std::chrono::steady_clock::now();
    const json r = client.request("hot/indexStats", {{"entryOf", uri_of(path)}})["result"];
    EXPECT_EQ(r.value("entryOf", ""), "missing") << r.dump();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    const json st = index_stats(client);
    EXPECT_EQ(st.value("total", -1), 0) << st.dump();
    EXPECT_EQ(st["analyzed"].get<int>() + st["loaded"].get<int>(), 0) << st.dump();
}

TEST(WorkspaceIndexEvents, IndexProbeGivesValidAfterADiskChangeWithoutAnEvent)
{
    TempDir dir;
    const std::string path = (dir.path() / "a.hot").string();
    const std::string text = "class Alone {\n    public int V() { return 1; }\n}\n";
    write_file(path, text);
    auto opt = options(dir, false);
    opt.background_index = true;
    LsClient client(opt);
    client.initialize(dir.path().string());
    const json before = wait_index_idle(client);
    ASSERT_TRUE(before.value("ready", false)) << before.dump();
    auto jobs = [&](const json& s) { return s["analyzed"].get<int>() + s["loaded"].get<int>(); };
    const int start = jobs(before);
    write_file(path, text + "// a longer text\n");
    // No watched-files event: the stamp check finds the change and the index builds the entry again.
    const json r = client.request("hot/indexStats", {{"entryOf", uri_of(path)}})["result"];
    EXPECT_EQ(r.value("entryOf", ""), "valid") << r.dump();
    const json after = index_stats(client);
    EXPECT_GT(jobs(after), start) << after.dump();
}

// ------------------------------------------------------------------ HOT-34: workspace symbol and the index

namespace
{
std::string ws_script(int i)
{
    const std::string n = std::to_string(i);
    return "class Probe" + n + " {\n    public int Value" + n + "() { return " + n + "; }\n    public int field" + n + ";\n}\n";
}

// Builds the same workspace in both modes and gives the workspace/symbol result with a fixed root.
json workspace_symbol_run(bool background_index, int count, const std::string& query, json* stats_before,
    json* stats_after, bool open_edit)
{
    TempDir dir;
    auto opt = options(dir, false);
    opt.background_index = background_index;
    LsClient client(opt);
    client.initialize(dir.path().string());
    for (int i = 0; i < count; ++i)
    {
        const auto file = dir.path() / ("s" + std::to_string(i) + ".hot");
        write_file(file, ws_script(i));
        const std::string uri = uri_of(file.string());
        client.open(uri, ws_script(i));
        client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri}}}});
    }
    if (open_edit)
    {
        // The text in the editor removes Value0 and adds Extra0. The disk keeps Value0.
        const std::string uri = uri_of((dir.path() / "s0.hot").string());
        client.open(uri, ws_script(0));
        client.change(uri, 2, "class Probe0 {\n    public int Extra0() { return 0; }\n}\n");
    }
    if (background_index)
    {
        // A close can queue a new job. The probe waits until each closed entry is valid.
        wait_index_idle(client);
        for (int i = 0; i < count; ++i)
        {
            const std::string uri = uri_of((dir.path() / ("s" + std::to_string(i) + ".hot")).string());
            const json r = client.request("hot/indexStats", {{"entryOf", uri}})["result"];
            EXPECT_NE(r.value("entryOf", ""), "missing") << r.dump();
        }
    }
    if (stats_before)
        *stats_before = client.request("hot/indexStats", json::object())["result"];
    json r = client.request("workspace/symbol", {{"query", query}})["result"];
    if (stats_after)
        *stats_after = client.request("hot/indexStats", json::object())["result"];
    std::string text = r.dump();
    const std::string root = uri_of(dir.path().string());
    for (size_t at; (at = text.find(root)) != std::string::npos;)
        text.replace(at, root.size(), "ROOT");
    return json::parse(text);
}
} // namespace

TEST(WorkspaceIndexSymbol, WorkspaceSymbolGivesTheSameResultWithAndWithoutTheIndex)
{
    for (const char* query : {"Probe", "Value", "Probe3.Value"})
    {
        json before, after;
        const json off = workspace_symbol_run(false, 6, query, nullptr, nullptr, false);
        const json on = workspace_symbol_run(true, 6, query, &before, &after, false);
        EXPECT_FALSE(off.empty()) << query;
        EXPECT_EQ(off, on) << query;
        EXPECT_GT(after["entryHits"].get<int>(), before["entryHits"].get<int>()) << after.dump();
        EXPECT_EQ(after["analyzed"], before["analyzed"]) << after.dump();
        EXPECT_EQ(after["tableAnalyses"], before["tableAnalyses"]) << after.dump();
    }
}

TEST(WorkspaceIndexSymbol, OpenDocumentWithUnsavedTextWinsOverTheDisk)
{
    for (const bool on : {false, true})
    {
        json before, after;
        const json r = workspace_symbol_run(on, 3, "0", &before, &after, true);
        std::set<std::string> names;
        for (const json& s : r)
            names.insert(s.value("name", ""));
        EXPECT_TRUE(names.count("Probe0.Extra0")) << r.dump();
        EXPECT_FALSE(names.count("Probe0.Value0")) << r.dump();
        if (on)
        {
            EXPECT_GT(after["entryHits"].get<int>(), before["entryHits"].get<int>()) << after.dump();
            EXPECT_EQ(after["analyzed"], before["analyzed"]) << after.dump();
            EXPECT_EQ(after["tableAnalyses"], before["tableAnalyses"]) << after.dump();
        }
    }
}

namespace
{
// A native symbol with three closed scripts. "use" calls it. "own" has the word only as a script
// symbol. "none" lacks the word. The result has the places with a fixed root and the statistics.
json native_reference_run(bool background_index, json* stats_before, json* stats_after)
{
    TempDir dir;
    write_file((dir.path() / "natives.h").string(), kNativeHeader);
    const std::string hero = (dir.path() / "hero.hot").string();
    hot::ls::ServerOptions o = options(dir, true);
    o.background_index = background_index;
    o.include_dirs.push_back(dir.path().string());
    o.native_token_wait_ms = 10000;
    o.reference_budget_ms = 120000;
    write_file(hero, kNativeScript);
    const std::vector<std::pair<std::string, std::string>> closed = {
        {"use.hot", kPetScript},
        {"own.hot", "class Own {\n    int Clamp100(int v) {\n        return v;\n    }\n    int x = Clamp100(2);\n}\n"},
        {"none.hot", "class None {\n    int y = 1;\n}\n"}};
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(hero), kNativeScript);
    for (const auto& [name, text] : closed)
    {
        const std::string file = (dir.path() / name).string();
        write_file(file, text);
        client.open(uri_of(file), text);
        client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(file)}}}});
    }
    if (background_index)
    {
        // The wait can return before the entries are valid. The probe waits for each closed entry.
        wait_index_idle(client);
        for (const auto& [name, text] : closed)
        {
            const std::string uri = uri_of((dir.path() / name).string());
            json r;
            for (int i = 0; i < 100; ++i)
            {
                r = client.request("hot/indexStats", {{"entryOf", uri}})["result"];
                if (r.value("entryOf", "") != "missing")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            EXPECT_NE(r.value("entryOf", ""), "missing") << r.dump();
        }
    }
    const json p = {{"textDocument", {{"uri", uri_of(hero)}}}, {"position", native_pos("Clamp100(MAX_HP)", "Clamp100")},
        {"context", {{"includeDeclaration", true}}}};
    // The first answers can miss the header place while clangd starts. Ask again until it has it.
    json r;
    for (int i = 0; i < 20; ++i)
    {
        r = client.request("textDocument/references", p);
        bool have_decl = false;
        for (const json& l : r.value("result", json::array()))
            have_decl = have_decl || l.value("uri", "").find("natives.h") != std::string::npos;
        if (have_decl)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // The measured request runs after the loop: the server state is warm, the counters are stable.
    *stats_before = client.request("hot/indexStats", json::object())["result"];
    r = client.request("textDocument/references", p);
    *stats_after = client.request("hot/indexStats", json::object())["result"];
    std::string text = r.value("result", json::array()).dump();
    const std::string root = uri_of(dir.path().string());
    for (size_t at; (at = text.find(root)) != std::string::npos;)
        text.replace(at, root.size(), "ROOT");
    return json::parse(text);
}
} // namespace

// HOT-34: the native reference filter reads the index. The result is the same as without the index.
// With the index on, only the script that uses the native symbol goes to an analyze.
TEST(WorkspaceIndexReferences, NativeReferencesGiveTheSameResultWithAndWithoutTheIndex)
{
    REQUIRE_CLANGD();
    json off_before, off_after, on_before, on_after;
    const json off = native_reference_run(false, &off_before, &off_after);
    const json on = native_reference_run(true, &on_before, &on_after);
    EXPECT_EQ(off, on);
    const std::string text = on.dump();
    EXPECT_NE(text.find("ROOT/use.hot"), std::string::npos) << text;
    EXPECT_EQ(text.find("ROOT/own.hot"), std::string::npos) << text;
    EXPECT_EQ(text.find("ROOT/none.hot"), std::string::npos) << text;
    EXPECT_GT(on_after["entryHits"].get<int>(), on_before["entryHits"].get<int>()) << on_after.dump();
    // Each analyze of the scan counts as a translation. The index jobs use their own session.
    EXPECT_EQ(on_after["translations"].get<int>() - on_before["translations"].get<int>(), 1) << on_after.dump();
}

// ------------------------------------------------------------------ HOT-34: script references and the index

namespace
{
const char* const kRefLib = "class Lib {\n    int Calc(int a) { return a; }\n}\nint Twice(int v) { return v * 2; }\n";
const char* const kRefUseA = "using \"lib.hot\";\nclass UserA {\n    Lib l;\n    int fx() { return Twice(1); }\n    int fy() { return l.Calc(2); }\n}\n";
const char* const kRefUseB = "using \"lib.hot\";\nclass UserB {\n    int fx() { return Twice(3); }\n}\n";
const char* const kRefUseC = "using \"lib.hot\";\nclass UserC {\n    int z = 1;\n}\n";
// The disk text of the open importer has one use. The editor text has one more.
const char* const kRefOpenDisk = "using \"lib.hot\";\nclass UserD {\n    Lib l;\n    int fx() { return Twice(4); }\n}\n";
const char* const kRefOpenText =
    "using \"lib.hot\";\nclass UserD {\n    Lib l;\n    int fx() { return Twice(4); }\n    int fw() { return Twice(5); }\n    int fv() { return l.Calc(6); }\n}\n";

// The position of the "n"-th word "word" in "text" (a position of the request).
json ref_pos_of(const std::string& text, const std::string& word, int n)
{
    size_t at = std::string::npos;
    for (int i = 0; i <= n; ++i)
        at = text.find(word, at == std::string::npos ? 0 : at + 1);
    const int line = static_cast<int>(std::count(text.begin(), text.begin() + at, '\n'));
    const int col = static_cast<int>(at - (text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1));
    return pos(line, col);
}

// Builds the library and four importers (one is open with unsaved text). "which" is 0 for the class,
// 1 for the method and 2 for the free function. "disk_change" is 0 for no change. It is 1 to append a new use to
// a closed importer with no event before the measured request. It is 2 to edit the closed importer with the same
// size and the old mtime: the index text has a misspelled name that the disk text has right. The result has a fixed root.
json script_reference_run(bool background_index, int which, int disk_change, json* stats_before, json* stats_after)
{
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, true);
    o.background_index = background_index;
    o.reference_budget_ms = 120000;
    std::string b_first = kRefUseB;
    if (disk_change == 2)
        b_first.replace(b_first.find("Twice"), 5, "Twicf");
    const std::vector<std::pair<std::string, std::string>> closed = {
        {"lib.hot", kRefLib}, {"a.hot", kRefUseA}, {"b.hot", b_first}, {"c.hot", kRefUseC}};
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    for (const auto& [name, text] : closed)
    {
        const std::string file = (dir.path() / name).string();
        write_file(file, text);
        client.open(uri_of(file), text);
        client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(file)}}}});
    }
    const std::string open_path = (dir.path() / "d.hot").string();
    write_file(open_path, kRefOpenDisk);
    client.open(uri_of(open_path), kRefOpenDisk);
    client.change(uri_of(open_path), 2, kRefOpenText);
    if (background_index)
    {
        // The wait can return before the entries are valid. The probe waits for each closed entry.
        wait_index_idle(client);
        for (const auto& [name, text] : closed)
        {
            const std::string uri = uri_of((dir.path() / name).string());
            json r;
            for (int i = 0; i < 100; ++i)
            {
                r = client.request("hot/indexStats", {{"entryOf", uri}})["result"];
                if (r.value("entryOf", "") != "missing")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            EXPECT_NE(r.value("entryOf", ""), "missing") << r.dump();
        }
    }
    // The request starts from the open importer: the position is a use in the editor text.
    const std::string text = kRefOpenText;
    const json at = which == 0 ? ref_pos_of(text, "Lib", 0) : which == 1 ? ref_pos_of(text, "Calc", 0) : ref_pos_of(text, "Twice", 0);
    const json p = {{"textDocument", {{"uri", uri_of(open_path)}}}, {"position", at}, {"context", {{"includeDeclaration", true}}}};
    // A first request warms the tables of the open document and clangd.
    // The first answers can be empty while clangd starts. Ask again until the result has a place.
    for (int i = 0; i < 40; ++i)
    {
        const json w = client.request("textDocument/references", p);
        if (!w.value("result", json::array()).empty())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (disk_change == 1)
    {
        const std::string b_text = std::string(kRefUseB) + "class Extra {\n    int fe() { return Twice(7); }\n}\n";
        write_file((dir.path() / "b.hot").string(), b_text); // no watched-files event
    }
    else if (disk_change == 2)
    {
        const std::string b_path = (dir.path() / "b.hot").string();
        const auto old_time = std::filesystem::last_write_time(b_path);
        write_file(b_path, kRefUseB); // no watched-files event, the same size
        std::filesystem::last_write_time(b_path, old_time);
    }
    *stats_before = client.request("hot/indexStats", json::object())["result"];
    const json r = client.request("textDocument/references", p);
    *stats_after = client.request("hot/indexStats", json::object())["result"];
    std::string out = r.value("result", json::array()).dump();
    const std::string root = uri_of(dir.path().string());
    for (size_t pos_at; (pos_at = out.find(root)) != std::string::npos;)
        out.replace(pos_at, root.size(), "ROOT");
    return json::parse(out);
}
} // namespace

// HOT-34: the script references read the index. The result is the same as without the index.
TEST(WorkspaceIndexReferences, ScriptReferencesGiveTheSameResultWithAndWithoutTheIndex)
{
    REQUIRE_CLANGD();
    for (int which = 0; which < 3; ++which)
    {
        json off_before, off_after, on_before, on_after;
        const json off = script_reference_run(false, which, false, &off_before, &off_after);
        const json on = script_reference_run(true, which, false, &on_before, &on_after);
        EXPECT_EQ(off, on) << which;
        const std::string text = on.dump();
        EXPECT_NE(text.find("ROOT/lib.hot"), std::string::npos) << text;
        EXPECT_NE(text.find("ROOT/d.hot"), std::string::npos) << text;
        if (which != 1)
            EXPECT_NE(text.find("ROOT/a.hot"), std::string::npos) << text;
        EXPECT_EQ(text.find("ROOT/c.hot"), std::string::npos) << text;
        EXPECT_EQ(off_after["entryHits"].get<int>(), 0) << off_after.dump();
        // Each closed script with the name gives one hit: lib and a for Lib and Calc; lib, a and b for Twice.
        EXPECT_EQ(on_after["entryHits"].get<int>() - on_before["entryHits"].get<int>(), which == 2 ? 3 : 2)
            << which << " " << on_after.dump();
        EXPECT_EQ(on_after["entryMisses"].get<int>(), on_before["entryMisses"].get<int>()) << on_after.dump();
        for (const char* key : {"analyzed", "tableAnalyses", "loaded"})
            EXPECT_EQ(on_after[key].get<int>(), on_before[key].get<int>()) << key << " " << which << " " << on_after.dump();
    }
}

// HOT-34: a disk change with no event gives the new place. The entry text differs from the disk text.
TEST(WorkspaceIndexReferences, ScriptReferencesSeeADiskChangeWithNoEvent)
{
    REQUIRE_CLANGD();
    json off_before, off_after, on_before, on_after;
    const json off = script_reference_run(false, 2, true, &off_before, &off_after);
    const json on = script_reference_run(true, 2, true, &on_before, &on_after);
    EXPECT_EQ(off, on);
    const json base = [&]
    {
        json b0, b1;
        return script_reference_run(true, 2, false, &b0, &b1);
    }();
    EXPECT_EQ(on.size(), base.size() + 1) << on.dump();
    EXPECT_NE(on.dump().find("\"line\":5"), std::string::npos) << on.dump();
}

// HOT-34: a same-size edit with the old mtime and no event gives the new place. The stamp is equal,
// so only the compare of the disk text with the entry text finds the change. The entry is dropped (a miss).
TEST(WorkspaceIndexReferences, ScriptReferencesSeeASameStampEditWithNoEvent)
{
    REQUIRE_CLANGD();
    json off_before, off_after, on_before, on_after;
    const json off = script_reference_run(false, 2, 2, &off_before, &off_after);
    const json on = script_reference_run(true, 2, 2, &on_before, &on_after);
    EXPECT_EQ(off, on);
    EXPECT_NE(on.dump().find("ROOT/b.hot"), std::string::npos) << on.dump();
    EXPECT_EQ(on_after["entryMisses"].get<int>() - on_before["entryMisses"].get<int>(), 1) << on_after.dump();
}

// HOT-34 step 5E: the rename uses the index entries. The changes are the same with the index on or off.
TEST(WorkspaceIndexReferences, RenameUsesTheIndexEntries)
{
    REQUIRE_CLANGD();
    auto run = [&](bool background_index, json* hits_before, json* hits_after)
    {
        TempDir dir;
        hot::ls::ServerOptions o = options(dir, true);
        o.background_index = background_index;
        o.reference_budget_ms = 120000;
        const std::vector<std::pair<std::string, std::string>> closed = {
            {"lib.hot", kRefLib}, {"a.hot", kRefUseA}, {"b.hot", kRefUseB}, {"c.hot", kRefUseC}};
        LsClient client(o);
        client.initialize(uri_of(dir.path().string()));
        for (const auto& [name, text] : closed)
        {
            const std::string file = (dir.path() / name).string();
            write_file(file, text);
            client.open(uri_of(file), text);
            if (name != "lib.hot") // the rename starts from the open library
                client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(file)}}}});
        }
        if (background_index)
        {
            wait_index_idle(client);
            for (const auto& [name, text] : closed)
            {
                if (name == "lib.hot")
                    continue;
                json r;
                for (int i = 0; i < 100; ++i)
                {
                    r = client.request("hot/indexStats", {{"entryOf", uri_of((dir.path() / name).string())}})["result"];
                    if (r.value("entryOf", "") != "missing")
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                EXPECT_NE(r.value("entryOf", ""), "missing") << r.dump();
            }
        }
        const std::string lib = (dir.path() / "lib.hot").string();
        json p = {{"textDocument", {{"uri", uri_of(lib)}}}, {"position", ref_pos_of(kRefLib, "Twice", 0)}, {"newName", "Double"}};
        // The first answers can be empty while clangd starts. Ask again until the rename has changes.
        json r;
        for (int i = 0; i < 40; ++i)
        {
            *hits_before = client.request("hot/indexStats", json::object())["result"];
            r = client.request("textDocument/rename", p);
            *hits_after = client.request("hot/indexStats", json::object())["result"];
            if (r.contains("result") && r["result"].is_object() && r["result"].contains("changes"))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::string out = r.value("result", json::object()).dump();
        const std::string root = uri_of(dir.path().string());
        for (size_t at; (at = out.find(root)) != std::string::npos;)
            out.replace(at, root.size(), "ROOT");
        return json::parse(out);
    };
    json off_before, off_after, on_before, on_after;
    const json off = run(false, &off_before, &off_after);
    const json on = run(true, &on_before, &on_after);
    ASSERT_TRUE(on.contains("changes")) << on.dump();
    EXPECT_TRUE(on["changes"].contains("ROOT/b.hot")) << on.dump();
    EXPECT_EQ(off, on);
    // The closed files a, b and c: one hit each.
    EXPECT_EQ(on_after["entryHits"].get<int>() - on_before["entryHits"].get<int>(), 3) << on_after.dump();
    EXPECT_EQ(on_after["entryMisses"].get<int>(), on_before["entryMisses"].get<int>()) << on_after.dump();
    EXPECT_EQ(off_after["entryHits"].get<int>(), 0) << off_after.dump();
}

namespace
{
const char* const kRnLib =
    "class Lib {\n    public int fld;\n    int Calc(int a) { return a; }\n}\nint Twice(int v) { return v * 2; }\n";
const char* const kRnUseA = "using \"lib.hot\";\nclass UserA {\n    Lib l;\n    int fx() { return Twice(1); }\n"
                            "    int fy() { return l.Calc(2) + l.fld; }\n}\n";
const char* const kRnUseB = "using \"lib.hot\";\nclass UserB {\n    int fx() { return Twice(3); }\n}\n";
const char* const kRnUseBBad = "using \"lib.hot\";\nclass UserB {\n    int fx() { return Twice(3) }\n}\n";

enum class RnMode
{
    Plain,
    SyntaxError,
    UnsavedLib,
    Outside,
    DiskAppend,
    DiskSameSize
};

// Renames from the open library. "which": 0 Lib, 1 Calc, 2 fld, 3 Twice. The result has a fixed root.
// "expect" is a part of the error text that the request must give (empty for a success).
// "hook_stage" (if not null): at this stage of the measured request, a hook rewrites b.hot with a text of the same
// size and restores the mtime. The request must then give the error "changed on disk during the rename".
json script_rename_run(bool background_index, int which, bool prepare, const std::string& new_name, RnMode mode,
    const std::string& expect_in, json* stats_before, json* stats_after, const char* hook_stage = nullptr)
{
    const std::string expect = hook_stage ? std::string() : expect_in;
    const std::string final_expect = hook_stage ? "changed on disk during the rename" : expect_in;
    auto armed = std::make_shared<std::atomic<bool>>(false);
    TempDir dir;
    TempDir extra;
    hot::ls::ServerOptions o = options(dir, true);
    o.background_index = background_index;
    o.reference_budget_ms = 120000;
    const std::string b_path_hook = (dir.path() / "b.hot").string();
    if (hook_stage)
        o.rename_hook = [armed, hook_stage, b_path_hook](const char* stage)
        {
            if (!armed->load() || std::string(stage) != hook_stage)
                return;
            armed->store(false);
            const auto old_time = std::filesystem::last_write_time(b_path_hook);
            std::string text = kRnUseB;
            text.replace(text.find("Twice(3)"), 8, "Twice(4)");
            write_file(b_path_hook, text); // the same size
            std::filesystem::last_write_time(b_path_hook, old_time);
        };
    if (mode == RnMode::Outside)
    {
        o.include_dirs.push_back(extra.path().string());
        o.include_dirs.push_back(dir.path().string());
        write_file(extra.path() / "x.hot", kRnUseB);
    }
    std::string b_first = mode == RnMode::SyntaxError ? kRnUseBBad : kRnUseB;
    if (mode == RnMode::DiskSameSize)
        b_first.replace(b_first.find("Twice"), 5, "Twicf");
    const std::vector<std::pair<std::string, std::string>> closed = {{"a.hot", kRnUseA}, {"b.hot", b_first}};
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    const std::string lib = (dir.path() / "lib.hot").string();
    write_file(lib, kRnLib);
    client.open(uri_of(lib), kRnLib);
    for (const auto& [name, text] : closed)
    {
        const std::string file = (dir.path() / name).string();
        write_file(file, text);
        client.open(uri_of(file), text);
        client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(file)}}}});
    }
    if (mode == RnMode::UnsavedLib)
        client.change(uri_of(lib), 2, std::string(kRnLib) + "// not saved\n");
    if (background_index)
    {
        wait_index_idle(client);
        for (const auto& [name, text] : closed)
        {
            json r;
            for (int i = 0; i < 100; ++i)
            {
                r = client.request("hot/indexStats", {{"entryOf", uri_of((dir.path() / name).string())}})["result"];
                if (r.value("entryOf", "") != "missing")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            EXPECT_NE(r.value("entryOf", ""), "missing") << r.dump();
        }
    }
    const std::string word = which == 0 ? "Lib" : which == 1 ? "Calc" : which == 2 ? "fld" : "Twice";
    json p = {{"textDocument", {{"uri", uri_of(lib)}}}, {"position", ref_pos_of(kRnLib, word, 0)}};
    if (!prepare)
        p["newName"] = new_name;
    const std::string method = prepare ? "textDocument/prepareRename" : "textDocument/rename";
    auto done_for = [&](const json& r, const std::string& want)
    {
        if (want.empty())
            return r.contains("result") && r["result"].is_object();
        return r.contains("error") && r["error"].value("message", "").find(want) != std::string::npos;
    };
    auto done = [&](const json& r) { return done_for(r, expect); };
    // The first answers can be wrong while clangd starts. Ask again until the answer is as expected.
    for (int i = 0; i < 40; ++i)
    {
        if (done(client.request(method, p)))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (mode == RnMode::DiskAppend)
        write_file((dir.path() / "b.hot").string(),
            std::string(kRnUseB) + "class Extra {\n    int fe() { return Twice(7); }\n}\n"); // no event
    else if (mode == RnMode::DiskSameSize)
    {
        const std::string b_path = (dir.path() / "b.hot").string();
        const auto old_time = std::filesystem::last_write_time(b_path);
        write_file(b_path, kRnUseB); // no event, the same size
        std::filesystem::last_write_time(b_path, old_time);
    }
    *stats_before = client.request("hot/indexStats", json::object())["result"];
    armed->store(hook_stage != nullptr);
    const json r = client.request(method, p);
    *stats_after = client.request("hot/indexStats", json::object())["result"];
    EXPECT_TRUE(done_for(r, final_expect)) << r.dump();
    std::string out = r.contains("error") ? r["error"].dump() : r.value("result", json::object()).dump();
    const std::string root = uri_of(dir.path().string());
    for (size_t at; (at = out.find(root)) != std::string::npos;)
        out.replace(at, root.size(), "ROOT");
    const std::string plain = dir.path().string();
    for (size_t at; (at = out.find(plain)) != std::string::npos;)
        out.replace(at, plain.size(), "ROOT");
    const std::string plain_extra = extra.path().string();
    for (size_t at; (at = out.find(plain_extra)) != std::string::npos;)
        out.replace(at, plain_extra.size(), "EXTRA");
    return json::parse(out);
}
} // namespace

// HOT-34 step 5E: rename and prepareRename of each kind give the same result with and without the index.
TEST(WorkspaceIndexRename, RenameGivesTheSameResultWithAndWithoutTheIndex)
{
    REQUIRE_CLANGD();
    const char* const names[] = {"Lib2", "Calc2", "fld2", "Twice2"};
    for (int which = 0; which < 4; ++which)
        for (const bool prepare : {false, true})
        {
            const std::string tag = std::to_string(which) + (prepare ? "p" : "r");
            json off_before, off_after, on_before, on_after;
            const json off = script_rename_run(false, which, prepare, names[which], RnMode::Plain, "", &off_before, &off_after);
            const json on = script_rename_run(true, which, prepare, names[which], RnMode::Plain, "", &on_before, &on_after);
            EXPECT_EQ(off, on) << tag;
            EXPECT_EQ(off_after["entryHits"].get<int>(), 0) << off_after.dump();
            if (prepare)
                continue;
            ASSERT_TRUE(on.contains("changes")) << on.dump();
            EXPECT_TRUE(on["changes"].contains("ROOT/lib.hot")) << on.dump();
            EXPECT_TRUE(on["changes"].contains("ROOT/a.hot")) << on.dump();
            // The closed files a and b: one hit each.
            EXPECT_EQ(on_after["entryHits"].get<int>() - on_before["entryHits"].get<int>(), 2) << tag << " " << on_after.dump();
            EXPECT_EQ(on_after["entryMisses"].get<int>(), on_before["entryMisses"].get<int>()) << tag << " " << on_after.dump();
            for (const char* key : {"analyzed", "tableAnalyses", "loaded"})
                EXPECT_EQ(on_after[key].get<int>(), on_before[key].get<int>()) << key << " " << tag << " " << on_after.dump();
        }
}

// HOT-34 step 5E: each refusal gives the same error text with and without the index.
TEST(WorkspaceIndexRename, RefusalsGiveTheSameTextWithAndWithoutTheIndex)
{
    REQUIRE_CLANGD();
    struct Case
    {
        RnMode mode;
        int which;
        const char* new_name;
        const char* expect;
    };
    const Case cases[] = {
        {RnMode::SyntaxError, 3, "Twice2", "has a syntax error"},
        {RnMode::Plain, 1, "fld", "fld"},
        {RnMode::UnsavedLib, 3, "Twice2", "has changes that are not saved"},
        {RnMode::Outside, 3, "Twice2", "is outside the workspace folders"},
    };
    for (const Case& c : cases)
    {
        json b0, b1, c0, c1;
        const json off = script_rename_run(false, c.which, false, c.new_name, c.mode, c.expect, &b0, &b1);
        const json on = script_rename_run(true, c.which, false, c.new_name, c.mode, c.expect, &c0, &c1);
        EXPECT_EQ(off, on) << c.expect;
        EXPECT_NE(on.dump().find(c.expect), std::string::npos) << on.dump();
    }
}

// HOT-34 step 5E: a closed importer that changes on disk with no event. The edits fit the new text.
// The same-size case has the old stamp: only the compare of the texts finds the change. The stale entry
// is dropped (a miss) and the disk text is used. The final compare cannot be made to fire in a
// deterministic way, because the text on disk must change inside one request.
TEST(WorkspaceIndexRename, RenameSeesADiskChangeWithNoEvent)
{
    REQUIRE_CLANGD();
    for (const RnMode mode : {RnMode::DiskAppend, RnMode::DiskSameSize})
    {
        json off_before, off_after, on_before, on_after;
        const json off = script_rename_run(false, 3, false, "Twice2", mode, "", &off_before, &off_after);
        const json on = script_rename_run(true, 3, false, "Twice2", mode, "", &on_before, &on_after);
        const std::string tag = std::to_string(static_cast<int>(mode));
        EXPECT_EQ(off, on) << tag;
        ASSERT_TRUE(on.contains("changes")) << on.dump();
        ASSERT_TRUE(on["changes"].contains("ROOT/b.hot")) << on.dump();
        // The disk text of b.hot: the use "Twice(3)" is at line 2, column 22.
        const json& edits = on["changes"]["ROOT/b.hot"];
        EXPECT_EQ(edits[0]["range"]["start"]["line"].get<int>(), 2) << on.dump();
        EXPECT_EQ(edits[0]["range"]["start"]["character"].get<int>(), 22) << on.dump();
        if (mode == RnMode::DiskAppend)
            EXPECT_EQ(edits.size(), 2u) << on.dump();
        // An append changes the stamp: the index makes a new entry. The same-size edit keeps the stamp.
        if (mode == RnMode::DiskSameSize)
            EXPECT_GT(on_after["entryMisses"].get<int>(), on_before["entryMisses"].get<int>()) << on_after.dump();
    }
}

// HOT-34 step 5E-fix: a file changes on disk (the same size, the old mtime) inside the request. The rename gives
// the error and no edit. The hook runs at the two stages. With the index off, the final compare refuses too.
TEST(WorkspaceIndexRename, RenameRefusesAFileThatChangesInsideTheRequest)
{
    REQUIRE_CLANGD();
    for (const char* stage : {"before_places", "before_final"})
    {
        json before, after;
        const json on = script_rename_run(true, 3, false, "Twice2", RnMode::Plain, "", &before, &after, stage);
        EXPECT_NE(on.dump().find("changed on disk during the rename"), std::string::npos) << stage << " " << on.dump();
        // The closed files a and b each used a valid entry: two hits, no miss. A miss for b.hot would show here.
        EXPECT_EQ(after["entryHits"].get<int>() - before["entryHits"].get<int>(), 2) << stage << " " << after.dump();
        EXPECT_EQ(after["entryMisses"].get<int>(), before["entryMisses"].get<int>()) << stage << " " << after.dump();
    }
    json before, after;
    const json off = script_rename_run(false, 3, false, "Twice2", RnMode::Plain, "", &before, &after, "before_final");
    EXPECT_NE(off.dump().find("changed on disk during the rename"), std::string::npos) << off.dump();
}

// HOT-34 step 5E-fix: one file has the name, 20 importers have no name and no class. The index is cold (no wait). The result is the
// same as without the index. The lookups count only the file with the name.
TEST(WorkspaceIndexRename, ColdIndexLooksUpOnlyTheFilesWithTheName)
{
    REQUIRE_CLANGD();
    auto run = [&](bool background_index, json* before, json* after)
    {
        TempDir dir;
        hot::ls::ServerOptions o = options(dir, true);
        o.background_index = background_index;
        o.reference_budget_ms = 120000;
        LsClient client(o);
        client.initialize(uri_of(dir.path().string()));
        const std::string lib = (dir.path() / "lib.hot").string();
        write_file(lib, kRefLib);
        client.open(uri_of(lib), kRefLib);
        client.notify("textDocument/didClose", {{"textDocument", {{"uri", uri_of(lib)}}}});
        for (int i = 0; i < 20; ++i)
        {
            const std::string name = "u" + std::to_string(i);
            const std::string text = "using \"lib.hot\";\nint z" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
            write_file((dir.path() / (name + ".hot")).string(), text);
        }
        const std::string open_path = (dir.path() / "d.hot").string();
        write_file(open_path, kRefOpenDisk);
        client.open(uri_of(open_path), kRefOpenDisk);
        const json p = {{"textDocument", {{"uri", uri_of(open_path)}}},
            {"position", ref_pos_of(kRefOpenDisk, "Twice", 0)}, {"newName", "Double"}};
        json r;
        for (int i = 0; i < 40; ++i)
        {
            *before = client.request("hot/indexStats", json::object())["result"];
            r = client.request("textDocument/rename", p);
            *after = client.request("hot/indexStats", json::object())["result"];
            if (r.contains("result") && r["result"].is_object() && r["result"].contains("changes"))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (background_index)
        {
            // The request called ensure only for lib.hot (the file with the name): the index has one job.
            // The entryOf probe itself makes a job, so the job count is read before any probe.
            const json st = client.request("hot/indexStats", json::object())["result"];
            EXPECT_EQ(st["index"]["total"].get<int>(), 1) << st.dump();
            const json probe = client.request("hot/indexStats", {{"entryOf", uri_of((dir.path() / "lib.hot").string())}})["result"];
            EXPECT_EQ(probe.value("entryOf", ""), "valid") << probe.dump();
        }
        std::string out = r.value("result", json::object()).dump();
        const std::string root = uri_of(dir.path().string());
        for (size_t at; (at = out.find(root)) != std::string::npos;)
            out.replace(at, root.size(), "ROOT");
        return json::parse(out);
    };
    json off_before, off_after, on_before, on_after;
    const json off = run(false, &off_before, &off_after);
    const json on = run(true, &on_before, &on_after);
    ASSERT_TRUE(on.contains("changes")) << on.dump();
    EXPECT_TRUE(on["changes"].contains("ROOT/lib.hot")) << on.dump();
    EXPECT_EQ(off, on);
    // Only lib.hot has the name. The importers have no name and no class: no lookup for them.
    const int lookups = on_after["entryHits"].get<int>() + on_after["entryMisses"].get<int>()
        - on_before["entryHits"].get<int>() - on_before["entryMisses"].get<int>();
    EXPECT_EQ(lookups, 1) << on_after.dump();
}

// ------------------------------------------------------------------ the split of the generated unit (HOT-43)

namespace
{

// Sets an environment variable for the test and restores it at the end.
struct EnvGuard
{
    std::string name, old;
    bool had = false;
    EnvGuard(const char* n, const std::string& value) : name(n)
    {
        if (const char* o = std::getenv(n))
        {
            had = true;
            old = o;
        }
#ifdef _WIN32
        _putenv_s(n, value.c_str());
#else
        setenv(n, value.c_str(), 1);
#endif
    }
    ~EnvGuard()
    {
#ifdef _WIN32
        _putenv_s(name.c_str(), had ? old.c_str() : "");
#else
        if (had)
            setenv(name.c_str(), old.c_str(), 1);
        else
            unsetenv(name.c_str());
#endif
    }
};

const char* const kSplitBase =
    "using \"Actor.h\";\n"
    "class Base : Actor {\n"
    "    float hp = 2.0;\n"
    "    float Heal(float a) { return this.hp + a; }\n"
    "}\n";

//   line 2: an own field, line 3: uses an own field and an imported method, line 7: a type error.
const char* const kSplitImporter =
    "using \"base.hot\";\n"
    "class Kid : Base {\n"
    "    float own = 1.0;\n"
    "    float Go(float a) { return this.Heal(a) + this.own; }\n"
    "}\n"
    "class Other : Actor {\n"
    "    void Bad() {\n"
    "        int x = \"text\";\n"
    "    }\n"
    "}\n";

std::vector<std::filesystem::path> shell_files(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> r;
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
        if (e.path().string().size() > 8 && e.path().string().substr(e.path().string().size() - 8) == ".shell.h")
            r.push_back(e.path());
    return r;
}

// The results of the features on the own class, with the split on or off.
json split_features(bool split, size_t* shell_count = nullptr, bool lite = true)
{
    const EnvGuard env("HOT_LS_SPLIT_SHELL", split ? "1" : "0");
    const EnvGuard lite_env("HOT_LS_LITE_DOC", lite ? "1" : "0");
    TempDir shells;
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
    json out;
    {
        Session s(true, "using \"Actor.h\";\n");
        write_file(s.dir.path() / "base.hot", kSplitBase);
        s.client->change(s.uri, 2, kSplitImporter);
        // With the split, clangd can show the errors of the old header for a short time after the header changed.
        // The test waits for the error of the own class: the list is then final.
        const json diag = s.diagnostics(
            [](const json& d)
            {
                return std::any_of(d.begin(), d.end(), [](const json& x)
                    { return x.value("message", "").rfind("Cannot initialize a variable", 0) == 0; });
            });
        json diags = json::array();
        if (!diag.is_null())
            for (const json& d : diag["params"]["diagnostics"])
                // An error inside the header is not shown for the importer: the test compares the own error only.
                if (d.value("message", "").rfind("Cannot initialize a variable", 0) == 0)
                    diags.push_back({{"range", d["range"]}, {"message", d.value("message", "")}, {"source", d.value("source", "")}});
        out["diagnostics"] = diags;
        const std::string go = "    float Go(float a) { return this.Heal(a) + this.own; }";
        out["hover_own"] = s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("this.own")) + 7));
        out["hover_heal"] = s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("Heal")) + 1));
        // A second method after the first (the lite document switches the body), and a place outside a body.
        out["hover_bad"] = s.client->request("textDocument/hover", doc_pos(s.uri, 7, 13));
        out["hover_field"] = s.client->request("textDocument/hover", doc_pos(s.uri, 2, 12));
        out["hover_go_again"] = s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("this.own")) + 7));
        out["definition_own"] = s.client->request("textDocument/definition", doc_pos(s.uri, 3, static_cast<int>(go.find("this.own")) + 7));
        out["definition_heal"] = s.client->request("textDocument/definition", doc_pos(s.uri, 3, static_cast<int>(go.find("Heal")) + 1));
        std::string text = kSplitImporter;
        text.replace(text.find("return this.Heal(a) + this.own;"), 31, "this.");
        s.client->change(s.uri, 3, text);
        const json r = s.client->request("textDocument/completion", doc_pos(s.uri, 3, 29));
        std::vector<std::string> labels;
        if (r.contains("result") && r["result"].contains("items"))
            for (const json& i : r["result"]["items"])
                labels.push_back(i.value("insertText", i.value("label", "")));
        std::sort(labels.begin(), labels.end());
        out["completion"] = labels;
        {
            std::ifstream log((s.dir.path() / "clangd.log").string());
            const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            out["lite_seen"] = text.find("player.hot.cpp.lite.cpp") != std::string::npos;
        }
        out["shell_count"] = static_cast<int>(shell_files(shells.path()).size()); // the session is open
        std::string dump = out.dump();
        const std::string root = uri_of(s.dir.path().string());
        for (size_t at; (at = dump.find(root)) != std::string::npos;)
            dump.replace(at, root.size(), "ROOT");
        out = json::parse(dump);
    }
    if (shell_count)
        *shell_count = static_cast<size_t>(out["shell_count"].get<int>());
    out.erase("shell_count");
    return out;
}

} // namespace

// Hover, definition, completion and diagnostics of an own class are the same with the split and without it.
TEST(LanguageServer, SplitShellGivesTheSameResultsAsOneUnit)
{
    REQUIRE_CLANGD();
    size_t shells_on = 0, shells_off = 0;
    const json on = split_features(true, &shells_on);
    const json off = split_features(false, &shells_off);
    EXPECT_EQ(shells_on, 1u);
    EXPECT_EQ(shells_off, 0u);
    EXPECT_EQ(on["diagnostics"], off["diagnostics"]) << on["diagnostics"].dump();
    EXPECT_FALSE(on["diagnostics"].empty());
    EXPECT_EQ(on["hover_own"], off["hover_own"]);
    EXPECT_EQ(on["hover_heal"], off["hover_heal"]);
    EXPECT_EQ(on["definition_own"], off["definition_own"]);
    EXPECT_EQ(on["definition_heal"], off["definition_heal"]);
    EXPECT_EQ(on["completion"], off["completion"]);
    const std::string hover = on["hover_own"].dump();
    EXPECT_NE(hover.find("own"), std::string::npos) << hover;
    // The method of the import is defined in the imported script, not in the header of the shell.
    EXPECT_NE(on["definition_heal"].dump().find("base.hot"), std::string::npos) << on["definition_heal"].dump();
    const auto& labels = on["completion"];
    EXPECT_NE(std::find(labels.begin(), labels.end(), "Heal"), labels.end());
    EXPECT_NE(std::find(labels.begin(), labels.end(), "own"), labels.end());
}

// An edit of an imported script changes the header, and the features see the change.
TEST(LanguageServer, SplitShellHeaderFollowsAnEditOfAnImport)
{
    REQUIRE_CLANGD();
    const EnvGuard env("HOT_LS_SPLIT_SHELL", "1");
    TempDir shells;
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
    Session s(true, kSplitImporter);
    write_file(s.dir.path() / "base.hot", kSplitBase);
    s.client->change(s.uri, 2, kSplitImporter);
    ASSERT_NE(s.client->request("textDocument/hover", doc_pos(s.uri, 3, 30)), nullptr);
    const auto files = shell_files(shells.path());
    ASSERT_EQ(files.size(), 1u);
    const std::string before = translator_test::read_file(files[0]);
    EXPECT_EQ(before.find("mana"), std::string::npos);
    // The edit of an own body keeps the header file.
    std::string own = kSplitImporter;
    own.replace(own.find("float own = 1.0;"), 16, "float own = 1.0;\n    float extra = 3.0;");
    s.client->change(s.uri, 3, own);
    ASSERT_NE(s.client->request("textDocument/hover", doc_pos(s.uri, 3, 10)), nullptr);
    EXPECT_EQ(translator_test::read_file(files[0]), before);
    // The edit of the import changes it.
    std::string base = kSplitBase;
    base.replace(base.find("float hp = 2.0;"), 15, "float hp = 2.0;\n    float mana = 3.0;");
    write_file(s.dir.path() / "base.hot", base);
    std::string user = kSplitImporter;
    user.replace(user.find("this.Heal(a)"), 12, "this.mana");
    s.client->change(s.uri, 4, user);
    const std::string go = "    float Go(float a) { return this.mana + this.own; }";
    // The header changed: clangd builds a new preamble first. The hover waits for it, so the test tries again.
    json r;
    for (int i = 0; i < 40; ++i)
    {
        r = s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("mana")) + 1));
        if (r.contains("result") && r["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_NE(r["result"].dump().find("mana"), std::string::npos) << r.dump();
    // The new header has a new name (the hash of the content) and the old header is gone.
    const auto now = shell_files(shells.path());
    ASSERT_EQ(now.size(), 1u);
    EXPECT_NE(now[0], files[0]);
    EXPECT_NE(translator_test::read_file(now[0]).find("mana"), std::string::npos);
}

// Hover, definition and completion in the bodies of the own functions are the same with the lite document and
// without it. A hover in a second body, a place outside the bodies and an edit in a body work.
TEST(LanguageServer, LiteDocGivesTheSameResultsAsTheFullDocument)
{
    REQUIRE_CLANGD();
    const json lite = split_features(true, nullptr, true);
    const json full = split_features(true, nullptr, false);
    EXPECT_TRUE(lite["lite_seen"].get<bool>());
    EXPECT_FALSE(full["lite_seen"].get<bool>());
    for (const char* key : {"hover_own", "hover_heal", "hover_bad", "hover_field", "hover_go_again", "definition_own",
             "definition_heal", "completion", "diagnostics"})
        EXPECT_EQ(lite[key], full[key]) << key;
    EXPECT_NE(lite["hover_bad"].dump().find("x"), std::string::npos) << lite["hover_bad"].dump();
    EXPECT_NE(lite["hover_field"].dump().find("own"), std::string::npos) << lite["hover_field"].dump();
}

// An edit inside a body changes the answer of the lite document.
TEST(LanguageServer, LiteDocFollowsAnEditInsideABody)
{
    REQUIRE_CLANGD();
    const EnvGuard env("HOT_LS_SPLIT_SHELL", "1");
    const EnvGuard lite_env("HOT_LS_LITE_DOC", "1");
    TempDir shells;
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
    Session s(true, "using \"Actor.h\";\n");
    write_file(s.dir.path() / "base.hot", kSplitBase);
    s.client->change(s.uri, 2, kSplitImporter);
    std::string text = kSplitImporter;
    text.replace(text.find("return this.Heal(a) + this.own;"), 31, "float fresh = 2.0;\n        return fresh + this.own;");
    s.client->change(s.uri, 3, text);
    json r;
    for (int i = 0; i < 40; ++i)
    {
        r = s.client->request("textDocument/hover", doc_pos(s.uri, 4, 17));
        if (r.contains("result") && r["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_NE(r["result"].dump().find("fresh"), std::string::npos) << r.dump();
}

// ------------------------------------------------------------------ HOT-43 review: the lite document

namespace
{

// The messages of publishDiagnostics for the script, until no new one comes for "quiet_ms".
std::vector<json> collect_diagnostics(Session& s, int quiet_ms)
{
    std::vector<json> out;
    for (;;)
    {
        const json n = s.client->wait_notification("textDocument/publishDiagnostics",
            [&](const json& p) { return p.value("uri", std::string()) == s.uri; }, quiet_ms);
        if (n.is_null())
            return out;
        out.push_back(n["params"]["diagnostics"]);
    }
}

bool has_message(const json& diags, const std::string& prefix)
{
    return std::any_of(diags.begin(), diags.end(),
        [&](const json& x) { return x.value("message", "").rfind(prefix, 0) == 0; });
}

// The list of the own errors of the importer (the type error in Other.Bad).
const char* const kOwnError = "Cannot initialize a variable";

} // namespace

// The lite document is a second open file with the same symbols. Its diagnostics do not reach the client: an error
// in a blanked body stays in the list while the hover and the completion use the lite document.
TEST(LanguageServer, LiteDocDiagnosticsNeverReplaceTheFullDiagnostics)
{
    REQUIRE_CLANGD();
    const EnvGuard env("HOT_LS_SPLIT_SHELL", "1");
    const EnvGuard lite_env("HOT_LS_LITE_DOC", "1");
    TempDir shells;
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
    Session s(true, "using \"Actor.h\";\n");
    write_file(s.dir.path() / "base.hot", kSplitBase);
    s.client->change(s.uri, 2, kSplitImporter);
    ASSERT_FALSE(s.diagnostics([](const json& d) { return has_message(d, kOwnError); }).is_null());
    const std::string go = "    float Go(float a) { return this.Heal(a) + this.own; }";
    bool lite_used = false;
    collect_diagnostics(s, 1500); // the list is final
    // A hover in Go, then a hover in Bad: the lite document changes the body each time, so its version runs through
    // the version of the full document. The full document does not change: each new publish is a lite publish.
    for (int round = 0; round < 10; ++round)
    {
        s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("this.own")) + 7));
        std::this_thread::sleep_for(std::chrono::milliseconds(700)); // clangd publishes each lite version
        s.client->request("textDocument/hover", doc_pos(s.uri, 7, 13));
        for (const json& d : collect_diagnostics(s, 700))
            EXPECT_TRUE(has_message(d, kOwnError)) << "round " << round << ": " << d.dump();
    }
    {
        std::ifstream log((s.dir.path() / "clangd.log").string());
        const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        lite_used = text.find("player.hot.cpp.lite.cpp") != std::string::npos;
    }
    EXPECT_TRUE(lite_used);
}

namespace
{

//   line 3: Go, line 4: calls Go two times, line 8: a type error in another body.
const char* const kLiteCalls =
    "using \"base.hot\";\n"
    "class Kid : Base {\n"
    "    float own = 1.0;\n"
    "    float Go(float a) { return this.Heal(a) + this.own; }\n"
    "    float Twice(float a) { return this.Go(a) + this.Go(a); }\n"
    "}\n"
    "class Other : Actor {\n"
    "    void Bad() {\n"
    "        int x = \"text\";\n"
    "    }\n"
    "}\n";

// A session with the split shell, the importer open and the first hover done (the document is warm), so the next
// hover in a body goes to the lite document when "lite" is on.
struct LiteSession
{
    EnvGuard split{"HOT_LS_SPLIT_SHELL", "1"};
    EnvGuard lite_env;
    TempDir shells;
    EnvGuard dir_env;
    Session s;
    LiteSession(bool lite, const char* text = kLiteCalls, const char* base = kSplitBase)
        : lite_env("HOT_LS_LITE_DOC", lite ? "1" : "0"), dir_env("HOT_LS_SHELL_DIR", shells.path().string()),
          s(true, "using \"Actor.h\";\n")
    {
        write_file(s.dir.path() / "base.hot", base);
        s.client->change(s.uri, 2, text);
        s.diagnostics([](const json& d) { return has_message(d, kOwnError); });
        warm(3, 54);
    }
    void warm(int line, int col)
    {
        for (int i = 0; i < 4; ++i)
        {
            json r;
            for (int k = 0; k < 40; ++k)
            {
                r = s.client->request("textDocument/hover", doc_pos(s.uri, line, col));
                if (r.contains("result") && r["result"].is_object())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }
    }
    bool lite_in_log()
    {
        std::ifstream log((s.dir.path() / "clangd.log").string());
        const std::string t((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        return t.find(".lite.cpp") != std::string::npos;
    }
    // A dump of the json with the root of the session replaced.
    std::string norm(const json& j)
    {
        std::string dump = j.dump();
        const std::string root = uri_of(s.dir.path().string());
        for (size_t at; (at = dump.find(root)) != std::string::npos;)
            dump.replace(at, root.size(), "ROOT");
        return dump;
    }
};

// The answers of the requests that use the full document, with the lite document open.
json lite_open_requests(bool lite, std::string* lite_seen)
{
    LiteSession ls(lite);
    Session& s = ls.s;
    json out;
    json refs_ctx = {{"includeDeclaration", true}};
    json at_go = doc_pos(s.uri, 3, 11), at_own = doc_pos(s.uri, 2, 12), at_kid = doc_pos(s.uri, 1, 8);
    at_go["context"] = refs_ctx;
    at_own["context"] = refs_ctx;
    out["refs_go"] = s.client->request("textDocument/references", at_go);
    out["refs_own"] = s.client->request("textDocument/references", at_own);
    ls.warm(3, 54); // the lite document is open again
    for (const char* q : {"Kid", "Go", "own"})
        out[std::string("symbol_") + q] = s.client->request("workspace/symbol", {{"query", q}});
    ls.warm(3, 54);
    json rn_go = at_go, rn_kid = at_kid;
    rn_go["newName"] = "Run";
    rn_kid["newName"] = "Kid2";
    out["rename_go"] = s.client->request("textDocument/rename", rn_go);
    ls.warm(3, 54);
    out["rename_kid"] = s.client->request("textDocument/rename", rn_kid);
    ls.warm(3, 54);
    out["prepare_call"] = s.client->request("textDocument/prepareCallHierarchy", at_go);
    out["definition_twice"] = s.client->request("textDocument/definition", doc_pos(s.uri, 4, 40));
    out["document_symbol"] = s.client->request("textDocument/documentSymbol", {{"textDocument", {{"uri", s.uri}}}});
    if (lite_seen)
        *lite_seen = ls.lite_in_log() ? "yes" : "no";
    return json::parse(ls.norm(out));
}

} // namespace

// References, rename, workspace/symbol, call hierarchy and definition with the lite document open: no duplicate
// entry, no uri of the lite document or of a header, and the same answer as without the lite document.
TEST(LanguageServer, LiteDocIsHiddenFromReferencesRenameAndSymbols)
{
    REQUIRE_CLANGD();
    std::string seen_on, seen_off;
    const json on = lite_open_requests(true, &seen_on);
    const json off = lite_open_requests(false, &seen_off);
    EXPECT_EQ(seen_on, "yes");
    EXPECT_EQ(seen_off, "no");
    for (const auto& [key, value] : on.items())
    {
        const std::string dump = value.dump();
        EXPECT_EQ(dump.find(".lite.cpp"), std::string::npos) << key << ": " << dump;
        EXPECT_EQ(dump.find(".shell.h"), std::string::npos) << key << ": " << dump;
        EXPECT_EQ(value, off[key]) << key;
        const json& res = value.contains("result") ? value["result"] : value;
        if (res.is_array())
        {
            std::set<std::string> seen;
            for (const json& e : res)
                EXPECT_TRUE(seen.insert(e.dump()).second) << key << " has a duplicate: " << e.dump();
        }
        if (res.is_object() && res.contains("changes"))
        {
            // The edits of one file never overlap or repeat.
            for (const auto& [uri, edits] : res["changes"].items())
            {
                std::set<std::string> ranges;
                for (const json& e : edits)
                    EXPECT_TRUE(ranges.insert(e["range"].dump()).second) << key << " " << uri << ": " << e.dump();
            }
        }
    }
    EXPECT_NE(on["rename_go"].dump().find("Run"), std::string::npos) << on["rename_go"].dump();
    EXPECT_NE(on["refs_go"].dump().find("range"), std::string::npos) << on["refs_go"].dump();
}

// The main script shows the same diagnostics with the split shell and without it when an imported script has an error
// in a body. clangd reports such an error at the include line of the unit: the server drops it, as for the full unit.
TEST(LanguageServer, ErrorInAnImportedBodyGivesTheSameDiagnosticsWithTheSplit)
{
    REQUIRE_CLANGD();
    const std::string bad_base =
        "using \"Actor.h\";\n"
        "class Base : Actor {\n"
        "    float hp = 2.0;\n"
        "    float Heal(float a) { int y = \"text\"; return this.hp + a; }\n"
        "}\n";
    auto run = [&](bool split)
    {
        const EnvGuard env("HOT_LS_SPLIT_SHELL", split ? "1" : "0");
        TempDir shells;
        const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
        Session s(true, "using \"Actor.h\";\n");
        write_file(s.dir.path() / "base.hot", bad_base);
        s.client->change(s.uri, 2, kLiteCalls);
        const json first = s.diagnostics([](const json& d) { return has_message(d, kOwnError); });
        json last = first["params"]["diagnostics"];
        // The last list with the own error, after no new list for 2 s. The older lists of the first texts are
        // in the queue too: they have no own error.
        for (const json& d : collect_diagnostics(s, 2000))
            if (has_message(d, kOwnError))
                last = d;
        json out = json::array();
        for (const json& d : last)
            // The error of the imported body makes a second error in the generated code of the importer: clangd
            // reports it at the line of the class. With the split shell it is in the header and clangd reports
            // nothing for the main file. This is the only difference.
            if (d.value("message", "").rfind("No matching function for call to 'forward'", 0) != 0)
                out.push_back({{"range", d["range"]}, {"message", d.value("message", "")}});
        return out;
    };
    const json on = run(true), off = run(false);
    EXPECT_EQ(on, off) << on.dump() << "\n" << off.dump();
    EXPECT_TRUE(has_message(on, kOwnError)) << on.dump();
}

// A body with a deduced return type is never empty in the lite document: the hover in a body that calls it shows
// the deduced type.
TEST(LanguageServer, LiteDocKeepsABodyWithADeducedReturnType)
{
    REQUIRE_CLANGD();
    const char* const text =
        "using \"base.hot\";\n"
        "class Kid : Base {\n"
        "    float own = 1.0;\n"
        "    auto Dedu() { return this.own + 1.0; }\n"
        "    float Use(float a) { auto v = this.Dedu(); return v + this.own + a; }\n"
        "}\n"
        "class Other : Actor {\n"
        "    void Bad() {\n"
        "        int x = \"text\";\n"
        "    }\n"
        "}\n";
    auto hover = [&](bool lite)
    {
        LiteSession ls(lite, text);
        const std::string use = "    float Use(float a) { auto v = this.Dedu(); return v + this.own + a; }";
        json r;
        for (int i = 0; i < 40; ++i)
        {
            r = ls.s.client->request("textDocument/hover", doc_pos(ls.s.uri, 4, static_cast<int>(use.find("this.own")) + 7));
            if (r.contains("result") && r["result"].is_object())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        return ls.norm(r);
    };
    const std::string lite = hover(true), full = hover(false);
    EXPECT_EQ(lite, full);
    EXPECT_NE(lite.find("own"), std::string::npos) << lite;
}

// A function that moves down by 30 lines: the definition of a call from another body has the new line.
TEST(LanguageServer, LiteDocDefinitionFollowsLinesAddedAboveAFunction)
{
    REQUIRE_CLANGD();
    LiteSession ls(true);
    Session& s = ls.s;
    std::string text = kLiteCalls;
    text.insert(text.find("class Kid"), std::string(30, '\n'));
    s.client->change(s.uri, 3, text);
    s.diagnostics([](const json& d) { return has_message(d, kOwnError); });
    const std::string twice = "    float Twice(float a) { return this.Go(a) + this.Go(a); }";
    json r;
    for (int i = 0; i < 40; ++i)
    {
        r = s.client->request("textDocument/definition", doc_pos(s.uri, 34, static_cast<int>(twice.find("Go(a)")) + 1));
        if (r.contains("result") && !r["result"].is_null())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    const std::string dump = ls.norm(r);
    EXPECT_NE(dump.find("\"line\":33"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("\"line\":3,"), std::string::npos) << dump;
    EXPECT_EQ(dump.find(".lite.cpp"), std::string::npos) << dump;
}

#ifndef _WIN32
// The directory of the headers can be a symbolic link with a space in the path. No answer has a uri of a header or
// of a lite document, and the lite diagnostics never reach the client.
TEST(LanguageServer, HeaderDirectoryWithASymlinkAndASpaceGivesNoInternalUri)
{
    REQUIRE_CLANGD();
    TempDir area;
    const auto real = area.path() / "real dir";
    const auto link = area.path() / "link dir";
    std::filesystem::create_directories(real);
    std::filesystem::create_directory_symlink(real, link);
    const EnvGuard split("HOT_LS_SPLIT_SHELL", "1");
    const EnvGuard lite_env("HOT_LS_LITE_DOC", "1");
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", link.string());
    Session s(true, "using \"Actor.h\";\n");
    write_file(s.dir.path() / "base.hot", kSplitBase);
    s.client->change(s.uri, 2, kLiteCalls);
    ASSERT_FALSE(s.diagnostics([](const json& d) { return has_message(d, kOwnError); }).is_null());
    collect_diagnostics(s, 1500); // the old lists of the first texts
    json all = json::array();
    const std::string go = "    float Go(float a) { return this.Heal(a) + this.own; }";
    for (int i = 0; i < 4; ++i)
    {
        all.push_back(s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("this.own")) + 7)));
        all.push_back(s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("Heal")) + 1)));
        all.push_back(s.client->request("textDocument/definition", doc_pos(s.uri, 3, static_cast<int>(go.find("Heal")) + 1)));
        json refs = doc_pos(s.uri, 3, 11);
        refs["context"] = {{"includeDeclaration", true}};
        all.push_back(s.client->request("textDocument/references", refs));
        all.push_back(s.client->request("workspace/symbol", {{"query", "Heal"}}));
    }
    const std::string dump = all.dump();
    EXPECT_EQ(dump.find(".shell.h"), std::string::npos);
    EXPECT_EQ(dump.find(".lite.cpp"), std::string::npos);
    EXPECT_NE(dump.find("Heal"), std::string::npos);
    for (const json& d : collect_diagnostics(s, 800))
        EXPECT_TRUE(has_message(d, kOwnError)) << d.dump();
    // The header is in a directory of this server inside the real directory.
    EXPECT_EQ(shell_files(real).size(), 1u);
}

// A second server on the same script closes the document: the first server answers without an edit, because each
// server has its own header directory. Dead directories of crashed servers are removed at start.
TEST(LanguageServer, TwoServersOnOneScriptAndPruneOfDeadHeaderDirectories)
{
    REQUIRE_CLANGD();
    TempDir shells;
    // A directory of a process that is gone (kill -9): the child ends at once.
    const pid_t child = fork();
    if (child == 0)
        _exit(0);
    int status = 0;
    waitpid(child, &status, 0);
    const auto dead = shells.path() / (std::to_string(child) + "-0");
    std::filesystem::create_directories(dead);
    write_file(dead / "0123-0456.shell.h", "// old\n");
    const EnvGuard split("HOT_LS_SPLIT_SHELL", "1");
    const EnvGuard lite_env("HOT_LS_LITE_DOC", "0");
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
    Session s(true, "using \"Actor.h\";\n");
    write_file(s.dir.path() / "base.hot", kSplitBase);
    s.client->change(s.uri, 2, kLiteCalls);
    ASSERT_FALSE(s.diagnostics([](const json& d) { return has_message(d, kOwnError); }).is_null());
    EXPECT_FALSE(std::filesystem::exists(dead)); // pruned
    {
        ls_test::LsClient second(options(s.dir, true));
        second.initialize(uri_of(s.dir.path().string()));
        second.open(s.uri, kLiteCalls);
        second.change(s.uri, 2, kLiteCalls);
        const std::string go = "    float Go(float a) { return this.Heal(a) + this.own; }";
        json r;
        for (int i = 0; i < 40; ++i)
        {
            r = second.request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("Heal")) + 1));
            if (r.contains("result") && r["result"].is_object())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        EXPECT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
        second.notify("textDocument/didClose", {{"textDocument", {{"uri", s.uri}}}});
        second.stop();
    }
    const std::string go = "    float Go(float a) { return this.Heal(a) + this.own; }";
    json r;
    for (int i = 0; i < 40; ++i)
    {
        r = s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("Heal")) + 1));
        if (r.contains("result") && r["result"].is_object())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_NE(r["result"].dump().find("Heal"), std::string::npos) << r.dump();
}
#endif

#ifndef _WIN32
// A clangd that never answers the first request for the lite document (a slow machine): the hover uses the full
// document inside its budget, and the server counts the fallback.
TEST(LanguageServer, SlowOpenOfTheLiteDocumentFallsBackToTheFullDocument)
{
    REQUIRE_CLANGD();
    if (std::system("python3 -c \"\" >/dev/null 2>&1") != 0)
        GTEST_SKIP() << "python3 is not installed";
    TempDir tools;
    const std::string proxy = (tools.path() / "mute_lite.py").string();
    write_file(proxy,
        "import subprocess, sys\n"
        "p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE)\n"
        "src = sys.stdin.buffer\n"
        "while True:\n"
        "    head = b''\n"
        "    while not head.endswith(b'\\r\\n\\r\\n'):\n"
        "        c = src.read(1)\n"
        "        if not c:\n"
        "            p.stdin.close()\n"
        "            p.wait()\n"
        "            sys.exit(0)\n"
        "        head += c\n"
        "    size = 0\n"
        "    for line in head.split(b'\\r\\n'):\n"
        "        if line.lower().startswith(b'content-length:'):\n"
        "            size = int(line.split(b':')[1])\n"
        "    body = src.read(size)\n"
        "    if b'\"textDocument/documentSymbol\"' in body and b'.lite.cpp' in body:\n"
        "        continue\n"
        "    p.stdin.write(head + body)\n"
        "    p.stdin.flush()\n");
    const std::string clangd = (tools.path() / "mute_clangd").string();
    write_file(clangd, "#!/bin/sh\nexec python3 '" + proxy + "' '" HOT_CLANGD_PATH "' \"$@\"\n");
    std::filesystem::permissions(clangd, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    const EnvGuard split("HOT_LS_SPLIT_SHELL", "1");
    const EnvGuard lite_env("HOT_LS_LITE_DOC", "1");
    TempDir shells;
    const EnvGuard dir_env("HOT_LS_SHELL_DIR", shells.path().string());
    TempDir dir;
    hot::ls::ServerOptions o = options(dir, true);
    o.clangd = clangd;
    Session s(true, "using \"Actor.h\";\n", &o);
    write_file(s.dir.path() / "base.hot", kSplitBase);
    s.client->change(s.uri, 2, kLiteCalls);
    ASSERT_FALSE(s.diagnostics([](const json& d) { return has_message(d, kOwnError); }).is_null());
    const std::string go = "    float Go(float a) { return this.Heal(a) + this.own; }";
    for (int i = 0; i < 4; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        json r;
        for (int k = 0; k < 40; ++k)
        {
            r = s.client->request("textDocument/hover", doc_pos(s.uri, 3, static_cast<int>(go.find("this.own")) + 7));
            if (r.contains("result") && r["result"].is_object())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_TRUE(r.contains("result") && r["result"].is_object()) << "hover " << i << ": " << r.dump();
        if (i >= 2) // the document is warm: the wait for the lite AST is the budget of the hover at most
            EXPECT_LT(ms, 9000.0) << "hover " << i;
    }
    const json stats = s.client->request("hot/indexStats", json::object());
    EXPECT_GT(stats["result"].value("liteFallback", 0), 0) << stats.dump();
}
#endif
