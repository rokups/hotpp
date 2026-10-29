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
