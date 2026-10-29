// Tests for spp-ls (S14b, design M12 to M14). One test for each feature. The tests that need
// clangd are skipped when CMake did not find clangd.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "integration/ls/ls_client.h"
#include "spp/clang_files_backend.h"
#include "spp/sppc_lib.h"
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
    const size_t at = t.cpp.find("st->hp = st->hp + amount;");
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
