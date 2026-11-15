// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The matrix of the completion requests of hot-ls (HOT-9). The server answers a completion
// request with clangd over the generated C++ and maps the answer back to the script (D5).
// A row is a position kind: the text before the cursor. The columns are the checks:
//   - the expected names are in the list,
//   - the names that must not be there are absent,
//   - no generated name leaks: "__self", "hot::", "__iface" and "hotc_" are not in a label,
//   - the textEdit range of an item is inside the script line of the cursor.
// One server and one script file serve all the cases. A case sends the new text with didChange.
// A case that fails on the current server is a hole. The list kHoles has the observed result of
// each hole. A hole case is skipped. When a hole case passes, the case fails: remove it from kHoles.
#include "integration/ls/ls_client.h"
#include "long_path.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using ls_test::json;
using ls_test::LsClient;
using translator_test::TempDir;

namespace
{

// The native header of the matrix.
const char* const kHeader = R"(#pragma once
struct Inner
{
    int deep = 0;
    void Poke() {}
};
struct Holder
{
    Inner Get() { return Inner(); }
    Inner* Ptr() { return nullptr; }
};
struct Node
{
    template <class T> T* GetComponent() { return nullptr; }
};
struct Mathx
{
    static int Abs(int v) { return v < 0 ? -v : v; }
    static const int Pi = 3;
};
namespace gfx
{
struct Color
{
    int r = 0;
};
inline int Blend(int a) { return a; }
} // namespace gfx
)";

const char* const kLib = R"(const int LibLimit = 3;
class LibBase {
    int baseHp = 1;
    void baseHeal() { }
}
class WeaponCatalog : LibBase {
    static const int MaxAmmo = 9;
    static int Made(int k) { return k; }
    int ammo = 1;
    void fire() { }
}
)";

const char* const kUsings = "using \"Actor.h\";\nusing \"Cm.h\";\nusing \"lib.hot\";\n";

const char* const kDecls = R"(enum Mode { Idle, Walk }
struct Point {
    float x = 0.0;
}
int total = 0;
class Base : Actor {
    int baseField = 0;
    void baseMethod() { }
}
class Counter {
    static const int Max = 4;
    static int Made(int k) { return k; }
    int inst = 0;
}
class Hero : Base {
    int ticks = 0;
    Point origin;
)";

const char* const kRun = R"(    void run(float speed, $Base pb) {
        int counter = 1;
        Mode current = Mode.Idle;
        Point pt;
        Vec3 vec;
        $Base nb = null;
        Holder h;
        Node node;
        Point[3] arr;
        $WeaponCatalog lt = new $WeaponCatalog();
)";

enum class Wrap
{
    Body,   // a line inside the method "run"
    Member, // a line in the class "Hero"
    Module, // a line after the classes
    Top     // a line before the imports
};

struct Case
{
    const char* name;
    const char* kind; // the position kind, for the report
    Wrap wrap;
    const char* snippet; // "@" is the cursor
    std::vector<std::string> expect;
    std::vector<std::string> absent;
};

const std::vector<Case> kCases = {
    // After a dot.
    {"LocalDot", "dot on a local", Wrap::Body, "pt.@", {"x"}, {"baseField"}},
    {"FieldDot", "dot on a field", Wrap::Body, "origin.@", {"x"}, {"ticks"}},
    {"ParamDot", "dot on a parameter", Wrap::Body, "pb.@", {"baseField", "baseMethod", "Move"}, {"ticks"}},
    {"ThisDot", "dot on this", Wrap::Body, "this.@", {"ticks", "baseField", "baseMethod", "Move", "run"},
        {"counter", "total"}},
    {"HandleNullSafeDot", "dot on a handle with ?", Wrap::Body, "nb?.@", {"baseField", "baseMethod", "Move"}, {"ticks"}},
    {"StaticClassDot", "static class name dot", Wrap::Body, "Counter.@", {"Made", "Max"}, {"inst"}},
    {"NamespaceDot", "namespace dot", Wrap::Body, "gfx.@", {"Color", "Blend"}, {"Abs"}},
    {"NativeTypeStaticDot", "native type with statics dot", Wrap::Body, "Mathx.@", {"Abs", "Pi"}, {}},
    {"NativeStructDot", "native struct type dot (Vec3.)", Wrap::Body, "Vec3.@", {}, {}},
    {"NativeChainDot", "native method result chain", Wrap::Body, "h.Get().@", {"deep", "Poke"}, {"Get"}},
    {"NativePtrChainDot", "native method pointer result chain", Wrap::Body, "h.Ptr().@", {"deep", "Poke"}, {}},
    {"ArrayElementDot", "array element dot", Wrap::Body, "arr[0].@", {"x"}, {"size"}},
    {"EnumDot", "enum name dot", Wrap::Body, "Mode.@", {"Idle", "Walk"}, {"x"}},
    {"ImportedLocalDot", "imported script class member", Wrap::Body, "lt.@", {"fire", "ammo", "baseHeal"}, {"MaxAmmo"}},
    {"ImportedStaticDot", "imported script class static", Wrap::Body, "WeaponCatalog.@", {"MaxAmmo", "Made"}, {"ammo"}},
    {"ImportedBaseThisDot", "base class member from an import", Wrap::Module,
        "class K3 : LibBase {\n    void f() { this.@ }\n}", {"baseHp", "baseHeal", "f"}, {}},
    // A bare identifier.
    {"BarePrefix", "bare prefix in a method body", Wrap::Body, "cou@", {"counter"}, {}},
    {"BareEmpty", "bare empty in a method body", Wrap::Body, "@",
        {"counter", "speed", "ticks", "total", "Hero", "Vec3", "baseField", "baseMethod", "origin"}, {"inst"}},
    {"BareDerivedBase", "base class member in a derived script", Wrap::Body, "baseF@", {"baseField"}, {}},
    {"BareAfterOperator", "bare after an operator", Wrap::Body, "int q = 1 + @", {"counter", "ticks"}, {}},
    {"FieldInitPrefix", "field initializer prefix", Wrap::Member, "int z = t@", {"ticks", "total"}, {}},
    {"FieldInitDot", "field initializer dot", Wrap::Member, "float fx = origin.@", {"x"}, {"ticks"}},
    // A type position.
    {"FieldType", "field type", Wrap::Member, "private @", {"Point", "Base", "Vec3", "Mode"}, {"ticks"}},
    {"FieldTypePrefix", "field type prefix", Wrap::Member, "private Po@", {"Point"}, {}},
    {"ParamType", "parameter type", Wrap::Member, "void F(@", {"Point", "Base", "Mode"}, {}},
    {"ParamTypeSecond", "second parameter type", Wrap::Member, "void F(int a, @", {"Point", "Base"}, {}},
    {"ReturnType", "return type", Wrap::Member, "@ G() { }", {"Point", "Base"}, {}},
    {"LocalType", "local type", Wrap::Body, "Po@", {"Point"}, {}},
    {"LocalHandleType", "local handle type", Wrap::Body, "$Ba@", {"Base"}, {}},
    {"BaseList", "base list", Wrap::Module, "class K : @ { }", {"Base", "Actor", "Hero"}, {"K"}},
    {"BaseListImported", "base list with an import", Wrap::Module, "class K2 : LibB@ { }", {"LibBase"}, {}},
    {"TemplateArgument", "template argument GetComponent<", Wrap::Body, "node.GetComponent<@", {"Base", "Hero"}, {}},
    {"NewType", "after new", Wrap::Body, "$Base b2 = new @", {"Base", "Hero"}, {"counter"}},
    {"NewTypePrefix", "after new with a prefix", Wrap::Body, "$Base b2 = new Ba@", {"Base"}, {}},
    // The special places.
    {"UsingString", "inside a using string", Wrap::Top, "using \"@", {"Cm.h", "lib.hot"}, {"main.hot"}},
    {"UsingStringPartial", "inside a using string with a prefix", Wrap::Top, "using \"li@", {"lib.hot"}, {"Cm.h"}},
    {"Override", "after override", Wrap::Member, "override @", {"Tick"}, {"Move"}},
    {"OverridePrefix", "after override with a prefix", Wrap::Member, "override Ti@", {"Tick"}, {}},
};

// Each hole: the case name and the observed result. A hole case is skipped.
const std::map<std::string, std::string> kHoles = {
};

std::string uri_of(const std::string& path)
{
    return "file://" + hot::ls::long_path(path);
}

struct Workspace
{
    TempDir dir;
    TempDir inc;
    std::unique_ptr<LsClient> client;
    std::string path, uri;
    int version = 1;

    Workspace()
    {
        std::ofstream((inc.path() / "Cm.h").string(), std::ios::binary) << kHeader;
        std::ofstream((dir.path() / "lib.hot").string(), std::ios::binary) << kLib;
        path = (dir.path() / "main.hot").string();
        std::ofstream(path, std::ios::binary) << "using \"Actor.h\";\n";
        uri = uri_of(path);
        hot::ls::ServerOptions o;
        o.clangd = HOT_CLANGD_PATH;
        o.include_dirs = {HOT_SOURCE_DIR "/include", HOT_SOURCE_DIR "/tests/engine", inc.path().string()};
        o.clangd_log = (dir.path() / "clangd.log").string();
        client = std::make_unique<LsClient>(o);
        client->initialize(uri_of(dir.path().string()));
        client->open(uri, "using \"Actor.h\";\n");
    }
};

Workspace* g_ws = nullptr;
int g_holes_seen = 0;

std::string build_text(const Case& c)
{
    const std::string snip = c.snippet;
    switch (c.wrap)
    {
    case Wrap::Body:
        return std::string(kUsings) + kDecls + kRun + "        " + snip + "\n    }\n}\n";
    case Wrap::Member:
        return std::string(kUsings) + kDecls + "    " + snip + "\n    void run() { }\n}\n";
    case Wrap::Module:
        return std::string(kUsings) + kDecls + "    void run() { }\n}\n" + snip + "\n";
    case Wrap::Top:
        return snip + "\n" + kUsings + kDecls + "    void run() { }\n}\n";
    }
    return {};
}

// "" when the case passes, else the one-line observed result.
std::string run_case(const Case& c)
{
    std::string text = build_text(c);
    const size_t at = text.find('@');
    text.erase(at, 1);
    const int line = static_cast<int>(std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
    const size_t ls = text.rfind('\n', at == 0 ? 0 : at - 1);
    const int col = static_cast<int>(at - (ls == std::string::npos || at == 0 ? 0 : ls + 1));
    const size_t le = text.find('\n', at);
    const int line_len = static_cast<int>(le - (at - col));

    g_ws->client->change(g_ws->uri, ++g_ws->version, text);
    const json r = g_ws->client->request("textDocument/completion",
        {{"textDocument", {{"uri", g_ws->uri}}}, {"position", {{"line", line}, {"character", col}}}});
    if (!r.contains("result") || r["result"].is_null())
        return "no result: " + r.dump().substr(0, 120);
    const json& res = r["result"];
    const json items = res.is_array() ? res : res.value("items", json::array());
    std::vector<std::string> names;
    std::string problems;
    for (const json& i : items)
    {
        const std::string label = i.value("label", "");
        const std::string ins = i.value("insertText", label);
        names.push_back(ins);
        names.push_back(label);
        for (const std::string& s : {label, ins, i.value("filterText", std::string())})
            for (const char* bad : {"__self", "hot::", "__iface", "hotc_", "creator", "live_next", "live_prev", "shared_from_this"})
                if (s.find(bad) != std::string::npos && problems.find("leak " + s) == std::string::npos)
                    problems += "leak '" + s + "'; ";
        if (i.contains("textEdit") && i["textEdit"].contains("range"))
        {
            const json& g = i["textEdit"]["range"];
            const int sl = g["start"]["line"], el = g["end"]["line"], sc = g["start"]["character"],
                      ec = g["end"]["character"];
            if (sl != line || el != line || sc > col || ec < col || ec > line_len)
            {
                if (problems.find("range") == std::string::npos)
                    problems += "range of '" + label + "' is " + std::to_string(sl) + ":" + std::to_string(sc) + "-"
                        + std::to_string(el) + ":" + std::to_string(ec) + " (cursor " + std::to_string(line) + ":"
                        + std::to_string(col) + ", line length " + std::to_string(line_len) + "); ";
            }
        }
    }
    auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    for (const std::string& e : c.expect)
        if (!has(e))
            problems += "missing '" + e + "'; ";
    for (const std::string& a : c.absent)
        if (has(a))
            problems += "unwanted '" + a + "'; ";
    if (problems.empty())
        return "";
    std::string sample;
    for (size_t i = 0; i < items.size() && i < 8; ++i)
        sample += (i ? "," : "") + items[i].value("label", "");
    return problems + std::to_string(items.size()) + " items [" + sample + "]";
}

class CompletionMatrix : public ::testing::TestWithParam<size_t>
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
        std::printf("[ CompletionMatrix ] %zu cases, %zu in kHoles, %d holes seen\n", kCases.size(), kHoles.size(),
            g_holes_seen);
        delete g_ws;
        g_ws = nullptr;
    }
};

TEST_P(CompletionMatrix, Case)
{
    if (!g_ws)
        GTEST_SKIP() << "clangd is not installed";
    const Case& c = kCases[GetParam()];
    const std::string observed = run_case(c);
    const auto hole = kHoles.find(c.name);
    if (hole != kHoles.end())
    {
        if (observed.empty())
            FAIL() << "the hole is fixed: remove " << c.name << " from kHoles";
        ++g_holes_seen;
        GTEST_SKIP() << "hole: " << hole->second;
    }
    EXPECT_TRUE(observed.empty()) << c.kind << ": " << observed;
}

std::vector<size_t> indices()
{
    std::vector<size_t> v(kCases.size());
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = i;
    return v;
}

INSTANTIATE_TEST_SUITE_P(Matrix, CompletionMatrix, ::testing::ValuesIn(indices()),
    [](const ::testing::TestParamInfo<size_t>& i) { return std::string(kCases[i.param].name); });

} // namespace
