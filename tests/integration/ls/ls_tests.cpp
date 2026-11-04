// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hot-ls (S14b, design M12 to M14). One test for each feature. The tests that need
// clangd are skipped when CMake did not find clangd.
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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using ls_test::json;
using ls_test::LsClient;
using translator_test::TempDir;
using translator_test::write_file;

namespace
{

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
    return "file://" + path;
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

// S30 (c): a clangd error in imported code is published for the imported script, at its own line
// and column. The importing script does not get it.
TEST(LanguageServer, ClangdErrorInAnImportedScriptIsPublishedForThatScript)
{
    REQUIRE_CLANGD();
    std::string imported = kImported;
    imported.replace(imported.find("    float hp = 2.0;\n"), 20, "");
    imported.replace(imported.find("void Boost() { }"), 16, "void Boost() { int x = \"abc\"; }");
    Session s(true); // the importer comes after the imported script is on disk
    write_file(s.dir.path() / "base.hot", imported);
    s.client->change(s.uri, 2, kImporter);
    const std::string base_uri = uri_of((s.dir.path() / "base.hot").lexically_normal().string());
    const json n = s.client->wait_notification("textDocument/publishDiagnostics", [&](const json& p)
    { return p.value("uri", std::string()) == base_uri && has_source(p["diagnostics"], "clangd"); });
    ASSERT_FALSE(n.is_null()) << "no clangd diagnostics for the imported script";
    const json& d = n["params"]["diagnostics"][0];
    EXPECT_EQ(d["range"]["start"]["line"], 4) << n.dump();
    EXPECT_EQ(d["range"]["start"]["character"], 23) << n.dump();
    EXPECT_NE(d.value("message", "").find("initialize"), std::string::npos) << n.dump();
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
    "        Counter* ptr = &local;\n"
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
    explicit NativeSession(const char* script = kNativeScript)
    {
        write_file((dir.path() / "natives.h").string(), kNativeHeader);
        path = (dir.path() / "hero.hot").string();
        uri = uri_of(path);
        write_file(path, script);
        hot::ls::ServerOptions o = options(dir, true);
        o.include_dirs.push_back(dir.path().string());
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
json native_pos(const std::string& context, const std::string& word)
{
    const std::string text = kNativeScript;
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
    const json out = native_calls(s, "callHierarchy/outgoingCalls", run[0]);
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
TEST(LanguageServer, TheKeywordsMatchTheParser)
{
    // C++ words that the parser accepts to give a clear error, and "extern", which is reserved.
    const std::set<std::string> not_highlighted = {
        "static", "thread_local", "friend", "mutable", "volatile", "operator", "extern"};
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
    ASSERT_GT(parser.size(), 30u) << "the parser keywords were not found";
    for (const std::string& w : not_highlighted)
        EXPECT_EQ(parser.erase(w), 1u) << "not a parser word any more: " << w;
    parser.insert(other_parser_words.begin(), other_parser_words.end());

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

// The workspace has two versions of the script. Both declare the class Player. A rename of a
// member in one version changes the other version too, also when it is not open (S18).
TEST(LanguageServer, RenameChangesTheSameSymbolInOtherScriptsOfTheWorkspace)
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
    EXPECT_EQ(changes.size(), 2u) << r.dump();
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    ASSERT_TRUE(changes.contains(uri_of(other_path))) << r.dump();
    std::vector<std::pair<int, int>> at;
    for (const json& e : changes[uri_of(other_path)])
    {
        EXPECT_EQ(e["newText"], "health");
        at.emplace_back(e["range"]["start"]["line"], e["range"]["start"]["character"]);
    }
    std::sort(at.begin(), at.end());
    EXPECT_EQ(at, (std::vector<std::pair<int, int>>{{3, 10}, {11, 13}, {11, 23}})) << r.dump();
}

// The other script is open in the editor too. clangd can answer a definition with the symbol
// of the other generated file. The rename still works in both files.
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
        EXPECT_EQ(changes[s.uri].size(), 3u) << r.dump();
        EXPECT_EQ(changes[uri_of(other_path)].size(), 3u) << r.dump();
    }
}

TEST(LanguageServer, RenameOfAClassChangesTheOtherScripts)
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
    ASSERT_TRUE(changes.contains(uri_of(other_path))) << r.dump();
    ASSERT_FALSE(changes[uri_of(other_path)].empty()) << r.dump();
    EXPECT_EQ(changes[uri_of(other_path)][0]["range"]["start"]["line"], 2);
}

TEST(LanguageServer, ReferencesIncludeTheOtherScriptsOfTheWorkspace)
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
    EXPECT_EQ(there, 3) << r.dump();
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
        hot::compile_commands_json({{dir.path().string(), cpp, hot::compile_command(co, cpp, so), so}}));

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
    ASSERT_FALSE(found.is_null()) << "the index has no script symbol for " << uri_of(script) << "\n"
                                   << last.dump().substr(0, 3000);
    EXPECT_EQ(found["name"], "Player.heal");
    EXPECT_EQ(found["location"]["range"]["start"]["line"], 10);
}

// S31 (DECISIONS 18): a C++ clangd (the editor extension) and hot-ls run on one project. The
// project has one compile_commands.json and one ".cache" directory. The clangd child of hot-ls
// keeps its index in ".cache/hot/clangd", so it does not mix with the index of the C++ clangd.
TEST(LanguageServer, CppClangdAndTheServerRunOnOneProjectWithoutDisturbingEachOther)
{
    REQUIRE_CLANGD();
    namespace fs = std::filesystem;
    TempDir dir;
    const fs::path root = dir.path();
    const std::string script = (root / "player.hot").string();
    write_file(script, kScript);
    hotc::TranslateOptions o;
    o.script_text = kScript;
    o.script_name = script;
    o.generation = 1;
    o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
    const hotc::TranslateResult t = hotc::translate(o);
    ASSERT_TRUE(t.ok) << t.diagnostics;
    const std::string gen = (root / "player.g1.cpp").string();
    write_file(gen, t.cpp);
    const std::string cpp_text = "int cpp_function() { return 1; }\nint cpp_bad() { int x = \"abc\"; return x; }\n";
    const std::string cpp = (root / "main.cpp").string();
    write_file(cpp, cpp_text);
    hot::ClangFilesOptions co;
    co.include_dirs = o.include_dirs;
    write_file(root / "compile_commands.json",
        hot::compile_commands_json({{root.string(), gen, hot::compile_command(co, gen, gen + ".so"), gen + ".so"},
            {root.string(), cpp, hot::compile_command(co, cpp, cpp + ".so"), cpp + ".so"}}));

    // The C++ clangd, as the clangd extension starts it.
    std::mutex mu;
    std::vector<json> published;
    hot::ls::ClangdProcess cpp_clangd;
    ASSERT_TRUE(cpp_clangd.start(HOT_CLANGD_PATH,
        {"--log=error", "--background-index", "--compile-commands-dir=" + root.string()}, "", [&](const json& m)
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
    hot::ls::ServerOptions opt = options(dir, true);
    opt.include_dirs.clear();
    opt.compile_commands_dir = root.string();
    LsClient client(opt);
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

    // Both background indexes run. The shards of the two clangd processes are in two directories.
    const fs::path cpp_index = root / ".cache" / "clangd" / "index";
    const fs::path hot_index = root / ".cache" / "hot" / "clangd" / ".cache" / "clangd" / "index";
    auto has_shard = [](const fs::path& dir_, const std::string& stem)
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir_, ec))
            if (e.path().filename().string().rfind(stem, 0) == 0)
                return true;
        return false;
    };
    bool ready = false;
    for (int i = 0; i < 120 && !ready; ++i)
    {
        ready = has_shard(cpp_index, "main.cpp.") && has_shard(hot_index, "main.cpp.");
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT_TRUE(ready) << "a background index is missing";
    EXPECT_TRUE(fs::exists(root / ".cache" / "hot" / "clangd" / "compile_commands.json"));

    // The C++ clangd still answers for C++, and hot-ls still answers for the script.
    const json sym = cpp_clangd.request("workspace/symbol", {{"query", "cpp_function"}}, 30000);
    ASSERT_TRUE(sym.contains("result")) << sym.dump();
    EXPECT_FALSE(sym["result"].empty()) << sym.dump();
    const json hover = client.request("textDocument/hover", doc_pos(uri_of(script), 3, 11));
    EXPECT_TRUE(hover.contains("result")) << hover.dump();
    cpp_clangd.stop();
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

} // namespace

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
    ASSERT_TRUE(index.store(script, d2, hot::ls::IndexShard{}));
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

// A class rename in a closed file. The first server analyzes the file and saves the shard.
// A new server after a restart loads the shard and analyzes no file.
TEST(LanguageServer, RenameOnAClosedFileAfterARestartUsesTheIndex)
{
    Session s(false);
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    write_file(other_path, kScript);
    write_file(s.dir.path() / "unrelated.hot", "class Other {\n    float hp = 1.0;\n}\n");
    json p = doc_pos(s.uri, 2, 7);
    p["newName"] = "Hero";
    json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    ASSERT_TRUE(r["result"]["changes"].contains(uri_of(other_path))) << r.dump();
    const json first = r["result"]["changes"][uri_of(other_path)];
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_TRUE(std::filesystem::exists(s.dir.path() / ".cache" / "hot" / "index"));
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
    std::string changed = kScript;
    changed += "\n";
    write_file(other_path, changed);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_EQ(file_count(s.dir.path() / ".cache" / "hot" / "index"), shards);
}

// The same for a member. The references of a member come from clangd. The shard keeps them.
TEST(LanguageServer, MemberRenameOnAClosedFileAfterARestartUsesTheIndex)
{
    REQUIRE_CLANGD();
    Session s(true);
    hot::ls::ServerOptions o = options(s.dir, true);
    o.index_dir = (s.dir.path() / "my-index").string();
    s.client.reset();
    s.client = std::make_unique<LsClient>(o);
    s.client->initialize(uri_of(s.dir.path().string()));
    s.client->open(s.uri, kScript);
    const std::string other_path = (s.dir.path() / "player_v2.hot").string();
    write_file(other_path, kScript);
    json p = doc_pos(s.uri, 3, 11);
    p["newName"] = "health";
    json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    ASSERT_TRUE(r["result"]["changes"].contains(uri_of(other_path))) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)].size(), 3u) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_FALSE(std::filesystem::exists(s.dir.path() / ".cache")); // --index-dir is used
    s.client.reset();
    s.client = std::make_unique<LsClient>(o);
    s.client->initialize(uri_of(s.dir.path().string()));
    s.client->open(s.uri, kScript);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)].size(), 3u) << r.dump();
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
Completion complete_at(bool clangd, std::string text, const std::string& replace = "")
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
    Completion c;
    json r = s.client->request("textDocument/completion", doc_pos(s.uri, line, col));
    c.result = r.contains("result") ? r["result"] : json::object();
    for (const json& i : c.result.value("items", json::array()))
        c.names.push_back(i.value("insertText", i.value("label", "")));
    return c;
}

} // namespace

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
    }
    for (const char* n : {"self", "st", "a"})
        EXPECT_FALSE(c.has(n)) << n;
}

int completion_translations(LsClient& client)
{
    return client.request("hot/indexStats", json::object())["result"].value("completions", -1);
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
// references of a symbol in an open script include the new file.
TEST(LanguageServerHosts, NewScriptFileIsFoundAfterAWatchedFilesEvent)
{
    REQUIRE_CLANGD();
    TempDir dir;
    const std::filesystem::path root = dir.path();
    std::filesystem::create_directories(root / "hosts");
    const std::string text = "class Player {\n    float hp = 1.0;\n}\n";
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
            {{"textDocument", {{"uri", uri_of(path)}}}, {"position", {{"line", 1}, {"character", 11}}},
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
    write_file(other, text);
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
// still finds the same method in the other script, although the interface has the name too.
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
    EXPECT_TRUE(changes.contains(uri_of(other_path))) << r.dump();
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
    LsClient client(options(dir, false));
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

    change_folders(client, {folder_json(two)}, json::array());
    EXPECT_TRUE(has_symbol(symbols(client, "bravo"), "Bravo", second));
    EXPECT_EQ(shard_count(client), 2);

    change_folders(client, json::array(), {folder_json(two)});
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
    change_folders(client, {folder_json(two)}, json::array());
    const std::string second = fill(client, two / "second.hot", "Bravo");
    EXPECT_TRUE(has_symbol(symbols(client, "bravo"), "Bravo", second));
    change_folders(client, {folder_json(two, "/")}, json::array()); // not a new folder
    change_folders(client, json::array(), {folder_json(two, "/")});
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
    change_folders(client, {folder_json(one)}, json::array());
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
