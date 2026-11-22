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
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
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
    public virtual int Reload(int n) { return n; }
    public int Rank { get { return ammo + 1; } set { ammo = value; } }
    public int Rate() { Rank = 2; return Rank; }
    public void Drift() { Vec3 d; this.Move(d); }
}
)";

const char* const kClosed = R"(using "Actor.h";
using "lib.hot";
class Rifle : Gun {
    public override int Fire(int bullets) { int shots = bullets + this.ammo + Gun.Max + Limit + Twice(1) + Gun.Make(2); Mode m = Mode.Fast; return shots; }
    public override int Reload(int n) { return n + 1; }
    public int Shoot() { return this.Fire(1) + this.Rank; }
    public void Nudge() { Vec3 d; this.Move(d); }
}
)";

const char* const kOpen = R"(using "Actor.h";
using "lib.hot";
class Pistol : Gun {
    public override int Fire(int bullets) { int shots = bullets + this.ammo + Gun.Max + Limit + Twice(1) + Gun.Make(2); Mode m = Mode.Fast; return shots; }
    public override int Reload(int n) { return n + 1; }
    public int Shoot() { return this.Fire(1) + this.Rank; }
    public void Nudge() { Vec3 d; this.Move(d); }
}
)";

const char* const kTop = R"(using "Actor.h";
using "closed.hot";
class Cannon : Rifle {
    public override int Fire(int bullets) { int shots = bullets + this.ammo + Gun.Max + Limit + Twice(1) + Gun.Make(2); Mode m = Mode.Fast; return shots; }
    public override int Reload(int n) { return n + 2; }
    public int Blast() { return this.Fire(1) + this.Rank; }
    public void Shove() { Vec3 d; this.Move(d); }
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
    public virtual int Reload(int n) { return n; }
    public int Rank { get { return ammo + 2; } set { ammo = value; } }
    public int Rate() { Rank = 3; return Rank; }
    public void Drift() { Vec3 d; this.Move(d); }
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
    // A native symbol of Actor.h: the definition is the text of "native_ctx" in the header, the
    // word is the same symbol in all the files, and a rename is not possible.
    const char* native_ctx = nullptr;
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
    {"Property", "Rank", false},
    {"SeparateOverride", "Reload", false, true},
    {"NativeType", "Vec3", false, false, "struct Vec3"},
    {"NativeMethod", "Move", false, false, "void Move("},
};

// The cases that fail on the current server. Each case has a reason. No case fails now.
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
#ifdef _WIN32
    // The same form as the server: file:///C:/dir/a.hot.
    std::string p = hot::ls::long_path(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    return "file:///" + p;
#else
    return "file://" + hot::ls::long_path(path);
#endif
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
        {
            client->open(uris[i], kTexts[i]);
            // Wait for the preamble, so the first rename does not meet a cold clangd.
            client->wait_notification(
                "textDocument/publishDiagnostics", [&, i](const json& p) { return p.value("uri", "") == uris[i]; }, 60000);
        }
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
    const bool native = row.native_ctx != nullptr;
    const bool same_symbol = (native || c.axis != Axis::Unrelated) && !indirect_fn && (!row.file_local || c.axis == Axis::Same);
    // The import is not transitive: a free function of lib.hot is not visible in top.hot.
    if (indirect_fn && (c.op == Op::Definition || c.op == Op::PrepareRename))
        return {};
    switch (c.op)
    {
    case Op::Definition:
    {
        if (native)
        {
            std::ifstream in(HOT_SOURCE_DIR "/tests/engine/Actor.h", std::ios::binary);
            const std::string h((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto at = words(h.substr(h.find(row.native_ctx)), row.word).front();
            const int base = static_cast<int>(std::count(h.begin(), h.begin() + static_cast<long>(h.find(row.native_ctx)), '\n'));
            // The context starts at a line start or after "void ": the column is of the first line.
            const size_t cs = h.find(row.native_ctx);
            const size_t ls = h.rfind('\n', cs - 1) + 1;
            return {Place{uri_of(HOT_SOURCE_DIR "/tests/engine/Actor.h"), base + at.first,
                at.first == 0 ? static_cast<int>(cs - ls) + at.second : at.second}};
        }
        // The first word in a file is the declaration.
        const int decl_file = same_symbol && !row.def_in_axis ? 0 : f;
        const auto first = words(kTexts[decl_file], row.word).front();
        return {Place{g_ws->uris[decl_file], first.first, first.second}};
    }
    case Op::PrepareRename:
    {
        // The server refuses the rename of a native symbol: no range.
        if (native)
            return {};
        const auto last = words(kTexts[f], row.word).back();
        return {Place{g_ws->uris[f], last.first, last.second}};
    }
    case Op::References:
    case Op::Rename:
        if (native && c.op == Op::Rename)
            return {};
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

// Latency with many importers. One script declares a class with a member and a static. 30 closed
// scripts import it and use both. The test counts the work with the table counters of
// hot/indexStats. The time limit is a guard against a blow-up, not a benchmark.
// Measured (HOT-20), Debug: before the fix the first request took about 9 s. Each of the 30
// translations parsed the imported header again (about 250 ms), because the session cache missed
// on the new class name. Now the 30 translations take about 36 ms and the first request takes
// about 2.7 s with the test setup. Warm requests take 0 to 3 ms.
TEST(RefMatrixLatency, ThirtyImportersAreAnalyzedOnce)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    constexpr int kImporters = 30;
    const std::string lib_text = "using \"Actor.h\";\nclass Gun : Actor {\n    public int ammo = 0;\n"
                                 "    public static const int Max = 4;\n}\n";
    TempDir dir;
    const std::string lib = (dir.path() / "lib.hot").string();
    std::ofstream(lib, std::ios::binary) << lib_text;
    std::set<std::string> importer_uris;
    for (int i = 0; i < kImporters; ++i)
    {
        const std::string path = (dir.path() / ("imp" + std::to_string(i) + ".hot")).string();
        std::ofstream(path, std::ios::binary) << "using \"Actor.h\";\nusing \"lib.hot\";\nclass Kid" << i
                                               << " : Gun {\n    public int Go() { return this.ammo + Gun.Max; }\n}\n";
        importer_uris.insert(uri_of(path));
    }
    hot::ls::ServerOptions o;
    o.clangd = HOT_CLANGD_PATH;
    o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine"};
    LsClient client(o);
    client.initialize(uri_of(dir.path().string()));
    client.open(uri_of(lib), lib_text);
    auto stats = [&] { return client.request("hot/indexStats", json::object())["result"]; };
    auto millis = [](std::chrono::steady_clock::time_point t0)
    { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };

    // The symbols: ammo at line 2, Max at line 3 of lib.hot.
    struct Symbol
    {
        const char* word;
        int line;
        int character;
    };
    const Symbol symbols[] = {{"ammo", 2, 15}, {"Max", 3, 31}};
    bool first = true;
    for (const Symbol& sym : symbols)
    {
        SCOPED_TRACE(sym.word);
        json p = {{"textDocument", {{"uri", uri_of(lib)}}}, {"position", {{"line", sym.line}, {"character", sym.character}}}};
        p["context"] = {{"includeDeclaration", true}};
        const json before = stats();
        auto t0 = std::chrono::steady_clock::now();
        json r = client.request("textDocument/references", p);
        const long long refs_ms = millis(t0);
        const json after = stats();
        ASSERT_TRUE(r.contains("result")) << r.dump().substr(0, 400);
        // Each importer has 1 place. The declaring file has 1 declaration (the static has a use too).
        std::set<std::string> files;
        size_t places = 0;
        for (const json& l : r["result"])
        {
            files.insert(l["uri"].get<std::string>());
            ++places;
        }
        EXPECT_EQ(files.size(), static_cast<size_t>(kImporters + 1));
        EXPECT_EQ(places, static_cast<size_t>(kImporters + 1));
        for (const std::string& u : importer_uris)
            EXPECT_TRUE(files.count(u)) << u;
        const int analyses = after.value("tableAnalyses", -1) - before.value("tableAnalyses", 0);
        if (first)
            EXPECT_LE(after.value("tableAnalyses", -1), kImporters + 10);
        std::printf("[ RefMatrixLatency ] %s references: %lld ms, analyses +%d, hits +%d\n", sym.word, refs_ms, analyses,
            after.value("tableHits", 0) - before.value("tableHits", 0));
        std::printf("[ RefMatrixLatency ] %s translations %d, translate %.0f ms\n", sym.word,
            after.value("translations", 0) - before.value("translations", 0),
            after.value("translateMs", 0.0) - before.value("translateMs", 0.0));
        first = false;

        // The second request: the warm tables, no new analyze.
        t0 = std::chrono::steady_clock::now();
        r = client.request("textDocument/references", p);
        const long long warm_ms = millis(t0);
        const json warm = stats();
        EXPECT_EQ(warm.value("tableAnalyses", -1), after.value("tableAnalyses", -2));
        EXPECT_GT(warm.value("tableHits", -1), after.value("tableHits", 0));
        std::printf("[ RefMatrixLatency ] %s warm references: %lld ms, analyses +%d, hits +%d\n", sym.word, warm_ms,
            warm.value("tableAnalyses", 0) - after.value("tableAnalyses", 0),
            warm.value("tableHits", 0) - after.value("tableHits", 0));

        // The rename. The time limit is a guard, not a benchmark.
        p.erase("context");
        p["newName"] = "Renamed";
        t0 = std::chrono::steady_clock::now();
        r = client.request("textDocument/rename", p);
        const long long rename_ms = millis(t0);
        EXPECT_LT(rename_ms, 20000) << "guard against a blow-up";
        ASSERT_TRUE(r.contains("result") && r["result"].is_object()) << r.dump().substr(0, 400);
        const json& changes = r["result"]["changes"];
        EXPECT_EQ(changes.size(), static_cast<size_t>(kImporters + 1)) << r.dump().substr(0, 400);
        for (const std::string& u : importer_uris)
        {
            ASSERT_TRUE(changes.contains(u)) << u;
            EXPECT_EQ(changes[u].size(), 1u);
        }
        const json end = stats();
        EXPECT_LE(end.value("tableAnalyses", -1), kImporters + 10);
        std::printf("[ RefMatrixLatency ] %s rename: %lld ms, analyses total %d, hits total %d\n", sym.word, rename_ms,
            end.value("tableAnalyses", -1), end.value("tableHits", -1));
    }
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
        {"member of a derived class", "ammo", "Shoot", ".hot:6"},
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

// HOT-35: the memo of the clangd answers for the native names in closed scripts. The header
// "nat.h" has two overloads of a method, a free function and a type. Two closed scripts use them.
const char* const kNatHeader = "#pragma once\nstruct Box { int Put(int v); int Put(float v, int w); };\nint Free(int v);\n";
const char* const kNatOpen = "using \"nat.h\";\nclass Open {\n    public int Go() { Box b; return b.Put(1) + Free(1); }\n}\n";
const char* const kNatClosedOne = "using \"nat.h\";\nclass One {\n    public int Go() { Box b; return b.Put(1) + Free(2); }\n}\n";
const char* const kNatClosedTwo = "using \"nat.h\";\nclass Two {\n    public int Go() { Box b; return b.Put(2.5, 3) + Free(4); }\n}\n";

struct NativeMemoWs
{
    TempDir dir;
    std::unique_ptr<LsClient> client;
    std::string open_uri, one_uri, two_uri;

    explicit NativeMemoWs(int budget_ms = 10000, int extra = 0,
        const std::vector<std::pair<std::string, std::string>>& files = {}, const std::string& hosts_dir = "")
    {
        auto put = [&](const char* name, const char* text)
        {
            const std::string p = (dir.path() / name).string();
            std::ofstream(p, std::ios::binary) << text;
            return uri_of(p);
        };
        put("nat.h", kNatHeader);
        open_uri = put("open.hot", kNatOpen);
        one_uri = put("one.hot", kNatClosedOne);
        two_uri = put("two.hot", kNatClosedTwo);
        for (const auto& [name, text] : files)
            std::ofstream((dir.path() / name).string(), std::ios::binary) << text;
        for (int i = 0; i < extra; ++i)
            std::ofstream((dir.path() / ("more" + std::to_string(i) + ".hot")).string(), std::ios::binary)
                << "using \"nat.h\";\nclass More" << i << " {\n    public int Go() { return Free(" << i << "); }\n}\n";
        hot::ls::ServerOptions o;
        o.clangd = HOT_CLANGD_PATH;
        o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine", dir.path().string()};
        o.clangd_log = (dir.path() / "clangd.log").string();
        o.index_dir = (dir.path() / "index").string();
        o.reference_budget_ms = budget_ms;
        o.hosts_dir = hosts_dir;
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(dir.path().string()));
        client->open(open_uri, kNatOpen);
    }
    json stats() { return client->request("hot/indexStats", json::object())["result"]; }
    // The references of the word at the first use of "context" in open.hot, only the places in the closed scripts.
    std::set<Place> refs(const std::string& context, const std::string& word)
    {
        const std::string text = kNatOpen;
        const size_t at = text.find(context) + context.rfind(word);
        const auto place = words(text.substr(0, at) + word, word).back();
        json p = doc_pos(open_uri, place);
        p["context"] = {{"includeDeclaration", true}};
        std::set<Place> out;
        for (const Place& pl : places_of(client->request("textDocument/references", p).value("result", json())))
            if (std::get<0>(pl) != open_uri && std::get<0>(pl).find("nat.h") == std::string::npos) // clangd alone gives the header place
                out.insert(pl);
        return out;
    }
};

TEST(NativeMemo, SecondRunUsesTheMemo)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    NativeMemoWs ws;
    struct Item
    {
        const char* context;
        const char* word;
        bool in_one, in_two;
    };
    // The overload Put(int) matches only one.hot. Put(float, int) is in two.hot, not in one.hot.
    for (const Item& it : {Item{"b.Put(1)", "Put", true, false}, Item{"Free(1)", "Free", true, true},
             Item{"Box b;", "Box", true, true}})
    {
        SCOPED_TRACE(it.word);
        const json s0 = ws.stats();
        const std::set<Place> first = ws.refs(it.context, it.word);
        const json s1 = ws.stats();
        const std::set<Place> second = ws.refs(it.context, it.word);
        const json s2 = ws.stats();
        EXPECT_EQ(show(second), show(first));
        auto has = [&](const std::string& uri)
        {
            return std::any_of(first.begin(), first.end(), [&](const Place& p) { return std::get<0>(p) == uri; });
        };
        EXPECT_EQ(has(ws.one_uri), it.in_one) << show(first);
        EXPECT_EQ(has(ws.two_uri), it.in_two) << show(first);
        EXPECT_GT(s1.value("nativeMemoMisses", 0), s0.value("nativeMemoMisses", 0));
        EXPECT_GT(s2.value("nativeMemoHits", 0), s1.value("nativeMemoHits", 0));
        EXPECT_EQ(s2.value("nativeMemoMisses", -1), s1.value("nativeMemoMisses", -2)); // no new temporary document
    }
}

TEST(NativeMemo, AHeaderEditDropsTheMemoAndTheResultStaysCorrect)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    NativeMemoWs ws;
    const std::set<Place> first = ws.refs("b.Put(1)", "Put");
    ws.refs("b.Put(1)", "Put"); // the memo serves this one
    const json s1 = ws.stats();
    ASSERT_GT(s1.value("nativeMemoHits", 0), 0);
    // Lines above the declarations: the size of the target file changes.
    const std::string header = (ws.dir.path() / "nat.h").string();
    std::ofstream(header, std::ios::binary | std::ios::trunc) << "#pragma once\n\n\n" << (kNatHeader + 13);
    ws.client->change(ws.open_uri, 2, std::string(kNatOpen) + "\n");
    std::set<Place> after = ws.refs("b.Put(1)", "Put");
    const json s2 = ws.stats();
    // clangd needs some time to read the new header for the open script.
    for (int i = 0; i < 20 && after.empty(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        after = ws.refs("b.Put(1)", "Put");
    }
    EXPECT_GT(s2.value("nativeMemoMisses", 0), s1.value("nativeMemoMisses", 0));
    EXPECT_EQ(s2.value("nativeMemoHits", -1), s1.value("nativeMemoHits", -2));
    auto count_in = [&](const std::set<Place>& s, const std::string& uri)
    {
        return std::count_if(s.begin(), s.end(), [&](const Place& p) { return std::get<0>(p) == uri; });
    };
    EXPECT_EQ(count_in(after, ws.one_uri), count_in(first, ws.one_uri)) << show(after);
    EXPECT_EQ(count_in(after, ws.two_uri), 0) << show(after);
}

TEST(NativeMemo, AScanThatTheBudgetCutsStoresNothing)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    NativeMemoWs ws(0);
    const json s0 = ws.stats();
    ws.refs("b.Put(1)", "Put");
    const json s1 = ws.stats();
    EXPECT_EQ(s1.value("nativeMemoMisses", -1), s0.value("nativeMemoMisses", -2));
    EXPECT_EQ(s1.value("nativeMemoHits", -1), s0.value("nativeMemoHits", -2));
}

// The time of the first and of the second native references request with 12 closed scripts.
TEST(NativeMemo, TimeOfTheFirstAndTheSecondRequest)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    NativeMemoWs ws(120000, 10);
    auto timed = [&]
    {
        const auto t0 = std::chrono::steady_clock::now();
        const std::set<Place> r = ws.refs("Free(1)", "Free");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        std::printf("[ NativeMemo ] references of Free, 12 closed scripts: %lld ms\n", static_cast<long long>(ms.count()));
        return r;
    };
    const std::set<Place> first = timed();
    const std::set<Place> second = timed();
    EXPECT_EQ(show(second), show(first));
    EXPECT_EQ(first.size(), 12u) << show(first);
}

// An empty answer (no declaration visible) is valid only while the headers of the script are the same.
// late.hot includes the empty header extra.h and calls Free: the answer is empty. Then extra.h includes nat.h.
TEST(NativeMemo, AnEmptyAnswerIsDroppedWhenAHeaderAddsTheDeclaration)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    NativeMemoWs ws(10000, 0,
        {{"extra.h", "#pragma once\n"},
            {"late.hot", "using \"extra.h\";\nclass Late {\n    public int Go() { return Free(5); }\n}\n"}});
    const std::string late_uri = uri_of((ws.dir.path() / "late.hot").string());
    auto has_late = [&](const std::set<Place>& s)
    {
        return std::any_of(s.begin(), s.end(), [&](const Place& p) { return std::get<0>(p) == late_uri; });
    };
    EXPECT_FALSE(has_late(ws.refs("Free(1)", "Free")));
    EXPECT_FALSE(has_late(ws.refs("Free(1)", "Free")));
    const json s1 = ws.stats();
    EXPECT_GT(s1.value("nativeMemoHits", 0), 0);
    std::ofstream((ws.dir.path() / "extra.h").string(), std::ios::binary | std::ios::trunc)
        << "#pragma once\n#include \"nat.h\"\n";
    EXPECT_TRUE(has_late(ws.refs("Free(1)", "Free")));
    const json s2 = ws.stats();
    EXPECT_GT(s2.value("nativeMemoMisses", 0), s1.value("nativeMemoMisses", 0));
    EXPECT_TRUE(has_late(ws.refs("Free(1)", "Free"))); // and the new answer is memoized
    EXPECT_GT(ws.stats().value("nativeMemoHits", 0), s2.value("nativeMemoHits", 0));
}

// A change of the host options (a flag of the host file) drops the memo of the script.
TEST(NativeMemo, AChangeOfTheHostFlagsDropsTheMemo)
{
    if (std::string(HOT_CLANGD_PATH).empty())
        GTEST_SKIP() << "clangd is not installed";
    TempDir hosts;
    const std::string root = hosts.path().string();
    std::filesystem::create_directories(hosts.path() / "build");
    std::ofstream((hosts.path() / "build" / "CMakeCache.txt").string()) << "# cache\n";
    NativeMemoWs ws(10000, 0, {}, "");
    // The host file lists the workspace of "ws" as its script dir.
    const std::string hosts_dir = (hosts.path() / "hosts").string();
    std::filesystem::create_directories(hosts_dir);
    auto write_host = [&](const std::string& flags)
    {
        std::ofstream((hosts.path() / "hosts" / "h.json").string(), std::ios::binary | std::ios::trunc)
            << R"({"version":1,"name":"h","binaryDir":")" << (hosts.path() / "build").generic_string()
            << R"(","compiler":"/usr/bin/c++","std":"c++20","resourceDir":"","scriptDirs":[")"
            << ws.dir.path().generic_string() << R"("],"scripts":[],"includeDirs":[")" << ws.dir.path().generic_string()
            << R"(",")" << HOT_SOURCE_DIR << R"(/include",")" << HOT_SOURCE_DIR << R"(/tests/engine"],"defines":[],"flags":[)"
            << flags << "]}";
    };
    write_host("");
    // A second server with the host: the first server of "ws" has no host.
    hot::ls::ServerOptions o;
    o.clangd = HOT_CLANGD_PATH;
    o.hosts_dir = hosts_dir;
    o.index_dir = (hosts.path() / "index").string();
    o.clangd_log = (hosts.path() / "clangd.log").string();
    LsClient client(o);
    client.initialize(uri_of(ws.dir.path().string()));
    client.open(ws.open_uri, kNatOpen);
    auto stats = [&] { return client.request("hot/indexStats", json::object())["result"]; };
    json p = doc_pos(ws.open_uri, words(kNatOpen, "Free").back());
    p["context"] = {{"includeDeclaration", true}};
    client.request("textDocument/references", p);
    client.request("textDocument/references", p);
    const json s1 = stats();
    ASSERT_GT(s1.value("nativeMemoHits", 0), 0) << s1.dump();
    write_host(R"("-DHOT_NAT_FLAG=1")");
    client.notify("workspace/didChangeWatchedFiles",
        {{"changes", json::array({{{"uri", uri_of((hosts.path() / "hosts" / "h.json").string())}, {"type", 2}}})}});
    client.change(ws.open_uri, 2, std::string(kNatOpen) + "\n");
    client.request("textDocument/references", p);
    const json s2 = stats();
    EXPECT_GT(s2.value("nativeMemoMisses", 0), s1.value("nativeMemoMisses", 0));
    EXPECT_EQ(s2.value("nativeMemoHits", -1), s1.value("nativeMemoHits", -2));
}

} // namespace
