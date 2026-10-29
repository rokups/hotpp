#include "server.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <variant>

#include <nlohmann/json.hpp>

#include "clangd_process.h"
#include "jsonrpc.h"
#include "semantic.h"
#include "symbol_index.h"
#include "spp/clang_files_backend.h"
#include "spp/sppc_lib.h"

namespace fs = std::filesystem;

namespace spp::ls {

using json = nlohmann::json;

namespace {

// The name that the completion puts in place of the word at the cursor (see on_completion).
const std::string kHole = "__spp_complete";

// LSP error codes.
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kRequestFailed = -32803;

int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string uri_to_path(const std::string& uri)
{
    std::string s = uri.compare(0, 7, "file://") == 0 ? uri.substr(7) : uri;
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            r += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            r += s[i];
        }
    }
    return r;
}

std::string path_to_uri(const std::string& path)
{
    static const char* digits = "0123456789ABCDEF";
    std::string r = "file://";
    for (char ch : path) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (is_id_char(ch) || c == '/' || c == '-' || c == '.' || c == '~') {
            r += ch;
        } else {
            r += '%';
            r += digits[c >> 4];
            r += digits[c & 15];
        }
    }
    return r;
}

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A text with its line table. Positions are 0-based lines and byte columns.
struct Text {
    std::string s;
    std::vector<size_t> starts;
    Text() : starts{0} {}
    explicit Text(std::string t) : s(std::move(t)), starts(line_starts(s)) {}
    std::string_view line(int l) const { return line_text(s, starts, l); }
    size_t offset(int l, int c) const
    {
        if (l < 0) return 0;
        if (static_cast<size_t>(l) >= starts.size()) return s.size();
        return std::min(starts[static_cast<size_t>(l)] + static_cast<size_t>(std::max(c, 0)), s.size());
    }
    std::pair<int, int> pos(size_t off) const
    {
        const auto it = std::upper_bound(starts.begin(), starts.end(), off);
        const int l = static_cast<int>(it - starts.begin()) - 1;
        return {l, static_cast<int>(off - starts[static_cast<size_t>(l)])};
    }
};

struct Pos {
    int line = 0, col = 0;  // 0-based line, byte column
    bool operator<(const Pos& o) const { return std::tie(line, col) < std::tie(o.line, o.col); }
    bool operator==(const Pos& o) const { return line == o.line && col == o.col; }
};

// A script symbol that other scripts can have too: a class, or a member of a class.
struct SymbolKey {
    std::string cls, member;  // member is empty for a class
};

// One open .spp document and its state in clangd.
struct Doc {
    std::string uri, path;
    Text text;
    sppc::AnalyzeResult facts;
    // The generated C++ that clangd has as the real text, and its map. The map belongs to the
    // script text "gen_script". After a translation error, the last good state stays.
    std::string gen_path, gen_uri;
    Text gen;
    std::shared_ptr<const sppc::SourceMap> map;
    int gen_version = 0;   // the clangd version of the real text
    int sent_version = 0;  // the last clangd version
    bool in_clangd = false;
    json clang_diags = json::array();  // the mapped clangd diagnostics of gen_version
    // The URIs of the imported scripts that got translator errors from this document.
    std::set<std::string> import_uris;
};

class Server {
public:
    Server(int in, int out, ServerOptions o) : in_(in), out_(out), opt_(std::move(o)) { read_compile_commands(); }
    ~Server()
    {
        if (clangd_) clangd_->stop();
    }
    int run();

private:
    // ---- the connection to the client
    void send_client(const json& msg)
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        write_message(out_, msg.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    void reply(const json& id, json result) { send_client({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}); }
    void reply_error(const json& id, int code, const std::string& message)
    {
        send_client({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
    }
    void handle(const json& msg);

    // ---- positions
    Pos from_lsp(const Text& t, const json& p, bool utf8) const
    {
        const int l = p.value("line", 0), c = p.value("character", 0);
        return {l, utf8 ? c : utf16_to_byte(t.line(l), c)};
    }
    json to_lsp(const Text& t, Pos p, bool utf8) const
    {
        return {{"line", p.line}, {"character", utf8 ? p.col : byte_to_utf16(t.line(p.line), p.col)}};
    }
    json script_range(const Doc& d, Pos b, Pos e) const
    {
        return {{"start", to_lsp(d.text, b, utf8_)}, {"end", to_lsp(d.text, e, utf8_)}};
    }
    json gen_pos(const Doc& d, Pos p) const { return to_lsp(d.gen, p, clangd_utf8_); }
    std::optional<Pos> to_gen(const Doc& d, Pos p) const
    {
        if (!d.map) return std::nullopt;
        const auto g = d.map->map_to_generated(p.line + 1, p.col + 1);
        if (!g) return std::nullopt;
        return Pos{g->line - 1, g->col - 1};
    }
    std::optional<Pos> to_script(const Doc& d, const json& lsp_pos) const
    {
        if (!d.map) return std::nullopt;
        const Pos g = from_lsp(d.gen, lsp_pos, clangd_utf8_);
        const auto s = d.map->map_to_script(g.line + 1, g.col + 1);
        if (!s) return std::nullopt;
        return Pos{s->line - 1, s->col - 1};
    }
    // The word at a script position, or the word that ends there. Gives [begin, end) columns.
    std::pair<int, int> word_at(const Doc& d, Pos p) const
    {
        const std::string_view l = d.text.line(p.line);
        int b = std::min<int>(p.col, static_cast<int>(l.size()));
        if ((b >= static_cast<int>(l.size()) || !is_id_char(l[static_cast<size_t>(b)])) && b > 0 &&
            is_id_char(l[static_cast<size_t>(b - 1)]))
            --b;
        int e = b;
        while (b > 0 && is_id_char(l[static_cast<size_t>(b - 1)])) --b;
        while (e < static_cast<int>(l.size()) && is_id_char(l[static_cast<size_t>(e)])) ++e;
        return {b, e};
    }
    std::string word(const Doc& d, Pos p) const
    {
        const auto [b, e] = word_at(d, p);
        return std::string(d.text.line(p.line).substr(static_cast<size_t>(b), static_cast<size_t>(e - b)));
    }
    // A clangd location in the generated text of "d" as a script location, or null.
    json map_location(const Doc& d, const json& loc) const;

    // ---- documents
    std::shared_ptr<Doc> doc_of(const json& params)
    {
        const std::string uri = params.value("/textDocument/uri"_json_pointer, std::string());
        const auto it = docs_.find(uri);
        return it == docs_.end() ? nullptr : it->second;
    }
    sppc::TranslateOptions translate_options(const Doc& d) const
    {
        sppc::TranslateOptions o;
        o.script_name = d.path;
        o.generation = 1;
        o.include_dirs = opt_.include_dirs;
        o.resource_dir = opt_.resource_dir;
        return o;
    }
    void update(Doc& d);
    void send_generated(Doc& d, const std::string& cpp);
    json publish_message(const Doc& d) const;
    std::map<std::string, json> import_diagnostics(const Doc& d) const;
    void publish_imports(Doc& d);
    json imported_location(const Doc& d, const json& gen_lsp_pos, bool whole_word) const;
    json analyze_diagnostics(const Doc& d) const;

    // ---- clangd
    void read_compile_commands();
    void start_clangd(const json& init);
    void on_clangd_notification(const json& msg);
    std::optional<json> clangd_result(const std::string& method, json params, int timeout_ms = 60000)
    {
        if (!clangd_ || !clangd_->running()) return std::nullopt;
        json r = clangd_->request(method, std::move(params), timeout_ms);
        if (!r.is_object() || !r.contains("result") || r["result"].is_null()) return std::nullopt;
        return r["result"];
    }

    // ---- features
    json on_initialize(const json& p);
    json on_completion(const Doc& d, const json& p);
    json on_hover(const Doc& d, const json& p);
    json on_locations(const Doc& d, const json& p, const std::string& method);
    json on_document_symbol(const Doc& d);
    json on_semantic_tokens(const Doc& d);
    json on_workspace_symbol(const json& p);
    json script_member_location(const std::string& uri, const std::string& script_name);
    json on_to_script(const json& p);
    // Gives the edit, or an error message.
    std::variant<json, std::string> on_rename(const Doc& d, const json& p);

    // ---- symbols across the .spp files of the workspace (S18)
    // The script places of the symbol at "cur": the declaration and each use. Or an error
    // message. "decl" gets the script place of the declaration.
    std::variant<std::set<Pos>, std::string> symbol_places(const Doc& d, Pos cur, const std::string& name, Pos* decl);
    // The class (and the member) that is declared at "decl", or no value for a local symbol.
    std::optional<SymbolKey> symbol_key(const Doc& d, Pos decl, const std::string& name) const;
    std::optional<SymbolKey> key_at(const Doc& d, Pos cur);
    std::optional<Pos> key_position(const Doc& d, const SymbolKey& key) const;
    Pos class_name_pos(const Doc& d, const sppc::AnalyzedClass& c) const;
    std::set<Pos> class_places(const Doc& d, const std::string& name) const;
    bool is_script_class(const Doc& d, const std::string& name) const;
    std::vector<std::string> workspace_scripts() const;
    IndexShard shard_of_doc(const Doc& d) const;
    // Calls "fn" for each other .spp file of the workspace that declares the symbol "key".
    // "places" are the places of the symbol in that file. An open document is used as it is.
    // A file on disk goes to clangd only for the call.
    void for_other_scripts(const Doc& origin, const SymbolKey& key, const std::string& name,
                           const std::function<void(const Doc&, const std::set<Pos>&)>& fn);

    int in_, out_;
    ServerOptions opt_;
    std::mutex out_mutex_;
    // docs_mutex_ guards the fields of the documents that the clangd reader thread reads. Only
    // the main thread changes a document. It never holds the lock during a clangd request.
    std::mutex docs_mutex_;
    std::map<std::string, std::shared_ptr<Doc>> docs_;      // by the script uri
    std::map<std::string, std::shared_ptr<Doc>> gen_docs_;  // by the generated uri
    std::unique_ptr<ClangdProcess> clangd_;
    std::vector<std::string> roots_;  // the workspace folders (paths), from initialize
    bool utf8_ = false, clangd_utf8_ = false, shutdown_ = false;
    std::set<std::string> preamble_ready_;
    // The analyze facts of the scripts on disk that are not open (script_member_location).
    std::map<std::string, std::pair<std::string, sppc::AnalyzeResult>> disk_facts_;
    std::string index_seed_;  // the first file of compile_commands.json (start_clangd)  // the generated files with a first completion
    // The saved symbol index of the scripts on disk (S28, symbol_index.h).
    SymbolIndex index_;
    int closed_analyses_ = 0;  // the analyze calls of for_other_scripts (spp/indexStats)
    sppc::TranslatorSession session_;
};

// ------------------------------------------------------------------ setup

void Server::read_compile_commands()
{
    if (opt_.compile_commands_dir.empty()) return;
    const auto text = read_file(opt_.compile_commands_dir + "/compile_commands.json");
    if (!text) return;
    const json db = json::parse(*text, nullptr, false);
    if (!db.is_array() || db.empty()) return;
    std::vector<std::string> args;
    if (db[0].contains("arguments")) {
        for (const json& a : db[0]["arguments"])
            if (a.is_string()) args.push_back(a.get<std::string>());
    } else if (db[0].contains("command")) {
        std::istringstream ss(db[0].value("command", std::string()));
        for (std::string a; ss >> a;) args.push_back(a);
    }
    const std::string dir = db[0].value("directory", std::string("."));
    index_seed_ = db[0].value("file", std::string());
    if (!index_seed_.empty() && fs::path(index_seed_).is_relative())
        index_seed_ = (fs::path(dir) / index_seed_).lexically_normal().string();
    auto add_dir = [&](std::string d) {
        if (fs::path(d).is_relative()) d = (fs::path(dir) / d).lexically_normal().string();
        for (const std::string& x : opt_.include_dirs)
            if (x == d) return;
        opt_.include_dirs.push_back(d);
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-I" && i + 1 < args.size()) add_dir(args[++i]);
        else if (a.compare(0, 2, "-I") == 0) add_dir(a.substr(2));
        else if ((a == "-isystem" || a == "-D") && i + 1 < args.size()) {
            opt_.extra_flags.push_back(a);
            opt_.extra_flags.push_back(args[++i]);
        } else if (a.compare(0, 2, "-D") == 0) opt_.extra_flags.push_back(a);
    }
}

void Server::start_clangd(const json& init)
{
    if (opt_.clangd.empty()) return;
    std::vector<std::string> args = {opt_.clangd_log.empty() ? "--log=error" : "--log=info", "--header-insertion=never", "-j=2"};
    if (!opt_.compile_commands_dir.empty()) {
        args.push_back("--compile-commands-dir=" + opt_.compile_commands_dir);
        args.push_back("--background-index");
    } else {
        args.push_back("--background-index=false");
    }
    auto c = std::make_unique<ClangdProcess>();
    if (!c->start(opt_.clangd, args, opt_.clangd_log, [this](const json& m) { on_clangd_notification(m); })) return;
    json params = {
        {"processId", static_cast<int>(getpid())},
        {"capabilities",
         {{"general", {{"positionEncodings", {"utf-8", "utf-16"}}}},
          {"textDocument",
           {{"completion", {{"completionItem", {{"snippetSupport", false}}}}},
            {"hover", {{"contentFormat", {"markdown", "plaintext"}}}},
            {"publishDiagnostics", {{"versionSupport", true}}}}}}},
    };
    for (const char* k : {"rootUri", "rootPath", "workspaceFolders"})
        if (init.contains(k)) params[k] = init[k];
    json r = c->request("initialize", params, 30000);
    if (!r.is_object() || !r.contains("result")) {
        c->stop();
        return;
    }
    clangd_utf8_ = r.value("/result/capabilities/positionEncoding"_json_pointer, std::string()) == "utf-8";
    c->notify("initialized", json::object());
    // clangd loads the compile_commands.json, and starts the background index, at the first
    // file that it opens. The server opens and closes one entry of the database for this.
    if (const auto seed = index_seed_.empty() ? std::nullopt : read_file(index_seed_)) {
        const std::string uri = path_to_uri(index_seed_);
        c->notify("textDocument/didOpen",
                  {{"textDocument", {{"uri", uri}, {"languageId", "cpp"}, {"version", 1}, {"text", *seed}}}});
        c->notify("textDocument/didClose", {{"textDocument", {{"uri", uri}}}});
    }
    clangd_ = std::move(c);
}

json Server::on_initialize(const json& p)
{
    const json encodings = p.value("/capabilities/general/positionEncodings"_json_pointer, json::array());
    for (const json& e : encodings)
        if (e == "utf-8") utf8_ = true;
    if (const json o = p.value("initializationOptions", json::object()); o.is_object()) {
        for (const json& d : o.value("includeDirs", json::array())) opt_.include_dirs.push_back(d.get<std::string>());
        if (o.contains("clangd")) opt_.clangd = o.value("clangd", opt_.clangd);
        if (o.contains("compileCommandsDir")) {
            opt_.compile_commands_dir = o.value("compileCommandsDir", std::string());
            read_compile_commands();
        }
    }
    roots_.clear();
    if (p.contains("workspaceFolders") && p["workspaceFolders"].is_array()) {
        for (const json& f : p["workspaceFolders"])
            if (f.is_object() && f.value("uri", std::string()).size()) roots_.push_back(uri_to_path(f.value("uri", std::string())));
    }
    if (roots_.empty() && p.value("rootUri", json()).is_string()) roots_.push_back(uri_to_path(p["rootUri"].get<std::string>()));
    if (roots_.empty() && p.value("rootPath", json()).is_string()) roots_.push_back(p["rootPath"].get<std::string>());
    if (!opt_.index_dir.empty()) index_.open(opt_.index_dir);
    else if (!roots_.empty()) index_.open((fs::path(roots_[0]) / ".cache" / "spp" / "index").string());
    index_.prune();
    start_clangd(p);
    json tokens = json::array();
    for (const std::string& t : semantic_token_types()) tokens.push_back(t);
    return {
        {"capabilities",
         {{"positionEncoding", utf8_ ? "utf-8" : "utf-16"},
          {"textDocumentSync", {{"openClose", true}, {"change", 1}}},
          {"completionProvider", {{"triggerCharacters", {"."}}}},
          {"hoverProvider", true},
          {"definitionProvider", true},
          {"referencesProvider", true},
          {"documentSymbolProvider", true},
          {"renameProvider", true},
          {"workspaceSymbolProvider", true},
          {"semanticTokensProvider",
           {{"legend", {{"tokenTypes", tokens}, {"tokenModifiers", json::array()}}}, {"full", true}}},
          {"experimental", {{"sppToScript", true}, {"clangd", clangd_ != nullptr}}}}},
        {"serverInfo", {{"name", "spp-ls"}, {"version", "0.1"}}},
    };
}

// ------------------------------------------------------------------ documents and diagnostics

void Server::send_generated(Doc& d, const std::string& cpp)
{
    if (!clangd_ || !clangd_->running()) return;
    const int v = ++d.sent_version;
    if (!d.in_clangd) {
        // The compile command of the backend for the virtual file (design 10.1).
        spp::ClangFilesOptions co;
        co.include_dirs = opt_.include_dirs;
        co.resource_dir = opt_.resource_dir;
        co.extra_flags = opt_.extra_flags;
        const std::vector<std::string> cmd = spp::compile_command(co, d.gen_path, d.gen_path + ".so");
        clangd_->notify("workspace/didChangeConfiguration",
                        {{"settings",
                          {{"compilationDatabaseChanges",
                            {{d.gen_path,
                              {{"workingDirectory", fs::path(d.gen_path).parent_path().string()},
                               {"compilationCommand", cmd}}}}}}}});
        clangd_->notify("textDocument/didOpen",
                        {{"textDocument", {{"uri", d.gen_uri}, {"languageId", "cpp"}, {"version", v}, {"text", cpp}}}});
        d.in_clangd = true;
    } else {
        clangd_->notify("textDocument/didChange", {{"textDocument", {{"uri", d.gen_uri}, {"version", v}}},
                                                   {"contentChanges", json::array({{{"text", cpp}}})}});
    }
}

void Server::update(Doc& d)
{
    sppc::AnalyzeResult facts = sppc::analyze(d.text.s, translate_options(d), session_);
    std::string cpp = facts.cpp;
    {
        std::lock_guard<std::mutex> lock(docs_mutex_);
        d.facts = std::move(facts);
        if (!cpp.empty()) {
            d.gen = Text(cpp);
            d.map = std::make_shared<const sppc::SourceMap>(d.facts.source_map);
            d.clang_diags = json::array();
            d.gen_version = d.sent_version + 1;
            send_generated(d, cpp);
        }
        send_client(publish_message(d));
        publish_imports(d);
    }
}

json Server::analyze_diagnostics(const Doc& d) const
{
    json out = json::array();
    auto add = [&](Pos p, const std::string& message, int severity) {
        const auto [b, e] = word_at(d, p);
        const Pos end = b <= p.col && e > p.col ? Pos{p.line, e} : Pos{p.line, p.col + 1};
        out.push_back({{"range", script_range(d, p, end)}, {"severity", severity}, {"source", "spp"},
                       {"message", message}});
    };
    for (const sppc::AnalyzeError& e : d.facts.errors) add({e.pos.line - 1, e.pos.col - 1}, e.message, 1);
    // The translation errors: "name:line:col: error: text", one line each.
    static const std::regex re(R"(^(.*):(\d+):(\d+): (error|warning): (.*)$)");
    std::istringstream ss(d.facts.diagnostics);
    for (std::string l; std::getline(ss, l);) {
        std::smatch m;
        if (!std::regex_match(l, m, re)) continue;
        if (m[1] != d.path) continue;  // an imported script: import_diagnostics() has it
        add({std::stoi(m[2]) - 1, std::stoi(m[3]) - 1}, m[5], m[4] == "error" ? 1 : 2);
    }
    return out;
}

// The path of a script that the translator names, as an absolute path.
static std::string script_file_path(const Doc& d, const std::string& name)
{
    fs::path f(name);
    if (f.is_relative()) {
        const fs::path near = fs::path(d.path).parent_path() / f;
        std::error_code ec;
        f = fs::exists(near, ec) ? near : fs::absolute(f);
    }
    return f.lexically_normal().string();
}

// The translator errors in the imported scripts, by the URI of the script. The line and the
// column are the real ones in that script.
std::map<std::string, json> Server::import_diagnostics(const Doc& d) const
{
    std::map<std::string, json> out;
    static const std::regex re(R"(^(.*):(\d+):(\d+): (error|warning): (.*)$)");
    std::istringstream ss(d.facts.diagnostics);
    std::map<std::string, Text> texts;
    for (std::string l; std::getline(ss, l);) {
        std::smatch m;
        if (!std::regex_match(l, m, re) || m[1] == d.path || m[1].length() == 0) continue;
        const std::string path = script_file_path(d, m[1]);
        const std::string uri = path_to_uri(path);
        if (uri == d.uri) continue;
        auto t = texts.find(path);
        if (t == texts.end()) t = texts.emplace(path, Text(read_file(path).value_or(std::string()))).first;
        const int line = std::stoi(m[2]) - 1, col = std::stoi(m[3]) - 1;
        const std::string_view text = t->second.line(line);
        int end = std::min<int>(col, static_cast<int>(text.size()));
        while (end < static_cast<int>(text.size()) && is_id_char(text[static_cast<size_t>(end)])) ++end;
        if (end <= col) end = col + 1;
        const json range = {{"start", to_lsp(t->second, {line, col}, utf8_)}, {"end", to_lsp(t->second, {line, end}, utf8_)}};
        out[uri].push_back({{"range", range}, {"severity", m[4] == "error" ? 1 : 2}, {"source", "spp"}, {"message", m[5].str()}});
    }
    return out;
}

// Publishes the translator errors of the imported scripts. A script that is open has its own
// diagnostics: it gets none here. A script that had errors and has none now gets an empty list.
void Server::publish_imports(Doc& d)
{
    std::map<std::string, json> now = import_diagnostics(d);
    for (auto it = now.begin(); it != now.end();) {
        if (docs_.count(it->first)) it = now.erase(it);
        else ++it;
    }
    auto send = [&](const std::string& uri, const json& diags) {
        send_client({{"jsonrpc", "2.0"},
                     {"method", "textDocument/publishDiagnostics"},
                     {"params", {{"uri", uri}, {"diagnostics", diags}}}});
    };
    for (const auto& [uri, diags] : now) send(uri, diags);
    for (const std::string& uri : d.import_uris)
        if (!now.count(uri)) send(uri, json::array());
    d.import_uris.clear();
    for (const auto& [uri, diags] : now) d.import_uris.insert(uri);
}

// A place in the generated text that comes from an imported script, as an LSP location in that
// script, or null. The range is the word at the place if "whole_word" is true.
json Server::imported_location(const Doc& d, const json& gen_lsp_pos, bool whole_word) const
{
    if (!d.map) return nullptr;
    const Pos g = from_lsp(d.gen, gen_lsp_pos, clangd_utf8_);
    const auto loc = d.map->locate(g.line + 1, g.col + 1);
    if (!loc || loc->file == 0 || loc->file > d.map->files().size()) return nullptr;
    const sppc::SourceFile& f = d.map->files()[loc->file - 1];
    const Text t(f.text);
    const Pos b{loc->pos.line - 1, loc->pos.col - 1};
    const std::string_view text = t.line(b.line);
    int end = std::min<int>(b.col, static_cast<int>(text.size()));
    if (whole_word)
        while (end < static_cast<int>(text.size()) && is_id_char(text[static_cast<size_t>(end)])) ++end;
    const std::string path = script_file_path(d, f.name);
    return {{"uri", path_to_uri(path)}, {"range", {{"start", to_lsp(t, b, utf8_)}, {"end", to_lsp(t, {b.line, std::max(end, b.col)}, utf8_)}}}};
}

json Server::publish_message(const Doc& d) const
{
    json diags = analyze_diagnostics(d);
    for (const json& x : d.clang_diags) diags.push_back(x);
    return {{"jsonrpc", "2.0"},
            {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", d.uri}, {"diagnostics", diags}}}};
}

void Server::on_clangd_notification(const json& msg)
{
    if (msg.value("method", std::string()) != "textDocument/publishDiagnostics") return;
    const json& p = msg["params"];
    std::lock_guard<std::mutex> lock(docs_mutex_);
    const auto it = gen_docs_.find(p.value("uri", std::string()));
    if (it == gen_docs_.end()) return;
    Doc& d = *it->second;
    // A diagnostic of a completion text (or of an old text) is not shown.
    if (p.contains("version") && p["version"].is_number_integer() && p["version"].get<int>() != d.gen_version) return;
    json out = json::array();
    for (const json& g : p.value("diagnostics", json::array())) {
        Pos b, e;
        const auto s = to_script(d, g["range"]["start"]);
        if (s) {
            b = *s;
            const auto se = to_script(d, g["range"]["end"]);
            e = se && se->line == b.line && b.col < se->col ? *se : Pos{b.line, word_at(d, b).second};
            if (e.col <= b.col) e.col = b.col + 1;
        } else {
            // No segment: the "#line" directives give the script line (column 0).
            const auto t = line_directive_target(d.gen.s, from_lsp(d.gen, g["range"]["start"], clangd_utf8_).line);
            if (!t || t->file != d.path) continue;
            b = {t->line, 0};
            e = {t->line, static_cast<int>(d.text.line(t->line).size())};
        }
        out.push_back({{"range", script_range(d, b, e)},
                       {"severity", g.value("severity", 1)},
                       {"source", "clangd"},
                       {"message", sppc::rewrite_clang_message(g.value("message", std::string()))}});
    }
    d.clang_diags = std::move(out);
    send_client(publish_message(d));
}

// ------------------------------------------------------------------ features

json Server::map_location(const Doc& d, const json& loc) const
{
    const std::string uri = loc.contains("targetUri") ? loc.value("targetUri", std::string()) : loc.value("uri", std::string());
    const json range = loc.contains("targetSelectionRange") ? loc["targetSelectionRange"] : loc.value("range", json());
    if (uri != d.gen_uri) return {{"uri", uri}, {"range", range}};  // a native place: no change
    const auto s = to_script(d, range["start"]);
    if (!s) return imported_location(d, range["start"], true);  // null when not in an import
    const auto [b, e] = word_at(d, *s);
    return {{"uri", d.uri}, {"range", script_range(d, {s->line, b}, {s->line, std::max(e, b)})}};
}

json Server::on_completion(const Doc& d, const json& p)
{
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    const std::string& t = d.text.s;
    const size_t off = d.text.offset(cur.line, cur.col);
    size_t s = off;
    while (s > 0 && is_id_char(t[s - 1])) --s;
    const bool after_dot = s > 0 && t[s - 1] == '.';
    const bool after_this = after_dot && s >= 5 && t.compare(s - 5, 5, "this.") == 0 && (s == 5 || !is_id_char(t[s - 6]));

    json items = json::array();
    std::set<std::string> names;
    // The script members of the class at the cursor (clangd does not see them on the shell).
    if (after_this) {
        const sppc::AnalyzedClass* cls = nullptr;
        for (const sppc::AnalyzedClass& c : d.facts.classes)
            if (Pos{c.begin.line - 1, c.begin.col - 1} < Pos{cur.line, cur.col + 1}) cls = &c;
        if (cls) {
            for (const sppc::AnalyzedMember& m : cls->members) {
                int kind = 0;
                if (m.kind == "field") kind = 5;
                else if (m.kind == "method") kind = 2;
                else if (m.kind == "property") kind = 10;
                if (!kind || !names.insert(m.name).second) continue;
                items.push_back({{"label", m.name}, {"kind", kind}, {"detail", "script " + m.kind + " of " + cls->name},
                                 {"insertText", m.name}, {"sortText", "0" + m.name}});
            }
        }
    }

    // The clangd part. The word at the cursor is replaced by a placeholder name, so the statement
    // translates and clangd completes at its generated place. The real text comes back after.
    if (clangd_ && clangd_->running()) {
        size_t n = off;
        while (n < t.size() && (t[n] == ' ' || t[n] == '\t' || t[n] == '\r' || t[n] == '\n')) ++n;
        const bool end = n < t.size() && std::string_view(";),]([+-*/%=<>!&|^?:.").find(t[n]) != std::string_view::npos;
        const std::string patched = t.substr(0, s) + kHole + (end ? "" : ";") + t.substr(off);
        sppc::AnalyzeResult facts = sppc::analyze(patched, translate_options(d), session_);
        const Pos hole = {cur.line, static_cast<int>(s - d.text.starts[static_cast<size_t>(cur.line)])};
        const auto g = facts.cpp.empty() ? std::nullopt : facts.source_map.map_to_generated(hole.line + 1, hole.col + 1);
        std::optional<json> r;
        if (g) {
            Doc& md = const_cast<Doc&>(d);
            const Text gen(facts.cpp);
            send_generated(md, facts.cpp);
            // Before its first preamble, clangd gives only "fallback" words of the file. A
            // request that needs the AST waits for the preamble.
            if (preamble_ready_.insert(d.gen_uri).second)
                clangd_result("textDocument/documentSymbol", {{"textDocument", {{"uri", d.gen_uri}}}});
            r = clangd_result("textDocument/completion",
                              {{"textDocument", {{"uri", d.gen_uri}}},
                               {"position", to_lsp(gen, {g->line - 1, g->col - 1}, clangd_utf8_)}});
            std::lock_guard<std::mutex> lock(docs_mutex_);
            md.gen_version = md.sent_version + 1;  // before the send: the diagnostics can come at once
            send_generated(md, d.gen.s);
        }
        const json list = r ? (r->is_array() ? *r : r->value("items", json::array())) : json::array();
        for (const json& it : list) {
            std::string name = it.value("filterText", std::string());
            if (name.empty()) name = it.value("insertText", it.value("label", std::string()));
            const int kind = it.value("kind", 1);
            // The names of the generated code and of the shell are not script names.
            if (name.empty() || name == kHole || name.find("__") != std::string::npos ||
                name.find("_Shell") != std::string::npos || name.compare(0, 8, "operator") == 0 || name[0] == '~' ||
                kind == 4 || name.find("::") != std::string::npos)
                continue;
            if (after_dot && (name == "cls" || name == "state")) continue;  // spp::ShellBase (runtime/shell.h)
            if (!after_dot && (kind != 6 || name == "self" || name == "st" || name == "a")) continue;
            if (!names.insert(name).second) continue;
            std::string label = it.value("label", name);
            while (!label.empty() && (label[0] == ' ')) label.erase(0, 1);
            if (label.compare(0, 3, "\xE2\x80\xA2") == 0) label.erase(0, 3);  // the include bullet
            json item = {{"label", sppc::rewrite_generated_names(label)}, {"kind", kind}, {"insertText", name},
                         {"filterText", name}, {"sortText", "1" + it.value("sortText", name)}};
            if (it.contains("detail")) item["detail"] = sppc::rewrite_generated_names(it.value("detail", std::string()));
            items.push_back(std::move(item));
        }
    }
    return {{"isIncomplete", false}, {"items", items}};
}

json Server::on_hover(const Doc& d, const json& p)
{
    const auto g = to_gen(d, from_lsp(d.text, p["position"], utf8_));
    if (!g) return nullptr;
    auto r = clangd_result("textDocument/hover", {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, *g)}});
    if (!r || !r->contains("contents")) return nullptr;
    json c = (*r)["contents"];
    if (c.is_object() && c.contains("value")) c["value"] = sppc::rewrite_generated_names(c.value("value", std::string()));
    else if (c.is_string()) c = sppc::rewrite_generated_names(c.get<std::string>());
    json out = {{"contents", c}};
    if (r->contains("range")) {
        const auto b = to_script(d, (*r)["range"]["start"]);
        if (b) {
            const auto [wb, we] = word_at(d, *b);
            out["range"] = script_range(d, {b->line, wb}, {b->line, we});
        }
    }
    return out;
}

json Server::on_locations(const Doc& d, const json& p, const std::string& method)
{
    json out = json::array();
    const Pos here = from_lsp(d.text, p["position"], utf8_);
    const bool is_ref = method == "textDocument/references";
    const std::string here_word = word(d, here);
    if (is_ref && !here_word.empty() && is_script_class(d, here_word)) {
        // A script class: the places come from the tokens (see class_places).
        auto add = [&](const Doc& doc, const std::set<Pos>& places) {
            for (const Pos& s : places)
                out.push_back({{"uri", doc.uri},
                               {"range", script_range(doc, s, {s.line, s.col + static_cast<int>(here_word.size())})}});
        };
        add(d, class_places(d, here_word));
        if (roots_.size()) for_other_scripts(d, {here_word, ""}, here_word, add);
        return out;
    }
    const auto g = to_gen(d, here);
    if (!g) return out;
    json params = {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, *g)}};
    if (method == "textDocument/references") params["context"] = {{"includeDeclaration", true}};
    auto r = clangd_result(method, params);
    if (!r) return out;
    json list = r->is_array() ? *r : json::array({*r});
    std::set<std::string> seen;
    for (const json& loc : list) {
        json m = map_location(d, loc);
        if (m.is_null() || !seen.insert(m.dump()).second) continue;
        out.push_back(std::move(m));
    }
    if (method == "textDocument/references" && roots_.size()) {
        const Pos cur = from_lsp(d.text, p["position"], utf8_);
        const std::string name = word(d, cur);
        if (const auto key = name.empty() ? std::nullopt : key_at(d, cur))
            for_other_scripts(d, *key, name, [&](const Doc& doc, const std::set<Pos>& places) {
                for (const Pos& s : places)
                    out.push_back({{"uri", doc.uri},
                                   {"range", script_range(doc, s, {s.line, s.col + static_cast<int>(name.size())})}});
            });
    }
    return out;
}

std::set<Pos> Server::class_places(const Doc& d, const std::string& name) const
{
    // The class name has no segment in the source map, so clangd cannot find it. The semantic
    // tokens of the facts mark each word that is the name of a script class.
    constexpr int kClassToken = 2;  // the index of "class" in semantic_token_types()
    std::set<Pos> out;
    for (const SemanticToken& t : script_tokens(d.text.s, d.facts))
        if (t.type == kClassToken && d.text.line(t.line).substr(static_cast<size_t>(t.col), static_cast<size_t>(t.len)) == name)
            out.insert({t.line, t.col});
    return out;
}

bool Server::is_script_class(const Doc& d, const std::string& name) const
{
    return std::any_of(d.facts.classes.begin(), d.facts.classes.end(),
                       [&](const sppc::AnalyzedClass& c) { return c.name == name; });
}

std::variant<std::set<Pos>, std::string> Server::symbol_places(const Doc& d, Pos cur, const std::string& old_name, Pos* decl)
{
    if (is_script_class(d, old_name)) {
        if (decl)
            for (const sppc::AnalyzedClass& c : d.facts.classes)
                if (c.name == old_name) *decl = class_name_pos(d, c);
        return class_places(d, old_name);
    }
    const auto g = to_gen(d, {cur.line, word_at(d, cur).first});
    if (!g || !clangd_) return std::string("no script symbol at the position");
    const json tdp = {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, *g)}};
    const auto defs = clangd_result("textDocument/definition", tdp);
    const json def_list = defs ? (defs->is_array() ? *defs : json::array({*defs})) : json::array();
    if (def_list.empty()) return std::string("no script symbol at the position");
    for (const json& loc : def_list) {
        const std::string uri = loc.contains("targetUri") ? loc.value("targetUri", std::string()) : loc.value("uri", std::string());
        // clangd can answer with the same symbol in the generated file of another script.
        if (uri != d.gen_uri && !gen_docs_.count(uri)) return std::string("cannot rename a native symbol: " + old_name);
        if (uri == d.gen_uri && map_location(d, loc).is_null()) return std::string("cannot rename a generated name: " + old_name);
    }
    if (decl) {
        *decl = {-1, -1};
        const std::string uri = def_list[0].contains("targetUri") ? def_list[0].value("targetUri", std::string())
                                                                   : def_list[0].value("uri", std::string());
        if (uri == d.gen_uri) {
            const json m = map_location(d, def_list[0]);
            *decl = from_lsp(d.text, m["range"]["start"], utf8_);
        }
    }
    json params = tdp;
    params["context"] = {{"includeDeclaration", true}};
    const auto refs = clangd_result("textDocument/references", params);
    std::set<Pos> starts;
    for (const json& loc : refs ? *refs : json::array()) {
        if (loc.value("uri", std::string()) != d.gen_uri) continue;
        const auto s = to_script(d, loc["range"]["start"]);
        if (!s) continue;
        // The mapped start can be the start of the expression ("this" of "this.heal"). Take the
        // first whole word "old_name" on the line from there.
        const std::string_view l = d.text.line(s->line);
        for (size_t at = l.find(old_name, static_cast<size_t>(s->col)); at != std::string_view::npos;
             at = l.find(old_name, at + 1)) {
            const bool left = at == 0 || !is_id_char(l[at - 1]);
            const bool right = at + old_name.size() >= l.size() || !is_id_char(l[at + old_name.size()]);
            if (left && right) {
                starts.insert({s->line, static_cast<int>(at)});
                break;
            }
        }
    }
    return starts;
}

std::optional<SymbolKey> Server::symbol_key(const Doc& d, Pos decl, const std::string& name) const
{
    if (decl.line < 0) {
        // The declaration is in another script. Take the member of this script, if it is the only one.
        std::optional<SymbolKey> found;
        for (const sppc::AnalyzedClass& c : d.facts.classes)
            for (const sppc::AnalyzedMember& m : c.members)
                if (m.name == name) {
                    if (found) return std::nullopt;
                    found = SymbolKey{c.name, name};
                }
        return found;
    }
    for (const sppc::AnalyzedClass& c : d.facts.classes) {
        if (c.name == name && c.begin.line - 1 == decl.line) return SymbolKey{name, ""};
        for (const sppc::AnalyzedMember& m : c.members)
            if (m.name == name && m.begin.line - 1 == decl.line) return SymbolKey{c.name, name};
    }
    return std::nullopt;
}

std::optional<SymbolKey> Server::key_at(const Doc& d, Pos cur)
{
    const std::string name = word(d, cur);
    if (name.empty()) return std::nullopt;
    Pos decl;
    const auto r = symbol_places(d, cur, name, &decl);
    if (std::holds_alternative<std::string>(r)) return std::nullopt;
    return symbol_key(d, decl, name);
}

Pos Server::class_name_pos(const Doc& d, const sppc::AnalyzedClass& c) const
{
    // The begin of a class is at the keyword "class". The name comes after it on the line.
    const Pos b = {c.begin.line - 1, c.begin.col - 1};
    const std::string_view l = d.text.line(b.line);
    for (size_t at = l.find(c.name, static_cast<size_t>(b.col)); at != std::string_view::npos; at = l.find(c.name, at + 1))
        if ((at == 0 || !is_id_char(l[at - 1])) && (at + c.name.size() >= l.size() || !is_id_char(l[at + c.name.size()])))
            return {b.line, static_cast<int>(at)};
    return b;
}

std::optional<Pos> Server::key_position(const Doc& d, const SymbolKey& key) const
{
    for (const sppc::AnalyzedClass& c : d.facts.classes) {
        if (c.name != key.cls) continue;
        if (key.member.empty()) return class_name_pos(d, c);
        for (const sppc::AnalyzedMember& m : c.members)
            if (m.name == key.member) return Pos{m.begin.line - 1, m.begin.col - 1};
    }
    return std::nullopt;
}

std::vector<std::string> Server::workspace_scripts() const
{
    std::vector<std::string> out;
    for (const std::string& root : roots_) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if ((!name.empty() && name[0] == '.') || name == "node_modules") it.disable_recursion_pending();
                continue;
            }
            if (it->path().extension() == ".spp" && it->is_regular_file(ec2)) out.push_back(it->path().lexically_normal().string());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Server::for_other_scripts(const Doc& origin, const SymbolKey& key, const std::string& name,
                               const std::function<void(const Doc&, const std::set<Pos>&)>& fn)
{
    index_.prune();  // the shard of a file that is gone
    for (const std::string& path : workspace_scripts()) {
        if (path == origin.path) continue;
        std::shared_ptr<Doc> open;
        for (const auto& [uri, doc] : docs_)
            if (doc->path == path) open = doc;
        if (open) {
            const auto at = key_position(*open, key);
            if (!at || word(*open, *at) != name) continue;
            const auto r = symbol_places(*open, *at, name, nullptr);
            if (const auto* places = std::get_if<std::set<Pos>>(&r); places && !places->empty()) fn(*open, *places);
            continue;
        }
        const auto text = read_file(path);
        if (!text) continue;
        Doc tmp;
        tmp.uri = path_to_uri(path);
        tmp.path = path;
        tmp.gen_path = path + ".cpp";
        tmp.gen_uri = path_to_uri(tmp.gen_path);
        tmp.text = Text(*text);
        bool analyzed = false;
        auto analyze_tmp = [&] {
            tmp.facts = sppc::analyze(tmp.text.s, translate_options(tmp), session_);
            analyzed = true;
            ++closed_analyses_;
        };
        // The saved shard of the file holds the symbols and the references of the last analyze.
        const std::string digest = SymbolIndex::digest(*text);
        auto shard = index_.load(path, digest);
        if (!shard) {
            if (text->find(name) == std::string::npos) continue;
            analyze_tmp();
            shard = shard_of_doc(tmp);
            index_.store(path, digest, *shard);
        }
        IndexClass* cls = shard->find_class(key.cls);
        if (!cls) continue;
        std::set<Pos> places;
        if (key.member.empty()) {
            for (const IndexPos& r : cls->refs) places.insert({r.line, r.col});
        } else {
            IndexMember* m = cls->find_member(key.member);
            if (!m) continue;
            if (m->refs) {
                for (const IndexPos& r : *m->refs) places.insert({r.line, r.col});
            } else {  // a member needs clangd. The result goes into the shard.
                if (!analyzed) analyze_tmp();
                const auto at = key_position(tmp, key);
                if (!at || tmp.facts.cpp.empty()) continue;
                tmp.gen = Text(tmp.facts.cpp);
                tmp.map = std::make_shared<const sppc::SourceMap>(tmp.facts.source_map);
                tmp.gen_version = 1;
                send_generated(tmp, tmp.facts.cpp);
                const auto r = symbol_places(tmp, *at, name, nullptr);
                if (const auto* found = std::get_if<std::set<Pos>>(&r)) places = *found;
                if (clangd_ && tmp.in_clangd) clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", tmp.gen_uri}}}});
                if (!places.empty()) {
                    m->refs = std::vector<IndexPos>();
                    for (const Pos& q : places) m->refs->push_back({q.line, q.col});
                    index_.store(path, digest, *shard);
                }
            }
        }
        if (!places.empty()) fn(tmp, places);
    }
}

IndexShard Server::shard_of_doc(const Doc& d) const
{
    IndexShard shard;
    for (const sppc::AnalyzedClass& c : d.facts.classes) {
        IndexClass ic;
        ic.name = c.name;
        const Pos b = class_name_pos(d, c);
        ic.begin = {b.line, b.col};
        for (const Pos& q : class_places(d, c.name)) ic.refs.push_back({q.line, q.col});
        for (const sppc::AnalyzedMember& m : c.members) ic.members.push_back({m.name, m.kind, {m.begin.line - 1, m.begin.col - 1}, std::nullopt});
        shard.classes.push_back(std::move(ic));
    }
    return shard;
}

std::variant<json, std::string> Server::on_rename(const Doc& d, const json& p)
{
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    const std::string old_name = word(d, cur);
    const std::string new_name = p.value("newName", std::string());
    if (old_name.empty()) return std::string("no symbol at the position");
    if (new_name.empty() || !std::all_of(new_name.begin(), new_name.end(), is_id_char) ||
        (new_name[0] >= '0' && new_name[0] <= '9'))
        return std::string("the new name is not an identifier: " + new_name);
    Pos decl;
    const auto r = symbol_places(d, cur, old_name, &decl);
    if (const auto* e = std::get_if<std::string>(&r)) return *e;
    auto edits_of = [&](const Doc& doc, const std::set<Pos>& places) {
        json edits = json::array();
        for (const Pos& s : places)
            edits.push_back({{"range", script_range(doc, s, {s.line, s.col + static_cast<int>(old_name.size())})},
                             {"newText", new_name}});
        return edits;
    };
    json changes = {{d.uri, edits_of(d, std::get<std::set<Pos>>(r))}};
    // A class or a member that other scripts of the workspace declare too (the versions of a
    // script) gets the new name there as well.
    if (const auto key = symbol_key(d, decl, old_name))
        for_other_scripts(d, *key, old_name, [&](const Doc& doc, const std::set<Pos>& places) {
            changes[doc.uri] = edits_of(doc, places);
        });
    return json{{"changes", changes}};
}

json Server::on_document_symbol(const Doc& d)
{
    json out = json::array();
    for (const sppc::AnalyzedClass& c : d.facts.classes) {
        const Pos b = {c.begin.line - 1, c.begin.col - 1};
        Pos e = {b.line, b.col + static_cast<int>(c.name.size())};
        json children = json::array();
        for (const sppc::AnalyzedMember& m : c.members) {
            int kind = 6;  // Method
            if (m.kind == "field") kind = 8;
            else if (m.kind == "constructor") kind = 9;
            else if (m.kind == "property") kind = 7;
            const Pos mb = {m.begin.line - 1, m.begin.col - 1}, me = {m.end.line - 1, m.end.col - 1};
            if (e < me) e = me;
            const std::string shown = m.kind == "destructor" && m.name[0] != '~' ? "~" + m.name : m.name;
            children.push_back({{"name", shown}, {"kind", kind}, {"detail", m.kind},
                                {"range", script_range(d, mb, me)},
                                {"selectionRange", script_range(d, mb, {mb.line, mb.col + static_cast<int>(m.name.size())})}});
        }
        out.push_back({{"name", c.name}, {"kind", 5}, {"detail", c.base.empty() ? "class" : "class : " + c.base},
                       {"range", script_range(d, b, e)},
                       {"selectionRange", script_range(d, b, {b.line, b.col + static_cast<int>(c.name.size())})},
                       {"children", children}});
    }
    return out;
}

json Server::on_semantic_tokens(const Doc& d)
{
    json data = json::array();
    int pl = 0, pc = 0;
    for (const SemanticToken& t : script_tokens(d.text.s, d.facts)) {
        const std::string_view l = d.text.line(t.line);
        const int c = utf8_ ? t.col : byte_to_utf16(l, t.col);
        const int len = utf8_ ? t.len : byte_to_utf16(l, t.col + t.len) - c;
        data.push_back(t.line - pl);
        data.push_back(t.line == pl ? c - pc : c);
        data.push_back(len);
        data.push_back(t.type);
        data.push_back(0);
        pl = t.line;
        pc = c;
    }
    return {{"data", data}};
}

// A place in a generated file on disk (from "sppc --compile-commands") as a script location.
// The "#line" directives give the script and the line.
json disk_to_script(const std::string& cpp_path, int line)
{
    const auto text = read_file(cpp_path);
    if (!text) return nullptr;
    const auto t = line_directive_target(*text, line);
    if (!t) return nullptr;
    fs::path f = t->file;
    if (f.is_relative()) {
        const fs::path near = fs::path(cpp_path).parent_path() / f;
        f = fs::exists(near) ? near : fs::absolute(f);
    }
    const json p = {{"line", t->line}, {"character", 0}};
    return {{"uri", path_to_uri(f.lexically_normal().string())}, {"range", {{"start", p}, {"end", p}}}};
}

json Server::script_member_location(const std::string& uri, const std::string& script_name)
{
    // The script name is "Class.member". A helper of the translator (a thunk, a slot function,
    // a field init) has a name that is not a member of the class: it gets no location.
    const size_t dot = script_name.find('.');
    if (uri.empty() || dot == std::string::npos) return nullptr;
    const std::string cls = script_name.substr(0, dot), member = script_name.substr(dot + 1);
    const sppc::AnalyzeResult* facts = nullptr;
    Text text;
    if (const auto it = docs_.find(uri); it != docs_.end()) {
        facts = &it->second->facts;
        text = it->second->text;
    } else {
        const std::string path = uri_to_path(uri);
        const auto t = read_file(path);
        if (!t) return nullptr;
        auto& cached = disk_facts_[path];
        if (cached.first != *t) {
            Doc tmp;
            tmp.path = path;
            cached = {*t, sppc::analyze(*t, translate_options(tmp), session_)};
        }
        facts = &cached.second;
        text = Text(*t);
    }
    for (const sppc::AnalyzedClass& c : facts->classes) {
        if (c.name != cls) continue;
        for (const sppc::AnalyzedMember& m : c.members) {
            if (m.name != member) continue;
            const Pos b = {m.begin.line - 1, m.begin.col - 1};
            const Pos e = {b.line, b.col + static_cast<int>(member.size())};
            return {{"uri", uri}, {"range", {{"start", to_lsp(text, b, utf8_)}, {"end", to_lsp(text, e, utf8_)}}}};
        }
    }
    return nullptr;
}

json Server::on_workspace_symbol(const json& p)
{
    json out = json::array();
    auto r = clangd_result("workspace/symbol", {{"query", p.value("query", std::string())}});
    if (!r || !r->is_array()) return out;
    for (const json& s : *r) {
        json loc = s.value("location", json());
        const std::string uri = loc.value("uri", std::string());
        const std::string name = s.value("name", std::string());
        const std::string container = s.value("containerName", std::string());
        const std::string script_name = sppc::rewrite_generated_names(name);
        const std::string script_container = sppc::rewrite_generated_names(container);
        // A symbol of a generated file is a script symbol when its name, or the name of its
        // container (a state struct), is a generated name. Else it is a part of the plumbing.
        const bool script = script_name != name || (script_container != container && name.compare(0, 5, "SLOT_") != 0);
        json mapped;
        bool generated = false;
        if (const auto it = gen_docs_.find(uri); it != gen_docs_.end()) {
            generated = true;
            if (script) mapped = map_location(*it->second, loc);
        } else if (uri.size() > 4 && uri.compare(uri.size() - 4, 4, ".cpp") == 0) {
            // A generated file of the workspace from the background index.
            const std::string path = uri_to_path(uri);
            const auto text = read_file(path);
            generated = text && text->find("// Generated by sppc") == 0;
            if (generated && script) mapped = disk_to_script(path, loc.value("/range/start/line"_json_pointer, 0));
        }
        if (generated && !mapped.is_null()) {
            // A field of a state struct: the container gives the class.
            const bool field = script_name.find('.') == std::string::npos && script_container != container;
            mapped = script_member_location(mapped.value("uri", std::string()),
                                            field ? script_container + "." + script_name : script_name);
        }
        if (generated && mapped.is_null()) continue;
        if (mapped.is_null()) mapped = loc;  // a native symbol
        json item = s;
        item["name"] = script_name;
        if (s.contains("containerName")) item["containerName"] = script_container;
        item["location"] = mapped;
        out.push_back(std::move(item));
    }
    return out;
}

json Server::on_to_script(const json& p)
{
    const std::string uri = p.value("/textDocument/uri"_json_pointer, std::string());
    const json pos = p.value("position", json::object());
    if (const auto it = gen_docs_.find(uri); it != gen_docs_.end()) {
        const Doc& d = *it->second;
        if (const auto s = to_script(d, pos)) return {{"uri", d.uri}, {"range", script_range(d, *s, *s)}};
        if (json imp = imported_location(d, pos, false); !imp.is_null()) return imp;
        const auto t = line_directive_target(d.gen.s, pos.value("line", 0));
        if (!t || t->file != d.path) return nullptr;
        return {{"uri", d.uri}, {"range", script_range(d, {t->line, 0}, {t->line, 0})}};
    }
    return disk_to_script(uri_to_path(uri), pos.value("line", 0));
}

// ------------------------------------------------------------------ dispatch

void Server::handle(const json& msg)
{
    const std::string method = msg.value("method", std::string());
    const bool is_request = msg.contains("id");
    const json id = is_request ? msg["id"] : json();
    const json p = msg.value("params", json::object());
    if (method.empty()) return;  // a response to a request that the server did not send

    if (method == "initialize") return reply(id, on_initialize(p));
    if (method == "shutdown") {
        shutdown_ = true;
        if (clangd_) clangd_->stop();
        return reply(id, nullptr);
    }
    if (method == "textDocument/didOpen") {
        const json& td = p["textDocument"];
        auto d = std::make_shared<Doc>();
        d->uri = td.value("uri", std::string());
        d->path = uri_to_path(d->uri);
        d->gen_path = d->path + ".cpp";
        d->gen_uri = path_to_uri(d->gen_path);
        d->text = Text(td.value("text", std::string()));
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            docs_[d->uri] = d;
            gen_docs_[d->gen_uri] = d;
        }
        return update(*d);
    }
    if (method == "textDocument/didChange") {
        auto d = doc_of(p);
        if (!d) return;
        const json changes = p.value("contentChanges", json::array());
        if (changes.empty()) return;
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            d->text = Text(changes.back().value("text", std::string()));
        }
        return update(*d);
    }
    if (method == "textDocument/didClose") {
        auto d = doc_of(p);
        if (!d) return;
        if (clangd_ && d->in_clangd) clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", d->gen_uri}}}});
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            docs_.erase(d->uri);
            gen_docs_.erase(d->gen_uri);
        }
        send_client({{"jsonrpc", "2.0"},
                     {"method", "textDocument/publishDiagnostics"},
                     {"params", {{"uri", d->uri}, {"diagnostics", json::array()}}}});
        for (const std::string& uri : d->import_uris)
            send_client({{"jsonrpc", "2.0"},
                         {"method", "textDocument/publishDiagnostics"},
                         {"params", {{"uri", uri}, {"diagnostics", json::array()}}}});
        return;
    }
    if (!is_request) return;  // initialized, $/cancelRequest and other notifications
    if (method == "workspace/symbol") return reply(id, on_workspace_symbol(p));
    if (method == "spp/indexStats")
        return reply(id, {{"analyzed", closed_analyses_}, {"loaded", index_.loads()}, {"stored", index_.stores()}, {"dir", index_.dir()}});
    if (method == "spp/toScript") return reply(id, on_to_script(p));

    static const std::set<std::string> doc_methods = {
        "textDocument/completion", "textDocument/hover", "textDocument/definition", "textDocument/references",
        "textDocument/documentSymbol", "textDocument/semanticTokens/full", "textDocument/rename"};
    if (!doc_methods.count(method)) return reply_error(id, kMethodNotFound, "unknown method: " + method);
    auto d = doc_of(p);
    if (!d) return reply_error(id, kInvalidParams, "the document is not open");
    if (method == "textDocument/completion") return reply(id, on_completion(*d, p));
    if (method == "textDocument/hover") return reply(id, on_hover(*d, p));
    if (method == "textDocument/definition" || method == "textDocument/references")
        return reply(id, on_locations(*d, p, method));
    if (method == "textDocument/documentSymbol") return reply(id, on_document_symbol(*d));
    if (method == "textDocument/semanticTokens/full") return reply(id, on_semantic_tokens(*d));
    auto r = on_rename(*d, p);
    if (auto* e = std::get_if<std::string>(&r)) return reply_error(id, kRequestFailed, *e);
    return reply(id, std::get<json>(r));
}

int Server::run()
{
    MessageReader reader(in_);
    std::string body;
    while (reader.read(body)) {
        const json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) continue;
        if (msg.value("method", std::string()) == "exit") break;
        try {
            handle(msg);
        } catch (const std::exception& e) {
            if (msg.contains("id")) reply_error(msg["id"], kRequestFailed, std::string("internal error: ") + e.what());
        }
    }
    if (clangd_) clangd_->stop();
    return shutdown_ ? 0 : 1;
}

}  // namespace

std::optional<ScriptLine> line_directive_target(const std::string& cpp, int line)
{
    static const std::regex re(R"(^\s*#\s*line\s+(\d+)\s+"((?:[^"\\]|\\.)*)\")");
    const std::vector<size_t> starts = line_starts(cpp);
    // Walk back to the nearest directive. A directive that names the .cpp file itself marks
    // translator text: the last script directive before it gives the line then.
    bool in_cpp_text = false;
    for (int l = std::min(line, static_cast<int>(starts.size()) - 1); l >= 0; --l) {
        const std::string_view t = line_text(cpp, starts, l);
        if (t.find("#") == std::string_view::npos || t.find("line") == std::string_view::npos) continue;
        std::cmatch m;
        if (!std::regex_search(t.data(), t.data() + t.size(), m, re)) continue;
        std::string file;
        for (size_t i = 0; i < static_cast<size_t>(m[2].length()); ++i) {
            const char c = m[2].first[i];
            if (c == '\\' && i + 1 < static_cast<size_t>(m[2].length())) ++i;
            file += m[2].first[i];
        }
        const bool is_cpp = file.size() > 4 && file.compare(file.size() - 4, 4, ".cpp") == 0;
        if (is_cpp) {
            in_cpp_text = true;
            continue;
        }
        const int n = std::stoi(m[1].str());
        return ScriptLine{file, in_cpp_text ? n - 1 : n - 1 + (line - l - 1)};
    }
    return std::nullopt;
}

int run_server(int in_fd, int out_fd, const ServerOptions& options)
{
    signal(SIGPIPE, SIG_IGN);  // a write to a clangd that stopped gives an error, not a signal
    Server server(in_fd, out_fd, options);
    return server.run();
}

const char* usage_text()
{
    return "usage: spp-ls [--stdio] [--clangd path] [-I dir]... [--resource-dir dir] [--cxxflag flag]...\n"
           "              [--compile-commands-dir dir] [--index-dir dir]\n"
           "              [--clangd-log file]\n"
           "  --clangd path      the clangd binary (default: clangd from PATH). \"\" turns clangd off.\n"
           "  -I dir             an include dir for the script imports and for clangd.\n"
           "  --compile-commands-dir dir\n"
           "                     the directory of compile_commands.json from sppc --compile-commands.\n"
           "                     clangd runs its background index on it.\n"
           "  --index-dir dir    the directory of the saved symbol index (default:\n"
           "                     .cache/spp/index in the first workspace folder).\n";
}

bool parse_arguments(const std::vector<std::string>& args, ServerOptions& out, std::string& error)
{
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= args.size()) {
                error = "missing value after " + a;
                return false;
            }
            v = args[++i];
            return true;
        };
        std::string v;
        if (a == "--stdio") continue;
        if (a == "--clangd") {
            if (!next(out.clangd)) return false;
        } else if (a == "-I") {
            if (!next(v)) return false;
            out.include_dirs.push_back(v);
        } else if (a.compare(0, 2, "-I") == 0) {
            out.include_dirs.push_back(a.substr(2));
        } else if (a == "--resource-dir") {
            if (!next(out.resource_dir)) return false;
        } else if (a == "--cxxflag") {
            if (!next(v)) return false;
            out.extra_flags.push_back(v);
        } else if (a == "--compile-commands-dir") {
            if (!next(out.compile_commands_dir)) return false;
        } else if (a == "--index-dir") {
            if (!next(out.index_dir)) return false;
        } else if (a == "--clangd-log") {
            if (!next(out.clangd_log)) return false;
        } else {
            error = "unknown argument: " + a;
            return false;
        }
    }
    return true;
}

}  // namespace spp::ls
