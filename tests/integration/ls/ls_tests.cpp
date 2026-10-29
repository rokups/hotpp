// Tests for spp-ls (S14b, design M12 to M14). One test for each feature. The tests that need
// clangd are skipped when CMake did not find clangd.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "clangd_process.h"
#include "integration/ls/ls_client.h"
#include "spp/clang_files_backend.h"
#include "spp/sppc_lib.h"
#include "symbol_index.h"
#include "support/translator_test_support.h"

using ls_test::json;
using ls_test::LsClient;
using translator_test::TempDir;
using translator_test::write_file;

namespace {

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

bool have_clangd() { return std::string(SPP_CLANGD_PATH).size() > 0; }

#define REQUIRE_CLANGD()                                              \
    do {                                                              \
        if (!have_clangd()) GTEST_SKIP() << "clangd is not installed"; \
    } while (0)

spp::ls::ServerOptions options(const TempDir& dir, bool clangd)
{
    spp::ls::ServerOptions o;
    o.clangd = clangd ? SPP_CLANGD_PATH : "";
    o.include_dirs = {SPP_SOURCE_DIR "/include", SPP_SOURCE_DIR "/example/engine"};
    // SPP_LS_CLANGD_LOG keeps the clangd log after the test (the temporary dir goes away).
    const char* log = std::getenv("SPP_LS_CLANGD_LOG");
    o.clangd_log = log ? log : (dir.path() / "clangd.log").string();
    return o;
}

std::string uri_of(const std::string& path) { return "file://" + path; }

json pos(int line, int character) { return {{"line", line}, {"character", character}}; }

json doc_pos(const std::string& uri, int line, int character)
{
    return {{"textDocument", {{"uri", uri}}}, {"position", pos(line, character)}};
}

// A server with the script open. The script file is on disk too.
struct Session {
    TempDir dir;
    std::string path, uri;
    std::unique_ptr<LsClient> client;
    explicit Session(bool clangd, const std::string& text = kScript, spp::ls::ServerOptions* o = nullptr)
    {
        path = (dir.path() / "player.spp").string();
        uri = uri_of(path);
        write_file(path, text);
        client = std::make_unique<LsClient>(o ? *o : options(dir, clangd));
        client->initialize(uri_of(dir.path().string()));
        client->open(uri, text);
    }
    // Waits for the diagnostics of the script that "match" accepts.
    json diagnostics(const std::function<bool(const json&)>& match)
    {
        return client->wait_notification("textDocument/publishDiagnostics", [&](const json& p) {
            return p.value("uri", std::string()) == uri && match(p["diagnostics"]);
        });
    }
};

bool has_source(const json& diags, const std::string& source)
{
    return std::any_of(diags.begin(), diags.end(), [&](const json& d) { return d.value("source", "") == source; });
}

}  // namespace

// ------------------------------------------------------------------ M12: diagnostics

TEST(LanguageServer, ParseErrorIsAtTheScriptPosition)
{
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.hp = ;");
    Session s(false, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "spp"); });
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
    for (const json& d : n["params"]["diagnostics"]) {
        if (d.value("source", "") != "clangd") continue;
        const std::string msg = d.value("message", "");
        if (msg.find("initialize") == std::string::npos) continue;
        found = true;
        // clangd puts the error on the variable "x" (character 12).
        EXPECT_EQ(d["range"]["start"]["line"], 11) << d.dump();
        EXPECT_EQ(d["range"]["start"]["character"], 12) << d.dump();
        EXPECT_NE(msg.find("'int'"), std::string::npos) << msg;
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
    for (;;) {
        const json n = s.client->wait_notification("textDocument/publishDiagnostics",
                                                   [&](const json& p) { return p.value("uri", std::string()) == s.uri; }, 5000);
        if (n.is_null()) break;
        last = n;
    }
    ASSERT_FALSE(last.is_null());
    bool found = false;
    for (const json& d : last["params"]["diagnostics"])
        if (d.value("source", "") == "clangd" && d["range"]["start"]["line"] == 11) found = true;
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
    for (const json& i : r["result"]["items"]) labels.push_back(i.value("insertText", i.value("label", "")));
    auto has = [&](const char* n) { return std::find(labels.begin(), labels.end(), n) != labels.end(); };
    EXPECT_TRUE(has("Move")) << r.dump();      // a native member of Actor
    EXPECT_TRUE(has("Position")) << r.dump();  // a native field of Actor
    EXPECT_TRUE(has("hp"));                    // a script field
    EXPECT_TRUE(has("heal"));                  // a script method
    EXPECT_FALSE(has("cls"));                  // spp::ShellBase is hidden
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

// S30 (a): the translation fails, so clangd keeps the last good C++. The positions go through
// the edit. The call of heal moves two lines down. A missing import makes the translation fail. The definition is still found.
TEST(LanguageServer, DefinitionAfterATranslationErrorUsesTheShiftedPositions)
{
    REQUIRE_CLANGD();
    Session s(true);
    ASSERT_TRUE(s.client->request("textDocument/definition", doc_pos(s.uri, 7, 14)).contains("result"));
    std::string text = kScript;
    text.replace(text.find("\nclass Player"), 0, "using \"missing.spp\";\n\n");
    s.client->change(s.uri, 2, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "spp"); });
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
    const json n = s.diagnostics([](const json& d) {
        return std::any_of(d.begin(), d.end(), [](const json& x) { return x.value("message", "").find("not viable") != std::string::npos; });
    });
    ASSERT_FALSE(n.is_null());
    for (const json& d : n["params"]["diagnostics"]) {
        if (d.value("message", "").find("not viable") == std::string::npos) continue;
        EXPECT_EQ(d["range"]["start"], pos(10, 9)) << d.dump();  // the name "heal" in the definition line
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
    "using \"base.spp\";\n"
    "class Kid : Base {\n"
    "    void Go() { this.Boost(); }\n"
    "}\n";

// A translator error in an imported script is published for that script, at its own line and
// column. The importing script gets no error at that position.
TEST(LanguageServer, TranslatorErrorInAnImportedScriptIsPublishedForThatScript)
{
    Session s(false, kImporter);
    write_file(s.dir.path() / "base.spp", kImported);
    s.client->change(s.uri, 2, kImporter);
    const std::string base_uri = uri_of((s.dir.path() / "base.spp").lexically_normal().string());
    const json n = s.client->wait_notification("textDocument/publishDiagnostics", [&](const json& p) {
        return p.value("uri", std::string()) == base_uri && !p["diagnostics"].empty();
    });
    ASSERT_FALSE(n.is_null()) << "no diagnostics for the imported script";
    const json& d = n["params"]["diagnostics"][0];
    EXPECT_EQ(d["range"]["start"]["line"], 3) << n.dump();
    EXPECT_EQ(d["range"]["start"]["character"], 10) << n.dump();
    EXPECT_EQ(d["source"], "spp");
}

// S30 (c): a clangd error in imported code is published for the imported script, at its own line
// and column. The importing script does not get it.
TEST(LanguageServer, ClangdErrorInAnImportedScriptIsPublishedForThatScript)
{
    REQUIRE_CLANGD();
    std::string imported = kImported;
    imported.replace(imported.find("    float hp = 2.0;\n"), 20, "");
    imported.replace(imported.find("void Boost() { }"), 16, "void Boost() { int x = \"abc\"; }");
    Session s(true);  // the importer comes after the imported script is on disk
    write_file(s.dir.path() / "base.spp", imported);
    s.client->change(s.uri, 2, kImporter);
    const std::string base_uri = uri_of((s.dir.path() / "base.spp").lexically_normal().string());
    const json n = s.client->wait_notification("textDocument/publishDiagnostics", [&](const json& p) {
        return p.value("uri", std::string()) == base_uri && has_source(p["diagnostics"], "clangd");
    });
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
    write_file(s.dir.path() / "base.spp", imported);
    s.client->change(s.uri, 2, kImporter);
    const json r = s.client->request("textDocument/definition", doc_pos(s.uri, 2, 21));
    ASSERT_TRUE(r.contains("result") && r["result"].is_array() && !r["result"].empty()) << r.dump();
    const json& loc = r["result"][0];
    EXPECT_EQ(loc["uri"], uri_of((s.dir.path() / "base.spp").lexically_normal().string())) << r.dump();
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

TEST(LanguageServer, ReferencesOfAScriptFieldAreInTheScript)
{
    REQUIRE_CLANGD();
    Session s(true);
    const json r = s.client->request("textDocument/references",
                                     {{"textDocument", {{"uri", s.uri}}}, {"position", pos(11, 14)},
                                      {"context", {{"includeDeclaration", true}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    std::vector<std::pair<int, int>> at;
    for (const json& l : r["result"]) {
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
    for (const json& m : c["children"]) names.push_back(m["name"]);
    EXPECT_EQ(names, (std::vector<std::string>{"hp", "Tick", "heal"}));
    EXPECT_EQ(c["children"][0]["kind"], 8);  // Field
    EXPECT_EQ(c["children"][2]["kind"], 6);  // Method
    EXPECT_EQ(c["children"][2]["selectionRange"]["start"]["line"], 10);
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
    for (size_t i = 0; i < data.size(); i += 5) {
        line += data[i].get<int>();
        col = data[i].get<int>() == 0 ? col + data[i + 1].get<int>() : data[i + 1].get<int>();
        tokens.emplace_back(line, col, data[i + 2].get<int>(), init_types[data[i + 3].get<int>()]);
    }
    auto has = [&](int l, int c, int n, const char* t) {
        return std::find(tokens.begin(), tokens.end(), std::make_tuple(l, c, n, std::string(t))) != tokens.end();
    };
    EXPECT_TRUE(has(0, 0, 5, "keyword"));   // using
    EXPECT_TRUE(has(0, 6, 9, "string"));    // "Actor.h"
    EXPECT_TRUE(has(2, 6, 6, "class"));     // Player
    EXPECT_TRUE(has(2, 15, 5, "class"));    // Actor
    EXPECT_TRUE(has(3, 4, 5, "type"));      // float
    EXPECT_TRUE(has(3, 10, 2, "property")); // hp
    EXPECT_TRUE(has(3, 15, 5, "number"));   // 100.0
    EXPECT_TRUE(has(7, 13, 4, "method"));   // heal
}

// ------------------------------------------------------------------ M14: index, C++ to script, rename

// A .cpp go-to-definition that ends in a generated .cpp file goes on to the script with the
// request spp/toScript. The "#line" directives of the file give the script line.
TEST(LanguageServer, GeneratedFileOnDiskMapsBackToTheScript)
{
    Session s(false);
    // Wait for the analyze of the server. Two parses at the same time race inside libclang
    // (TSan reports it), and the test does not test this.
    s.diagnostics([](const json&) { return true; });
    sppc::TranslateOptions o;
    o.script_text = kScript;
    o.script_name = s.path;
    o.generation = 1;
    o.include_dirs = {SPP_SOURCE_DIR "/include", SPP_SOURCE_DIR "/example/engine"};
    const sppc::TranslateResult t = sppc::translate(o);
    ASSERT_TRUE(t.ok) << t.diagnostics;
    const std::string cpp = (s.dir.path() / "player.g1.cpp").string();
    write_file(cpp, t.cpp);
    // The generated line of "this.hp = this.hp + amount;" (0-based).
    const size_t at = t.cpp.find("__st->hp = __st->hp + amount;");
    ASSERT_NE(at, std::string::npos);
    const int gline = static_cast<int>(std::count(t.cpp.begin(), t.cpp.begin() + static_cast<long>(at), '\n'));
    const json r = s.client->request("spp/toScript", doc_pos(uri_of(cpp), gline, 4));
    ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump();
    EXPECT_EQ(r["result"]["uri"], s.uri);
    EXPECT_EQ(r["result"]["range"]["start"]["line"], 11);
    // A line of translator text (the shell) gets the last script line before it.
    const auto direct = spp::ls::line_directive_target(t.cpp, gline + 1);
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
    for (const json& e : edits) {
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
    const std::string other_path = (s.dir.path() / "sub" / "player_v2.spp").string();
    std::string v2 = kScript;
    v2.replace(v2.find("100.0"), 5, "50.0");
    std::filesystem::create_directories(std::filesystem::path(other_path).parent_path());
    write_file(other_path, v2);
    write_file(s.dir.path() / "unrelated.spp", "class Other {\n    float hp = 1.0;\n}\n");
    json p = doc_pos(s.uri, 3, 11);
    p["newName"] = "health";
    const json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    const json& changes = r["result"]["changes"];
    EXPECT_EQ(changes.size(), 2u) << r.dump();
    ASSERT_TRUE(changes.contains(s.uri)) << r.dump();
    ASSERT_TRUE(changes.contains(uri_of(other_path))) << r.dump();
    std::vector<std::pair<int, int>> at;
    for (const json& e : changes[uri_of(other_path)]) {
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
    const std::string other_path = (s.dir.path() / "player_v2.spp").string();
    write_file(other_path, kScript);
    s.client->open(uri_of(other_path), kScript);
    for (const std::string& uri : {uri_of(other_path), s.uri}) {
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
    const std::string other_path = (s.dir.path() / "player_v2.spp").string();
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
    const std::string other_path = (s.dir.path() / "player_v2.spp").string();
    write_file(other_path, kScript);
    const json r = s.client->request("textDocument/references",
                                     {{"textDocument", {{"uri", s.uri}}}, {"position", pos(11, 14)},
                                      {"context", {{"includeDeclaration", true}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].is_array()) << r.dump();
    int here = 0, there = 0;
    for (const json& l : r["result"]) {
        if (l["uri"] == s.uri) ++here;
        if (l["uri"] == uri_of(other_path)) ++there;
    }
    EXPECT_EQ(here, 3) << r.dump();
    EXPECT_EQ(there, 3) << r.dump();
}

// clangd runs its background index on the compile_commands.json of "sppc --compile-commands".
// A workspace symbol from a generated file that is not open maps back to the script.
TEST(LanguageServer, BackgroundIndexFindsAScriptSymbolInAGeneratedFile)
{
    REQUIRE_CLANGD();
    TempDir dir;
    const std::string script = (dir.path() / "player.spp").string();
    write_file(script, kScript);
    sppc::TranslateOptions o;
    o.script_text = kScript;
    o.script_name = script;
    o.generation = 1;
    o.include_dirs = {SPP_SOURCE_DIR "/include", SPP_SOURCE_DIR "/example/engine"};
    const sppc::TranslateResult t = sppc::translate(o);
    ASSERT_TRUE(t.ok) << t.diagnostics;
    const std::string cpp = (dir.path() / "player.g1.cpp").string();
    write_file(cpp, t.cpp);
    spp::ClangFilesOptions co;
    co.include_dirs = o.include_dirs;
    const std::string so = (dir.path() / "player.g1.so").string();
    write_file(dir.path() / "compile_commands.json",
               spp::compile_commands_json({{dir.path().string(), cpp, spp::compile_command(co, cpp, so), so}}));

    spp::ls::ServerOptions opt = options(dir, true);
    opt.include_dirs.clear();  // the server reads them from compile_commands.json
    opt.compile_commands_dir = dir.path().string();
    LsClient client(opt);
    client.initialize(uri_of(dir.path().string()));
    json found;
    for (int i = 0; i < 120 && found.is_null(); ++i) {
        const json r = client.request("workspace/symbol", {{"query", "heal"}});
        for (const json& sym : r.value("result", json::array()))
            if (sym["location"].value("uri", "") == uri_of(script)) found = sym;
        if (found.is_null()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_FALSE(found.is_null()) << "the index has no script symbol";
    EXPECT_EQ(found["name"], "Player.heal");
    EXPECT_EQ(found["location"]["range"]["start"]["line"], 10);
}

// S31 (DECISIONS 18): a C++ clangd (the editor extension) and spp-ls run on one project. The
// project has one compile_commands.json and one ".cache" directory. The clangd child of spp-ls
// keeps its index in ".cache/spp/clangd", so it does not mix with the index of the C++ clangd.
TEST(LanguageServer, CppClangdAndTheServerRunOnOneProjectWithoutDisturbingEachOther)
{
    REQUIRE_CLANGD();
    namespace fs = std::filesystem;
    TempDir dir;
    const fs::path root = dir.path();
    const std::string script = (root / "player.spp").string();
    write_file(script, kScript);
    sppc::TranslateOptions o;
    o.script_text = kScript;
    o.script_name = script;
    o.generation = 1;
    o.include_dirs = {SPP_SOURCE_DIR "/include", SPP_SOURCE_DIR "/example/engine"};
    const sppc::TranslateResult t = sppc::translate(o);
    ASSERT_TRUE(t.ok) << t.diagnostics;
    const std::string gen = (root / "player.g1.cpp").string();
    write_file(gen, t.cpp);
    const std::string cpp_text = "int cpp_function() { return 1; }\nint cpp_bad() { int x = \"abc\"; return x; }\n";
    const std::string cpp = (root / "main.cpp").string();
    write_file(cpp, cpp_text);
    spp::ClangFilesOptions co;
    co.include_dirs = o.include_dirs;
    write_file(root / "compile_commands.json",
               spp::compile_commands_json({{root.string(), gen, spp::compile_command(co, gen, gen + ".so"), gen + ".so"},
                                           {root.string(), cpp, spp::compile_command(co, cpp, cpp + ".so"), cpp + ".so"}}));

    // The C++ clangd, as the clangd extension starts it.
    std::mutex mu;
    std::vector<json> published;
    spp::ls::ClangdProcess cpp_clangd;
    ASSERT_TRUE(cpp_clangd.start(SPP_CLANGD_PATH, {"--log=error", "--background-index", "--compile-commands-dir=" + root.string()}, "",
                                 [&](const json& m) {
                                     if (m.value("method", std::string()) != "textDocument/publishDiagnostics") return;
                                     std::lock_guard<std::mutex> lock(mu);
                                     published.push_back(m["params"]);
                                 }));
    const json init = cpp_clangd.request("initialize", {{"processId", nullptr}, {"rootUri", uri_of(root.string())}, {"capabilities", json::object()}}, 30000);
    ASSERT_TRUE(init.contains("result")) << init.dump();
    cpp_clangd.notify("initialized", json::object());
    cpp_clangd.notify("textDocument/didOpen", {{"textDocument", {{"uri", uri_of(cpp)}, {"languageId", "cpp"}, {"version", 1}, {"text", cpp_text}}}});

    // spp-ls on the same project.
    spp::ls::ServerOptions opt = options(dir, true);
    opt.include_dirs.clear();
    opt.compile_commands_dir = root.string();
    LsClient client(opt);
    client.initialize(uri_of(root.string()));
    std::string bad = kScript;
    bad.replace(bad.find("this.hp = this.hp + amount;"), 27, "int x = \"abc\";");
    write_file(script, bad);
    client.open(uri_of(script), bad);

    // Both servers give their own diagnostics.
    const json spp_diag = client.wait_notification("textDocument/publishDiagnostics", [&](const json& p) {
        return p.value("uri", std::string()) == uri_of(script) && has_source(p["diagnostics"], "clangd");
    });
    EXPECT_FALSE(spp_diag.is_null()) << "spp-ls has no clangd diagnostic";
    bool cpp_diag = false;
    for (int i = 0; i < 100 && !cpp_diag; ++i) {
        {
            std::lock_guard<std::mutex> lock(mu);
            for (const json& p : published)
                if (p.value("uri", std::string()) == uri_of(cpp) && !p["diagnostics"].empty()) cpp_diag = true;
        }
        if (!cpp_diag) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(cpp_diag) << "the C++ clangd has no diagnostic";
    {
        // The C++ clangd never gets a diagnostic for the script, and spp-ls never for main.cpp.
        std::lock_guard<std::mutex> lock(mu);
        for (const json& p : published) EXPECT_EQ(p.value("uri", std::string()).find(".spp"), std::string::npos) << p.dump();
    }

    // Both background indexes run. The shards of the two clangd processes are in two directories.
    const fs::path cpp_index = root / ".cache" / "clangd" / "index";
    const fs::path spp_index = root / ".cache" / "spp" / "clangd" / ".cache" / "clangd" / "index";
    auto has_shard = [](const fs::path& dir_, const std::string& stem) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir_, ec))
            if (e.path().filename().string().rfind(stem, 0) == 0) return true;
        return false;
    };
    bool ready = false;
    for (int i = 0; i < 120 && !ready; ++i) {
        ready = has_shard(cpp_index, "main.cpp.") && has_shard(spp_index, "main.cpp.");
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT_TRUE(ready) << "a background index is missing";
    EXPECT_TRUE(fs::exists(root / ".cache" / "spp" / "clangd" / "compile_commands.json"));

    // The C++ clangd still answers for C++, and spp-ls still answers for the script.
    const json sym = cpp_clangd.request("workspace/symbol", {{"query", "cpp_function"}}, 30000);
    ASSERT_TRUE(sym.contains("result")) << sym.dump();
    EXPECT_FALSE(sym["result"].empty()) << sym.dump();
    const json hover = client.request("textDocument/hover", doc_pos(uri_of(script), 3, 11));
    EXPECT_TRUE(hover.contains("result")) << hover.dump();
    cpp_clangd.stop();
}

// ------------------------------------------------------------------ S28: saved symbol index

namespace {

spp::ls::IndexShard sample_shard()
{
    spp::ls::IndexShard shard;
    spp::ls::IndexClass c;
    c.name = "Player";
    c.begin = {2, 6};
    c.refs = {{2, 6}, {9, 4}};
    spp::ls::IndexMember known{"hp", "field", {3, 10}, std::vector<spp::ls::IndexPos>{{3, 10}, {11, 13}}};
    spp::ls::IndexMember unknown{"heal", "method", {10, 9}, std::nullopt};
    c.members = {known, unknown};
    shard.classes.push_back(c);
    return shard;
}

size_t file_count(const std::filesystem::path& dir)
{
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

int analyzed_files(LsClient& client)
{
    const json r = client.request("spp/indexStats", json::object());
    return r["result"].value("analyzed", -1);
}

}  // namespace

TEST(SymbolIndex, DigestCoversTheImportedScripts)
{
    TempDir dir;
    const std::string main = (dir.path() / "main.spp").string();
    const std::string deep = (dir.path() / "deep.spp").string();
    const std::string inc = (dir.path() / "inc").string();
    std::filesystem::create_directories(inc);
    write_file(main, "using \"deep.spp\";\nusing \"lib.spp\";\n");
    write_file(deep, "class Deep {}\n");
    write_file(inc + "/lib.spp", "using \"far.spp\";\n");
    write_file(inc + "/far.spp", "class Far {}\n");
    const std::string text = "using \"deep.spp\";\nusing \"lib.spp\";\n";
    const auto digest = [&] { return spp::ls::SymbolIndex::digest(main, text, {inc}); };
    const std::string before = digest();
    EXPECT_EQ(before, digest());
    write_file(deep, "class Deep { int x = 1; }\n");  // an import
    const std::string after_deep = digest();
    EXPECT_NE(before, after_deep);
    write_file(inc + "/far.spp", "class Far { int y = 1; }\n");  // an import of an import
    EXPECT_NE(after_deep, digest());
    // A script with no import has the digest of its text, plus nothing else.
    EXPECT_EQ(spp::ls::SymbolIndex::digest(main, "class A {}\n", {}), spp::ls::SymbolIndex::digest("class A {}\n"));
}

TEST(SymbolIndex, ReusesAShardWhenTheDigestMatches)
{
    TempDir dir;
    spp::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.spp").string();
    write_file(script, "class A {}\n");
    const std::string digest = spp::ls::SymbolIndex::digest("class A {}\n");
    ASSERT_TRUE(index.store(script, digest, sample_shard()));
    auto shard = index.load(script, digest);
    ASSERT_TRUE(shard);
    spp::ls::IndexClass* c = shard->find_class("Player");
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
    spp::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.spp").string();
    write_file(script, "x");
    const std::string d1 = spp::ls::SymbolIndex::digest("one"), d2 = spp::ls::SymbolIndex::digest("two");
    ASSERT_NE(d1, d2);
    ASSERT_TRUE(index.store(script, d1, sample_shard()));
    EXPECT_FALSE(index.load(script, d2));
    ASSERT_TRUE(index.store(script, d2, spp::ls::IndexShard{}));
    EXPECT_FALSE(index.load(script, d1));
    ASSERT_TRUE(index.load(script, d2));
    EXPECT_EQ(file_count(dir.path() / "index"), 1u);
}

TEST(SymbolIndex, PruneDeletesTheShardOfAMissingFile)
{
    TempDir dir;
    spp::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string a = (dir.path() / "a.spp").string(), b = (dir.path() / "b.spp").string();
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
    spp::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.spp").string();
    write_file(script, "x");
    ASSERT_TRUE(index.store(script, "d", sample_shard()));
    const std::string file = index.shard_path(script);
    std::string text = *[&] {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return std::optional<std::string>(ss.str());
    }();
    const std::string head = "spp-index " + std::to_string(spp::ls::SymbolIndex::kFormatVersion) + " ";
    ASSERT_EQ(text.rfind(head, 0), 0u);
    text.replace(0, head.size(), "spp-index 999 ");
    write_file(file, text);
    EXPECT_FALSE(index.load(script, "d"));
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST(SymbolIndex, TheIndexDoesNotGrowAfter1000EditsOfOneFile)
{
    TempDir dir;
    spp::ls::SymbolIndex index;
    index.open((dir.path() / "index").string());
    const std::string script = (dir.path() / "a.spp").string();
    write_file(script, "x");
    std::uintmax_t first_size = 0;
    for (int i = 0; i < 1000; ++i) {
        const std::string digest = spp::ls::SymbolIndex::digest("version " + std::to_string(i));
        ASSERT_TRUE(index.store(script, digest, sample_shard()));
        if (i == 0) first_size = std::filesystem::file_size(index.shard_path(script));
    }
    EXPECT_EQ(file_count(dir.path() / "index"), 1u);  // no old shard and no temporary file
    EXPECT_LE(std::filesystem::file_size(index.shard_path(script)), first_size + 16);
}

// A class rename in a closed file. The first server analyzes the file and saves the shard.
// A new server after a restart loads the shard and analyzes no file.
TEST(LanguageServer, RenameOnAClosedFileAfterARestartUsesTheIndex)
{
    Session s(false);
    const std::string other_path = (s.dir.path() / "player_v2.spp").string();
    write_file(other_path, kScript);
    write_file(s.dir.path() / "unrelated.spp", "class Other {\n    float hp = 1.0;\n}\n");
    json p = doc_pos(s.uri, 2, 7);
    p["newName"] = "Hero";
    json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    ASSERT_TRUE(r["result"]["changes"].contains(uri_of(other_path))) << r.dump();
    const json first = r["result"]["changes"][uri_of(other_path)];
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_TRUE(std::filesystem::exists(s.dir.path() / ".cache" / "spp" / "index"));
    s.client.reset();  // the restart
    s.client = std::make_unique<LsClient>(options(s.dir, false));
    s.client->initialize(uri_of(s.dir.path().string()));
    s.client->open(s.uri, kScript);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)], first) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 0);
    // A changed file is analyzed again. The shard count stays the same.
    const size_t shards = file_count(s.dir.path() / ".cache" / "spp" / "index");
    std::string changed = kScript;
    changed += "\n";
    write_file(other_path, changed);
    r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_EQ(file_count(s.dir.path() / ".cache" / "spp" / "index"), shards);
}

// The same for a member. The references of a member come from clangd. The shard keeps them.
TEST(LanguageServer, MemberRenameOnAClosedFileAfterARestartUsesTheIndex)
{
    REQUIRE_CLANGD();
    Session s(true);
    spp::ls::ServerOptions o = options(s.dir, true);
    o.index_dir = (s.dir.path() / "my-index").string();
    s.client.reset();
    s.client = std::make_unique<LsClient>(o);
    s.client->initialize(uri_of(s.dir.path().string()));
    s.client->open(s.uri, kScript);
    const std::string other_path = (s.dir.path() / "player_v2.spp").string();
    write_file(other_path, kScript);
    json p = doc_pos(s.uri, 3, 11);
    p["newName"] = "health";
    json r = s.client->request("textDocument/rename", p);
    ASSERT_TRUE(r.contains("result")) << r.dump();
    ASSERT_TRUE(r["result"]["changes"].contains(uri_of(other_path))) << r.dump();
    EXPECT_EQ(r["result"]["changes"][uri_of(other_path)].size(), 3u) << r.dump();
    EXPECT_EQ(analyzed_files(*s.client), 1);
    EXPECT_FALSE(std::filesystem::exists(s.dir.path() / ".cache"));  // --index-dir is used
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

namespace {

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

struct Completion {
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
    for (size_t i = 0; i < at; ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    col += static_cast<int>(replace.size());
    Session s(clangd, text);
    Completion c;
    json r = s.client->request("textDocument/completion", doc_pos(s.uri, line, col));
    c.result = r.contains("result") ? r["result"] : json::object();
    for (const json& i : c.result.value("items", json::array())) c.names.push_back(i.value("insertText", i.value("label", "")));
    return c;
}

}  // namespace

TEST(LanguageServer, CompletionListsLocalsParametersAndModuleVariables)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript);
    EXPECT_TRUE(c.has("counter")) << c.result.dump();  // a local
    EXPECT_TRUE(c.has("current")) << c.result.dump();
    EXPECT_TRUE(c.has("speed")) << c.result.dump();    // a parameter
    EXPECT_TRUE(c.has("total")) << c.result.dump();    // a module variable
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
    EXPECT_TRUE(m.has("ticks")) << m.result.dump();        // own
    EXPECT_TRUE(m.has("run"));
    EXPECT_TRUE(m.has("baseField")) << m.result.dump();    // from the script base
    EXPECT_TRUE(m.has("baseMethod"));
    EXPECT_TRUE(m.has("Move")) << m.result.dump();         // from the native base
}

TEST(LanguageServer, CompletionListsNativeTypesAndFreeFunctions)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript, "Vec");
    EXPECT_TRUE(c.has("Vec3")) << c.result.dump();  // a native type of Math.h
    const Completion f = complete_at(true, kCompleteScript, "Hit");
    bool function = false;
    for (const json& i : f.result.value("items", json::array()))
        if (i.value("kind", 0) == 3) function = true;
    EXPECT_TRUE(function) << f.result.dump();  // a native free function
}

TEST(LanguageServer, CompletionListsKeywordsAndBuiltinTypes)
{
    const Completion c = complete_at(false, kCompleteScript);
    EXPECT_TRUE(c.has("while")) << c.result.dump();
    EXPECT_TRUE(c.has("return"));
    EXPECT_TRUE(c.has("float"));
    EXPECT_TRUE(c.has("int32"));  // an alias of a builtin type
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
    for (const json& i : c.result["items"]) {  // the filter text is the name without the namespace
        std::string n = i.value("filterText", i.value("label", ""));
        for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
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
    EXPECT_TRUE(c.has("counter")) << c.result.dump();  // a script fact
    const Completion k = complete_at(true, text, "whi");
    EXPECT_TRUE(k.has("while")) << k.result.dump();     // a keyword
}

TEST(LanguageServer, CompletionHidesTheGeneratedHelpers)
{
    REQUIRE_CLANGD();
    const Completion c = complete_at(true, kCompleteScript);
    ASSERT_FALSE(c.names.empty());
    for (const std::string& n : c.names) {
        for (const char* bad : {"__", "_Shell", "SLOT_", "thunk", "spp_", "_slot", "_layout", "_fields", "SppModule", "_g1"})
            EXPECT_EQ(n.find(bad), std::string::npos) << n << " has " << bad;
        EXPECT_NE(n[0], '_') << n;
    }
    for (const char* n : {"self", "st", "a"}) EXPECT_FALSE(c.has(n)) << n;
}

int completion_translations(LsClient& client)
{
    return client.request("spp/indexStats", json::object())["result"].value("completions", -1);
}

TEST(LanguageServer, CompletionReusesTheAnswerWhileThePrefixGrows)
{
    REQUIRE_CLANGD();
    std::string text = kCompleteScript;
    auto with_word = [&](const std::string& word) {
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
    EXPECT_EQ(completion_translations(*s.client), 1);  // the longer prefix reuses the answer
    bool counter = false;
    for (const json& i : b["result"]["items"]) counter = counter || i.value("insertText", "") == "counter";
    EXPECT_TRUE(counter) << b.dump();
    for (const json& i : b["result"]["items"]) EXPECT_EQ(i.value("filterText", "coun").compare(0, 4, "coun"), 0) << i.dump();
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
    const json again = s.client->request("textDocument/formatting", {{"textDocument", {{"uri", s.uri}}}, {"options", o}});
    ASSERT_TRUE(again.contains("result")) << again.dump();
    EXPECT_TRUE(again["result"].empty()) << again.dump();
}

TEST(LanguageServer, CodeActionInsertsAMissingSemicolon)
{
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.hp = this.hp + amount");
    Session s(false, text);
    const json n = s.diagnostics([](const json& d) { return has_source(d, "spp"); });
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
    EXPECT_EQ(edit["range"]["start"], pos(11, 34)) << r.dump();  // after "amount"
    EXPECT_EQ(edit["range"]["end"], pos(11, 34)) << r.dump();
}

TEST(LanguageServer, CodeActionAppliesTheNameThatClangdSuggests)
{
    REQUIRE_CLANGD();
    std::string text = kScript;
    text.replace(text.find("this.hp = this.hp + amount;"), 27, "this.hp = amoun;");
    Session s(true, text);
    const json n = s.diagnostics([](const json& d) {
        return std::any_of(d.begin(), d.end(), [](const json& x) { return x.value("message", "").find("did you mean") != std::string::npos; });
    });
    ASSERT_FALSE(n.is_null());
    json diag;
    for (const json& x : n["params"]["diagnostics"])
        if (x.value("message", "").find("did you mean") != std::string::npos) diag = x;
    const json r = s.client->request("textDocument/codeAction",
                                     {{"textDocument", {{"uri", s.uri}}}, {"range", diag["range"]},
                                      {"context", {{"diagnostics", json::array({diag})}}}});
    ASSERT_TRUE(r.contains("result") && r["result"].size() == 1) << r.dump();
    const json& edit = r["result"][0]["edit"]["changes"][s.uri][0];
    EXPECT_EQ(edit["newText"], "amount") << r.dump();
    EXPECT_EQ(edit["range"], diag["range"]);
}

// ------------------------------------------------------------------ hosts (step 2b)
namespace {

// A workspace with a header in "inc" and a script in "scripts". The script uses the header.
// With "host" the hosts dir holds a host file for the scripts dir, with "inc" as include dir.
// The server gets no -I option for "inc". The completion after "Thing." comes from the host only.
// With "watched" the client sends workspace/didChangeWatchedFiles for the new host file. The
// script gets no further change.
// With "defines_file" the header needs the define HOST_NAME. The host file names "h.defines" in
// "definesFile". The file holds the text of the argument, or no file when the text is "-".
std::vector<std::string> host_completion_labels(bool host, bool change_host_later = false, bool watched = false,
                                                const std::string& defines_file = "")
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
    if (!defines_file.empty() && defines_file != "-") write_file((root / "hosts" / "h.defines").string(), defines_file);
    const std::string text = "using \"thing.h\";\n\nclass A {\n    void f() {\n        Thing.\n    }\n}\n";
    const std::string path = (root / "scripts" / "a.spp").string();
    write_file(path, text);
    write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
    auto write_host = [&] {
        write_file((root / "hosts" / "h.json").string(),
                   R"({"version":1,"name":"h","binaryDir":")" + (root / "build").string() +
                       R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")" +
                       (root / "scripts").string() + R"("],"scripts":[],"includeDirs":[")" +
                       (root / "inc").string() + R"(",")" + SPP_SOURCE_DIR + R"(/include"],"defines":[],)" +
                       (defines_file.empty() ? "" : R"("definesFile":"h.defines",)") + R"("flags":[]})");
    };
    if (host && !change_host_later) write_host();
    spp::ls::ServerOptions o;
    o.clangd = SPP_CLANGD_PATH;
    o.hosts_dir = (root / "hosts").string();
    o.clangd_log = (root / "clangd.log").string();
    // The runtime headers are needed to translate. Without a host, they come from -I.
    if (!host) o.include_dirs = {SPP_SOURCE_DIR "/include"};
    LsClient client(o);
    client.initialize(uri_of(root.string()));
    const std::string uri = uri_of(path);
    client.open(uri, text);
    if (host && change_host_later) {
        // The host file appears after the open. The next change must pick it up.
        write_host();
        if (watched)
            client.notify("workspace/didChangeWatchedFiles",
                          {{"changes", json::array({{{"uri", uri_of((root / "hosts" / "h.json").string())}, {"type", 1}}})}});
        else
            client.change(uri, 2, text + "\n");
    }
    const json r = client.request("textDocument/completion", doc_pos(uri, 4, 14));
    std::vector<std::string> labels;
    if (r.contains("result") && r["result"].is_object())
        for (const json& i : r["result"]["items"]) labels.push_back(i.value("insertText", i.value("label", "")));
    return labels;
}

bool has_label(const std::vector<std::string>& labels, const char* n)
{
    return std::find(labels.begin(), labels.end(), n) != labels.end();
}

}  // namespace

TEST(LanguageServerHosts, ScriptUsesTheIncludeDirsOfItsHost)
{
    REQUIRE_CLANGD();
    EXPECT_TRUE(has_label(host_completion_labels(true), "Value"));
}

TEST(LanguageServerHosts, DefineWithQuotesFromADefinesFileApplies)
{
    REQUIRE_CLANGD();
    // The lines hold quotes and a backslash. The server passes them to clangd as they are.
    EXPECT_TRUE(has_label(host_completion_labels(true, false, false, "HOST_NAME=\"a b\"\nOTHER=\"C:\\dir\"\n"), "Value"));
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
    if (ec) GTEST_SKIP() << "the file system does not allow the directory name";
    std::filesystem::create_directories(root / "scripts");
    std::filesystem::create_directories(root / "build");
    std::filesystem::create_directories(root / "hosts");
    write_file((weird / "thing.h").string(), "#pragma once\nnamespace Thing { int Value(); }\n");
    write_file((root / "hosts" / "h.includes").string(), weird.string() + "\n");
    write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
    write_file((root / "hosts" / "h.json").string(),
               R"({"version":1,"name":"h","binaryDir":")" + (root / "build").string() +
                   R"(","sourceDir":")" + root.string() +
                   R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":["scripts"],)" +
                   R"("scripts":[],"includeDirs":[")" + SPP_SOURCE_DIR + R"(/include"],"defines":[],)" +
                   R"("includesFile":"h.includes","flags":[]})");
    const std::string text = "using \"thing.h\";\n\nclass A {\n    void f() {\n        Thing.\n    }\n}\n";
    const std::string path = (root / "scripts" / "a.spp").string();
    write_file(path, text);
    spp::ls::ServerOptions o;
    o.clangd = SPP_CLANGD_PATH;
    o.hosts_dir = (root / "hosts").string();
    o.clangd_log = (root / "clangd.log").string();
    LsClient client(o);
    client.initialize(uri_of(root.string()));
    const std::string uri = uri_of(path);
    client.open(uri, text);
    const json r = client.request("textDocument/completion", doc_pos(uri, 4, 14));
    std::vector<std::string> labels;
    if (r.contains("result") && r["result"].is_object())
        for (const json& i : r["result"]["items"]) labels.push_back(i.value("insertText", i.value("label", "")));
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
    const std::string path = (root / "a.spp").string();
    write_file(path, text);
    spp::ls::ServerOptions o;
    o.hosts_dir = (root / "hosts").string();
    o.include_dirs = {SPP_SOURCE_DIR "/include"};
    LsClient client(o);
    client.initialize(uri_of(root.string()));
    client.open(uri_of(path), text);
    auto count_other = [&](const std::string& other_uri) {
        const json r = client.request("textDocument/references",
                                      {{"textDocument", {{"uri", uri_of(path)}}}, {"position", {{"line", 1}, {"character", 11}}},
                                       {"context", {{"includeDeclaration", true}}}});
        int n = 0;
        if (r.contains("result") && r["result"].is_array())
            for (const json& l : r["result"])
                if (l["uri"] == other_uri) ++n;
        return n;
    };
    const std::string other = (root / "b.spp").string();
    EXPECT_EQ(count_other(uri_of(other)), 0);
    write_file(other, text);
    client.notify("workspace/didChangeWatchedFiles", {{"changes", json::array({{{"uri", uri_of(other)}, {"type", 1}}})}});
    EXPECT_EQ(count_other(uri_of(other)), 1);
    std::filesystem::remove(other);
    client.notify("workspace/didChangeWatchedFiles", {{"changes", json::array({{{"uri", uri_of(other)}, {"type", 3}}})}});
    EXPECT_EQ(count_other(uri_of(other)), 0);
}

// The information diagnostic "No script host". Rule: a .spp script has no host and the server
// has no -I option.
namespace {

struct HostDiagCase {
    TempDir dir;
    std::string path, uri, text = "class A {\n}\n";
    spp::ls::ServerOptions o;
    HostDiagCase()
    {
        const std::filesystem::path root = dir.path();
        std::filesystem::create_directories(root / "scripts");
        std::filesystem::create_directories(root / "build");
        std::filesystem::create_directories(root / "hosts");
        path = (root / "scripts" / "a.spp").string();
        uri = uri_of(path);
        write_file(path, text);
        write_file((root / "build" / "CMakeCache.txt").string(), "# cache\n");
        o.hosts_dir = (root / "hosts").string();
    }
    void write_host() const
    {
        const std::filesystem::path root = dir.path();
        write_file((root / "hosts" / "h.json").string(),
                   R"({"version":1,"name":"h","binaryDir":")" + (root / "build").string() +
                       R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")" +
                       (root / "scripts").string() + R"("],"scripts":[],"includeDirs":[")" + SPP_SOURCE_DIR +
                       R"(/include"],"defines":[],"flags":[]})");
    }
    json next_diagnostics(LsClient& c) const
    {
        return c.wait_notification("textDocument/publishDiagnostics",
                                   [&](const json& p) { return p.value("uri", std::string()) == uri; });
    }
};

}  // namespace

TEST(LanguageServerHosts, NoHostGivesAnInformationDiagnostic)
{
    HostDiagCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, c.text);
    const json n = c.next_diagnostics(client);
    ASSERT_TRUE(has_source(n["params"]["diagnostics"], "spp-ls")) << n.dump();
    for (const json& d : n["params"]["diagnostics"])
        if (d.value("source", "") == "spp-ls") {
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
    EXPECT_FALSE(has_source(n["params"]["diagnostics"], "spp-ls")) << n.dump();
}

TEST(LanguageServerHosts, InformationDiagnosticGoesWhenAHostAppears)
{
    HostDiagCase c;
    LsClient client(c.o);
    client.initialize(uri_of(c.dir.path().string()));
    client.open(c.uri, c.text);
    EXPECT_TRUE(has_source(c.next_diagnostics(client)["params"]["diagnostics"], "spp-ls"));
    c.write_host();
    client.change(c.uri, 2, c.text + "\n");
    EXPECT_FALSE(has_source(c.next_diagnostics(client)["params"]["diagnostics"], "spp-ls"));
}
