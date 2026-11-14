// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The matrix of the name requests of hot-ls. A row is a kind of symbol. A column is a request
// (definition, references, prepareRename, rename). An axis is a file of the workspace:
//   - Same: lib.hot, the script that declares the symbols (open).
//   - Closed: closed.hot imports lib.hot (not open).
//   - Open: open.hot imports lib.hot (open).
//   - Deep: top.hot imports closed.hot, thus lib.hot through it (open). The import is not
//     transitive: a free function of lib.hot has no ref in top.hot.
//   - Unrelated: other.hot has no import. It declares symbols with the same names (open).
// Each case asserts the exact set of (uri, line, character). The expectation is the correct
// behaviour. A case that fails on the current server is in kSkip with the missing feature.
//
// definition and prepareRename: the request is at the last use of the word in the axis file.
// references and rename: the request is at the last use of the word in lib.hot. The case
// checks the places in the axis file only. A request in a closed file is not possible, so the
// columns definition and prepareRename have no Closed case.
#include "integration/ls/ls_client.h"
#include "long_path.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using ls_test::json;
using ls_test::LsClient;
using translator_test::TempDir;

namespace
{

const char* const kLib = R"(using "Actor.h";
const int Limit = 5;
int Twice(int v) { return v * 2; }
enum Mode { Slow, Fast }
class Gun : Actor {
    public int ammo = 0;
    public static const int Max = 4;
    public static int Make(int k) { Mode m = Mode.Fast; return k + Gun.Max; }
    public virtual int Fire(int bullets) { int shots = bullets + ammo; return Twice(shots) + Max + Limit + Make(shots); }
}
)";

const char* const kClosed = R"(using "Actor.h";
using "lib.hot";
class Rifle : Gun {
    public override int Fire(int bullets) { int shots = bullets + this.ammo + Gun.Max + Limit + Twice(1) + Gun.Make(2); Mode m = Mode.Fast; return shots; }
    public int Shoot() { return this.Fire(1); }
}
)";

const char* const kOpen = R"(using "Actor.h";
using "lib.hot";
class Pistol : Gun {
    public override int Fire(int bullets) { int shots = bullets + this.ammo + Gun.Max + Limit + Twice(1) + Gun.Make(2); Mode m = Mode.Fast; return shots; }
    public int Shoot() { return this.Fire(1); }
}
)";

const char* const kTop = R"(using "Actor.h";
using "closed.hot";
class Cannon : Rifle {
    public override int Fire(int bullets) { int shots = bullets + this.ammo + Gun.Max + Limit + Twice(1) + Gun.Make(2); Mode m = Mode.Fast; return shots; }
    public int Blast() { return this.Fire(1); }
}
)";

const char* const kOther = R"(using "Actor.h";
const int Limit = 7;
int Twice(int v) { return v; }
enum Mode { Slow, Fast }
class Gun : Actor {
    public int ammo = 1;
    public static const int Max = 9;
    public static int Make(int k) { Mode m = Mode.Fast; return k + Gun.Max; }
    public virtual int Fire(int bullets) { int shots = bullets + ammo + Max + Limit + Twice(1) + Make(1); return shots; }
}
)";

enum class Axis
{
    Same,
    Closed,
    Open,
    Deep,
    Unrelated
};
const char* const kAxisNames[] = {"Same", "Closed", "Open", "Deep", "Unrelated"};
const char* const kFiles[] = {"lib.hot", "closed.hot", "open.hot", "top.hot", "other.hot"};
const char* const kTexts[] = {kLib, kClosed, kOpen, kTop, kOther};

enum class Op
{
    Definition,
    References,
    PrepareRename,
    Rename
};
const char* const kOpNames[] = {"Definition", "References", "PrepareRename", "Rename"};

struct Row
{
    const char* kind;
    const char* word;
    bool file_local;
    // The definition is in the axis file: the importers override the method "Fire". There a use
    // of "Fire" goes to the override of the class.
    bool def_in_axis = false;
};

const Row kRows[] = {
    {"Local", "shots", true},
    {"Param", "bullets", true},
    {"Field", "ammo", false},
    {"Method", "Fire", false, true},
    {"StaticMethod", "Make", false},
    {"StaticConst", "Max", false},
    {"ModuleConst", "Limit", false},
    {"FreeFunction", "Twice", false},
    {"Class", "Gun", false},
    {"EnumValue", "Fast", false},
};

// The cases that fail on the current server. Each group names the missing feature. All the cases
// pass now: the list is empty.
const std::vector<std::string> kSkip = {};

using Place = std::tuple<std::string, int, int>; // uri, line, character

bool is_word(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The places of the whole word "w" in "text" as (line, character).
std::vector<std::pair<int, int>> words(const std::string& text, const std::string& w)
{
    std::vector<std::pair<int, int>> out;
    for (size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1))
    {
        if ((at > 0 && is_word(text[at - 1])) || (at + w.size() < text.size() && is_word(text[at + w.size()])))
            continue;
        const size_t ls = at == 0 ? 0 : text.rfind('\n', at - 1);
        const int line = static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
        out.emplace_back(line, static_cast<int>(ls == std::string::npos || at == 0 ? at : at - ls - 1));
    }
    return out;
}

std::string uri_of(const std::string& path)
{
    return "file://" + hot::ls::long_path(path);
}

// One server for all the cases. The cases do not change the files (a rename gives only edits).
struct Workspace
{
    TempDir dir;
    std::unique_ptr<LsClient> client;
    std::string uris[5];

    Workspace()
    {
        for (int i = 0; i < 5; ++i)
        {
            const std::string path = (dir.path() / kFiles[i]).string();
            std::ofstream(path, std::ios::binary) << kTexts[i];
            uris[i] = uri_of(path);
        }
        hot::ls::ServerOptions o;
        o.clangd = HOT_CLANGD_PATH;
        o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
        o.clangd_log = (dir.path() / "clangd.log").string();
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(dir.path().string()));
        for (int i : {0, 2, 3, 4})
            client->open(uris[i], kTexts[i]);
    }
};

Workspace* g_ws = nullptr;
int g_skipped = 0;

struct Case
{
    size_t row;
    Op op;
    Axis axis;
};

std::string case_name(const Case& c)
{
    return std::string(kRows[c.row].kind) + "_" + kOpNames[static_cast<int>(c.op)] + "_"
        + kAxisNames[static_cast<int>(c.axis)];
}

std::vector<Case> all_cases()
{
    std::vector<Case> out;
    for (size_t r = 0; r < std::size(kRows); ++r)
        for (int o = 0; o < 4; ++o)
            for (int a = 0; a < 5; ++a)
            {
                const Op op = static_cast<Op>(o);
                if (a == static_cast<int>(Axis::Closed) && (op == Op::Definition || op == Op::PrepareRename))
                    continue;
                out.push_back({r, op, static_cast<Axis>(a)});
            }
    return out;
}

json doc_pos(const std::string& uri, std::pair<int, int> p)
{
    return {{"textDocument", {{"uri", uri}}}, {"position", {{"line", p.first}, {"character", p.second}}}};
}

std::set<Place> places_of(const json& locations)
{
    std::set<Place> out;
    if (locations.is_object())
        return places_of(json::array({locations}));
    if (!locations.is_array())
        return out;
    for (const json& l : locations)
    {
        const json& r = l.contains("targetSelectionRange") ? l["targetSelectionRange"] : l["range"];
        out.emplace(l.value("uri", l.value("targetUri", std::string())), r["start"]["line"].get<int>(),
            r["start"]["character"].get<int>());
    }
    return out;
}

// The places of the word in the axis file. They are all the symbol, or none of them is.
std::set<Place> word_places(int file, const std::string& w)
{
    std::set<Place> out;
    for (const auto& [l, c] : words(kTexts[file], w))
        out.emplace(g_ws->uris[file], l, c);
    return out;
}

std::set<Place> expected(const Case& c)
{
    const Row& row = kRows[c.row];
    const int f = static_cast<int>(c.axis);
    const bool indirect_fn = c.axis == Axis::Deep && std::string(row.kind) == "FreeFunction";
    const bool same_symbol = c.axis != Axis::Unrelated && !indirect_fn && (!row.file_local || c.axis == Axis::Same);
    // The import is not transitive: a free function of lib.hot is not visible in top.hot.
    if (indirect_fn && (c.op == Op::Definition || c.op == Op::PrepareRename))
        return {};
    switch (c.op)
    {
    case Op::Definition:
    {
        // The first word in a file is the declaration.
        const int decl_file = same_symbol && !row.def_in_axis ? 0 : f;
        const auto first = words(kTexts[decl_file], row.word).front();
        return {Place{g_ws->uris[decl_file], first.first, first.second}};
    }
    case Op::PrepareRename:
    {
        const auto last = words(kTexts[f], row.word).back();
        return {Place{g_ws->uris[f], last.first, last.second}};
    }
    case Op::References:
    case Op::Rename:
        return same_symbol ? word_places(f, row.word) : std::set<Place>{};
    }
    return {};
}

std::set<Place> actual(const Case& c, std::string& dump)
{
    const Row& row = kRows[c.row];
    const int f = static_cast<int>(c.axis);
    LsClient& client = *g_ws->client;
    if (c.op == Op::Definition || c.op == Op::PrepareRename)
    {
        const json p = doc_pos(g_ws->uris[f], words(kTexts[f], row.word).back());
        const json r = client.request(
            c.op == Op::Definition ? "textDocument/definition" : "textDocument/prepareRename", p);
        dump = r.dump();
        if (!r.contains("result") || r["result"].is_null())
            return {};
        const json& res = r["result"];
        if (c.op == Op::Definition)
            return places_of(res);
        const json& range = res.contains("range") ? res["range"] : res;
        return {Place{g_ws->uris[f], range["start"]["line"].get<int>(), range["start"]["character"].get<int>()}};
    }
    json p = doc_pos(g_ws->uris[0], words(kLib, row.word).back());
    std::set<Place> all;
    if (c.op == Op::References)
    {
        p["context"] = {{"includeDeclaration", true}};
        const json r = client.request("textDocument/references", p);
        dump = r.dump();
        all = places_of(r.value("result", json()));
    }
    else
    {
        p["newName"] = "Renamed";
        const json r = client.request("textDocument/rename", p);
        dump = r.dump();
        const json changes = r.contains("result") && r["result"].is_object()
            ? r["result"].value("changes", json::object())
            : json::object();
        for (const auto& [uri, edits] : changes.items())
            for (const json& e : edits)
                all.emplace(uri, e["range"]["start"]["line"].get<int>(), e["range"]["start"]["character"].get<int>());
    }
    std::set<Place> in_file;
    for (const Place& pl : all)
        if (std::get<0>(pl) == g_ws->uris[f])
            in_file.insert(pl);
    return in_file;
}

std::string show(const std::set<Place>& s)
{
    std::string out;
    for (const auto& [u, l, c] : s)
        out += std::filesystem::path(u.substr(7)).filename().string() + ":" + std::to_string(l) + ":"
            + std::to_string(c) + " ";
    return out;
}

class RefMatrix : public ::testing::TestWithParam<Case>
{
public:
    static void SetUpTestSuite()
    {
        if (std::string(HOT_CLANGD_PATH).empty())
            return;
        g_ws = new Workspace();
    }
    static void TearDownTestSuite()
    {
        std::printf("[ RefMatrix ] %d cases in the skip list\n", g_skipped);
        delete g_ws;
        g_ws = nullptr;
    }
};

TEST_P(RefMatrix, Case)
{
    if (!g_ws)
        GTEST_SKIP() << "clangd is not installed";
    const Case c = GetParam();
    const std::string name = case_name(c);
    const bool skip = std::find(std::begin(kSkip), std::end(kSkip), name) != std::end(kSkip);
    // HOT_REF_MATRIX_ALL=1 runs the skipped cases too: a pass shows a case to take from kSkip.
    if (skip && !std::getenv("HOT_REF_MATRIX_ALL"))
    {
        ++g_skipped;
        GTEST_SKIP() << "in kSkip";
    }
    std::string dump;
    const std::set<Place> got = actual(c, dump);
    const std::set<Place> want = expected(c);
    EXPECT_EQ(show(got), show(want)) << dump.substr(0, 600);
}

// A second references request uses the warm tables: the server does no new analyze.
TEST_F(RefMatrix, WarmTableNoAnalyze)
{
    if (!g_ws)
        GTEST_SKIP() << "clangd is not installed";
    LsClient& client = *g_ws->client;
    json p = doc_pos(g_ws->uris[0], words(kLib, "Gun").back());
    p["context"] = {{"includeDeclaration", true}};
    auto stats = [&] { return client.request("hot/indexStats", json::object())["result"]; };
    auto ask = [&]
    {
        const auto t0 = std::chrono::steady_clock::now();
        const json r = client.request("textDocument/references", p);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        std::printf("[ RefMatrix ] references of Gun: %lld ms\n", static_cast<long long>(ms.count()));
        return places_of(r.value("result", json()));
    };
    const std::set<Place> first = ask();
    const int analyses = stats().value("tableAnalyses", -1);
    const std::set<Place> warm = ask();
    EXPECT_EQ(stats().value("tableAnalyses", -1), analyses);
    EXPECT_EQ(show(warm), show(first));
    EXPECT_FALSE(first.empty());
}

// A rename to a name that a declaration in a hiding or hidden scope has: an error and no edit.
// A free new name gives the edit (the cases of the matrix use "Renamed").
TEST_F(RefMatrix, RenameConflicts)
{
    if (!g_ws)
        GTEST_SKIP() << "clangd is not installed";
    LsClient& client = *g_ws->client;
    struct Conflict
    {
        const char* kind;
        const char* word;
        const char* new_name;
        const char* file_line; // the file and the line that the message names
    };
    const Conflict cases[] = {
        {"member of the class", "ammo", "Fire", "lib.hot:9"},
        {"member of a derived class", "ammo", "Shoot", ".hot:5"},
        {"module symbol", "Limit", "Twice", "lib.hot:3"},
        {"local and a parameter", "shots", "bullets", "lib.hot:9"},
        {"local that hides a member", "shots", "ammo", "lib.hot:9"},
    };
    for (const Conflict& c : cases)
    {
        SCOPED_TRACE(c.kind);
        json p = doc_pos(g_ws->uris[0], words(kLib, c.word).back());
        p["newName"] = c.new_name;
        const json r = client.request("textDocument/rename", p);
        ASSERT_TRUE(r.contains("error")) << r.dump();
        EXPECT_FALSE(r.contains("result") && !r["result"].is_null()) << r.dump();
        EXPECT_NE(r["error"].value("message", std::string()).find(c.file_line), std::string::npos) << r.dump();
    }
}

INSTANTIATE_TEST_SUITE_P(Matrix, RefMatrix, ::testing::ValuesIn(all_cases()),
    [](const ::testing::TestParamInfo<Case>& i) { return case_name(i.param); });

// The scripts have no translator error: a failure of a case is not a broken script.
TEST(RefMatrixScripts, HaveNoErrors)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    TempDir dir;
    for (int i = 0; i < 5; ++i)
        std::ofstream((dir.path() / kFiles[i]).string(), std::ios::binary) << kTexts[i];
    hot::ls::ServerOptions o;
    o.clangd = "";
    o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    for (int i = 0; i < 5; ++i)
    {
        const std::string uri = uri_of((dir.path() / kFiles[i]).string());
        client.open(uri, kTexts[i]);
        const json d = client.wait_notification(
            "textDocument/publishDiagnostics", [&](const json& p) { return p.value("uri", "") == uri; }, 30000);
        ASSERT_FALSE(d.is_null()) << kFiles[i];
        for (const json& e : d["params"]["diagnostics"])
            EXPECT_NE(e.value("severity", 1), 1) << kFiles[i] << ": " << e.dump();
    }
}

} // namespace
