// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "server.h"

#include "clangd_process.h"
#include "hosts.h"
#include "jsonrpc.h"
#include "long_path.h"
#include "semantic.h"
#include "hot/clang_files_backend.h"
#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "symbol_index.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <signal.h>
#include <sstream>
#include <variant>
#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

#include "hot/runtime/process.h"

namespace fs = std::filesystem;

namespace hot::ls
{

using json = nlohmann::json;

namespace
{

// The name that the completion puts in place of the word at the cursor (see on_completion).
const std::string kHole = "__hot_complete";

// LSP error codes.
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kRequestFailed = -32803;

int hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string uri_to_path(const std::string& uri)
{
    std::string s = uri.compare(0, 7, "file://") == 0 ? uri.substr(7) : uri;
    std::string r;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0)
        {
            r += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        }
        else
        {
            r += s[i];
        }
    }
#ifdef _WIN32
    // "file:///C:/dir/a.hot" has the path "/C:/dir/a.hot". The Windows path is "C:\dir\a.hot".
    if (r.size() > 2 && r[0] == '/' && r[2] == ':')
        r.erase(0, 1);
    std::replace(r.begin(), r.end(), '/', '\\');
    r = long_path(r); // clangd writes the long names, not the short 8.3 names
#endif
    return r;
}

// hot-ls serves a ".hot" script and a ".cpp" script. Any other file (a header, for example) belongs
// to the C++ tools: hot-ls does not analyze it and does not publish for it.
bool is_served_path(const std::string& path)
{
    const std::string ext = fs::path(path).extension().string();
    return ext == ".hot" || ext == ".cpp";
}

std::string path_to_uri(const std::string& path)
{
    static const char* digits = "0123456789ABCDEF";
    std::string r = "file://";
#ifdef _WIN32
    // The URI of "C:\dir\a.hot" is "file:///C:/dir/a.hot", the form that clangd writes.
    std::string p = long_path(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.size() > 1 && p[1] == ':')
        p.insert(0, 1, '/');
    for (char ch : p)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (is_id_char(ch) || c == '/' || c == '-' || c == '.' || c == '~' || c == ':')
#else
    for (char ch : path)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (is_id_char(ch) || c == '/' || c == '-' || c == '.' || c == '~')
#endif
        {
            r += ch;
        }
        else
        {
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
    if (!in)
        return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A text with its line table. Positions are 0-based lines and byte columns.
struct Text
{
    std::string s;
    std::vector<size_t> starts;
    Text()
        : starts{0}
    {
    }
    explicit Text(std::string t)
        : s(std::move(t))
        , starts(line_starts(s))
    {
    }
    std::string_view line(int l) const { return line_text(s, starts, l); }
    size_t offset(int l, int c) const
    {
        if (l < 0)
            return 0;
        if (static_cast<size_t>(l) >= starts.size())
            return s.size();
        return std::min(starts[static_cast<size_t>(l)] + static_cast<size_t>(std::max(c, 0)), s.size());
    }
    std::pair<int, int> pos(size_t off) const
    {
        const auto it = std::upper_bound(starts.begin(), starts.end(), off);
        const int l = static_cast<int>(it - starts.begin()) - 1;
        return {l, static_cast<int>(off - starts[static_cast<size_t>(l)])};
    }
};

struct Pos
{
    int line = 0, col = 0; // 0-based line, byte column
    bool operator<(const Pos& o) const { return std::tie(line, col) < std::tie(o.line, o.col); }
    bool operator==(const Pos& o) const { return line == o.line && col == o.col; }
};

// A script symbol that other scripts can have too: a class, or a member of a class.
struct SymbolKey
{
    std::string cls, member; // member is empty for a class
};

// One open .hot document and its state in clangd. A ".cpp" document is also a Doc, but with "cpp"
// set: it has no state in clangd and it gets the errors of hotc only (update_cpp).
struct Doc
{
    std::string uri, path;
    // A ".cpp" document. It is a script when it has a host. The server publishes the hotc errors of
    // a script. It ignores any other ".cpp" document. All the other requests get no answer.
    bool cpp = false;
    bool cpp_published = false; // the last publish of a ".cpp" document had a diagnostic list
    // The "no host matches" message went to the user for this document. It resets when the host
    // set of the document changes. It lives in the document, so a close of the document drops it.
    mutable bool no_host_told = false;
    Text text;
    hotc::AnalyzeResult facts;
    // The generated C++ that clangd has as the real text, and its map. The map belongs to the
    // script text "gen_script". After a translation error, the last good state stays. The
    // positions of the script then go through the edit between "gen_script" and "text".
    std::string gen_path, gen_uri;
    Text gen;
    std::shared_ptr<const hotc::SourceMap> map;
    Text gen_script;
    bool edited = false; // the text differs from gen_script
    // The edit from gen_script to text: the bytes [0, edit_prefix) are equal. The old bytes from
    // edit_old_end and the new bytes from edit_new_end are equal to the end.
    size_t edit_prefix = 0, edit_old_end = 0, edit_new_end = 0;
    void set_edit()
    {
        const std::string &a = gen_script.s, &b = text.s;
        edited = a != b;
        size_t p = 0;
        const size_t n = std::min(a.size(), b.size());
        while (p < n && a[p] == b[p])
            ++p;
        size_t q = 0;
        while (q < n - p && a[a.size() - 1 - q] == b[b.size() - 1 - q])
            ++q;
        edit_prefix = p;
        edit_old_end = a.size() - q;
        edit_new_end = b.size() - q;
    }
    // A position of the text as a position of gen_script, and back. No value inside the edit.
    std::optional<Pos> text_to_gen_script(Pos p) const;
    std::optional<Pos> gen_script_to_text(Pos p) const;
    int gen_version = 0; // the clangd version of the real text
    int sent_version = 0; // the last clangd version
    bool in_clangd = false;
    // The host of the script at the last update. Null: no host.
    std::shared_ptr<const Host> host;
    json clang_diags = json::array(); // the mapped clangd diagnostics of gen_version
    // The last diagnostics of clangd as clangd sent them. clangd does not send them again when a
    // new version has the same diagnostics. So update() maps them again to the new text.
    json raw_clang_diags = json::array();
    // The clangd diagnostics of gen_version that come from imported scripts, by the URI of the script.
    std::map<std::string, json> clang_import_diags;
    // The URIs of the imported scripts that got translator errors from this document.
    std::set<std::string> import_uris;
    // The last clangd answer of a completion. "rest" is the script text without the word at the
    // cursor. The answer holds the names that start with "prefix".
    struct CompletionCache
    {
        std::string rest, prefix;
        bool valid = false, complete = false;
        json list = json::array();
    };
    mutable CompletionCache completion;
};

std::optional<Pos> Doc::text_to_gen_script(Pos p) const
{
    if (!edited)
        return p;
    const size_t off = text.offset(p.line, p.col);
    size_t old;
    if (off < edit_prefix)
        old = off;
    else if (off >= edit_new_end)
        old = off - edit_new_end + edit_old_end;
    else
        return std::nullopt;
    const auto [l, c] = gen_script.pos(old);
    return Pos{l, c};
}

std::optional<Pos> Doc::gen_script_to_text(Pos p) const
{
    if (!edited)
        return p;
    const size_t old = gen_script.offset(p.line, p.col);
    size_t off;
    if (old < edit_prefix)
        off = old;
    else if (old >= edit_old_end)
        off = old - edit_old_end + edit_new_end;
    else
        return std::nullopt;
    const auto [l, c] = text.pos(off);
    return Pos{l, c};
}

class Server
{
public:
    Server(int in, int out, ServerOptions o)
        : in_(in)
        , out_(out)
        , opt_(std::move(o))
    {
        read_compile_commands();
    }
    ~Server()
    {
        if (clangd_)
            clangd_->stop();
    }
    int run();

private:
    // ---- the connection to the client
    void send_client(const json& msg)
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        write_message(out_, msg.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    void reply(const json& id, json result)
    {
        send_client({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    }
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
        if (!d.map)
            return std::nullopt;
        const auto old = d.text_to_gen_script(p);
        if (!old)
            return std::nullopt;
        const auto g = d.map->map_to_generated(old->line + 1, old->col + 1);
        if (!g)
            return std::nullopt;
        return Pos{g->line - 1, g->col - 1};
    }
    std::optional<Pos> to_script(const Doc& d, const json& lsp_pos) const
    {
        if (!d.map)
            return std::nullopt;
        const Pos g = from_lsp(d.gen, lsp_pos, clangd_utf8_);
        const auto s = d.map->map_to_script(g.line + 1, g.col + 1);
        if (!s)
            return std::nullopt;
        return d.gen_script_to_text(Pos{s->line - 1, s->col - 1});
    }
    // The word at a script position, or the word that ends there. Gives [begin, end) columns.
    std::pair<int, int> word_at(const Doc& d, Pos p) const
    {
        const std::string_view l = d.text.line(p.line);
        int b = std::min<int>(p.col, static_cast<int>(l.size()));
        if ((b >= static_cast<int>(l.size()) || !is_id_char(l[static_cast<size_t>(b)])) && b > 0
            && is_id_char(l[static_cast<size_t>(b - 1)]))
            --b;
        int e = b;
        while (b > 0 && is_id_char(l[static_cast<size_t>(b - 1)]))
            --b;
        while (e < static_cast<int>(l.size()) && is_id_char(l[static_cast<size_t>(e)]))
            ++e;
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
    hotc::TranslateOptions translate_options(const Doc& d) const
    {
        hotc::TranslateOptions o;
        o.script_name = d.path;
        o.generation = 1;
        const Effective e = effective_options(d.path);
        o.include_dirs = e.include_dirs;
        o.resource_dir = e.resource_dir;
        o.std_version = e.std_version;
        o.format_lib = hot::format_lib_name();
        return o;
    }
    // ---- hosts
    struct Effective
    {
        std::vector<std::string> include_dirs, extra_flags;
        std::string resource_dir;
        std::string std_version; // the "std" of the host file (empty: the standard of the build)
    };
    std::shared_ptr<const Host> host_of(const std::string& path) const
    {
        for (const HostSet& h : hosts_)
            if (auto m = h.match(path))
                return m;
        return nullptr;
    }
    // The host that is nearest to a script that no host matches, or null. It must share a
    // directory with the script that is a workspace root or below it. Without a workspace root, it
    // must share two path parts at least.
    std::shared_ptr<const Host> nearest_host(const std::string& path) const
    {
        std::shared_ptr<const Host> best;
        size_t best_parts = 0;
        for (const HostSet& h : hosts_)
        {
            fs::path common;
            const auto m = h.nearest(path, common);
            const size_t parts = static_cast<size_t>(std::distance(common.begin(), common.end()));
            bool in_root = roots_.empty() ? parts >= 2 : false;
            for (const std::string& r : roots_)
            {
                const fs::path root = fs::path(r).lexically_normal();
                const std::string rel = common.lexically_relative(root).string();
                if (!common.empty() && !rel.empty() && rel.compare(0, 2, "..") != 0)
                    in_root = true;
            }
            if (m && in_root && parts > best_parts)
            {
                best_parts = parts;
                best = m;
            }
        }
        return best;
    }
    // The options for one script: the host first, then the options of the command line. A script
    // that no host matches uses the nearest host, so that its "using" headers are found. When
    // the command line has no include dir and no host is near, the default include dirs of the
    // build are used (the runtime headers).
    Effective effective_options(const std::string& path) const
    {
        Effective e;
        auto host = host_of(path);
        if (!host)
            host = nearest_host(path);
        if (host)
        {
            e.include_dirs = host->include_dirs;
            e.extra_flags = host->cxx_flags();
            e.std_version = host->std_version;
        }
        e.include_dirs.insert(e.include_dirs.end(), opt_.include_dirs.begin(), opt_.include_dirs.end());
        e.extra_flags.insert(e.extra_flags.end(), opt_.extra_flags.begin(), opt_.extra_flags.end());
        e.resource_dir = host && !host->resource_dir.empty() ? host->resource_dir : opt_.resource_dir;
        if (!host && opt_.include_dirs.empty() && opt_.compile_commands_dir.empty())
        {
            const hot::DefaultPaths defaults = hot::default_paths();
            e.include_dirs = defaults.include_dirs;
            if (e.resource_dir.empty())
                e.resource_dir = defaults.resource_dir;
        }
        return e;
    }
    static bool same_host(const std::shared_ptr<const Host>& a, const std::shared_ptr<const Host>& b)
    {
        if (a == b)
            return true;
        if (!a || !b)
            return false;
        return a->name == b->name && a->resource_dir == b->resource_dir && a->std_version == b->std_version
            && a->include_dirs == b->include_dirs && a->cxx_flags() == b->cxx_flags();
    }
    void load_hosts();
    // Tells the client which host a document uses (window/logMessage, type Info).
    void log_host(const Doc& d, const std::shared_ptr<const Host>& host)
    {
        send_client({{"jsonrpc", "2.0"}, {"method", "window/logMessage"},
            {"params",
                {{"type", 3},
                    {"message",
                        "hot-ls: " + d.path + " uses " + (host ? "host " + host->name : std::string("no host"))}}}});
    }
    // Checks the host files after a document event. A document whose host changed gets the new
    // compile command. "current" is the document of the event: the caller updates it.
    void refresh_hosts(Doc* current);
    // The notification workspace/didChangeWatchedFiles: new, changed and deleted files on disk.
    void on_watched_files(const json& p);
    void update(Doc& d);
    void update_cpp(Doc& d);
    void send_generated(Doc& d, const std::string& cpp);
    json publish_message(const Doc& d) const;
    std::map<std::string, json> import_diagnostics(const Doc& d) const;
    void publish_imports(Doc& d);
    json imported_location(const Doc& d, const json& gen_lsp_pos, bool whole_word) const;
    json analyze_diagnostics(const Doc& d) const;

    // ---- clangd
    void read_compile_commands();
    void start_clangd(const json& init);
    std::string private_cdb_dir();
#ifdef _WIN32
    static bool write_cdb_copy(const fs::path& target, const fs::path& copy);
    void refresh_cdb_copy();
    // The source database, its copy and the write time of the source at the last copy.
    fs::path cdb_target_, cdb_copy_;
    fs::file_time_type cdb_time_{};
#endif
    void on_clangd_notification(const json& msg);
    void map_clangd_diagnostics(Doc& d);
    // clangd adds the symbols of a new preamble to its index on a worker thread, after the
    // preamble. A completion before that gets no index names (a native free function, for
    // example), and clangd says that the answer is complete. The index adds all symbols of one
    // preamble in one step. So the server waits until a runtime symbol of the preamble is in
    // the index. Each generated file includes "hot/runtime.h" (hot::ShellBase).
    // Limit: an earlier preamble of an other document can give the symbol before this preamble.
    // The wait has a limit of 5 s in total. Each request gets only the time that remains.
    void wait_for_preamble_index()
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
            if (left.count() <= 0)
                return;
            const auto r = clangd_result("workspace/symbol", {{"query", "ShellBase"}}, static_cast<int>(left.count()));
            if (r && r->is_array())
                for (const json& s : *r)
                    if (s.value("name", std::string()) == "ShellBase")
                        return;
            if (clock::now() + std::chrono::milliseconds(20) >= deadline)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::optional<json> clangd_result(const std::string& method, json params, int timeout_ms = 60000)
    {
        if (!clangd_ || !clangd_->running())
            return std::nullopt;
        json r = clangd_->request(method, std::move(params), timeout_ms);
        if (!r.is_object() || !r.contains("result") || r["result"].is_null())
            return std::nullopt;
        return r["result"];
    }

    // ---- features
    json on_initialize(const json& p);
    json on_completion(const Doc& d, const json& p);
    json on_hover(const Doc& d, const json& p);
    json on_signature_help(const Doc& d, const json& p);
    json on_formatting(const Doc& d, const json& p);
    json on_code_action(const Doc& d, const json& p);
    json on_locations(const Doc& d, const json& p, const std::string& method);
    std::vector<Pos> gen_word_candidates(const Doc& d, Pos script_pos) const;
    json on_document_symbol(const Doc& d);
    json on_semantic_tokens(const Doc& d);
    json on_workspace_symbol(const json& p);
    json script_member_location(const std::string& uri, const std::string& script_name);
    json on_to_script(const json& p);
    // Gives the edit, or an error message.
    std::variant<json, std::string> on_rename(const Doc& d, const json& p);

    // ---- symbols across the .hot files of the workspace (S18)
    // The script places of the symbol at "cur": the declaration and each use. Or an error
    // message. "decl" gets the script place of the declaration.
    std::variant<std::set<Pos>, std::string> symbol_places(const Doc& d, Pos cur, const std::string& name, Pos* decl);
    // The class (and the member) that is declared at "decl", or no value for a local symbol.
    std::optional<SymbolKey> symbol_key(const Doc& d, Pos decl, const std::string& name) const;
    std::optional<SymbolKey> key_at(const Doc& d, Pos cur);
    std::optional<Pos> key_position(const Doc& d, const SymbolKey& key) const;
    Pos class_name_pos(const Doc& d, const hotc::AnalyzedClass& c) const;
    std::set<Pos> class_places(const Doc& d, const std::string& name) const;
    bool is_script_class(const Doc& d, const std::string& name) const;
    std::vector<std::string> workspace_scripts() const;
    IndexShard shard_of_doc(const Doc& d) const;
    // Calls "fn" for each other .hot file of the workspace that declares the symbol "key".
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
    std::map<std::string, std::shared_ptr<Doc>> docs_; // by the script uri
    std::map<std::string, std::shared_ptr<Doc>> gen_docs_; // by the generated uri
    std::unique_ptr<ClangdProcess> clangd_;
    std::vector<std::string> roots_; // the workspace folders (paths), from initialize
    std::vector<HostSet> hosts_; // the host files: one set for each hosts dir
    bool utf8_ = false, clangd_utf8_ = false, shutdown_ = false;
    std::set<std::string> preamble_ready_;
    // The analyze facts of the scripts on disk that are not open (script_member_location).
    std::map<std::string, std::pair<std::string, hotc::AnalyzeResult>> disk_facts_;
    std::string index_seed_; // the first file of compile_commands.json (start_clangd)  // the generated files with a
                             // first completion
    // The saved symbol index of the scripts on disk (S28, symbol_index.h).
    SymbolIndex index_;
    int completion_translations_ = 0; // the translations of a completion text (hot/indexStats)
    int closed_analyses_ = 0; // the analyze calls of for_other_scripts (hot/indexStats)
    hotc::TranslatorSession session_;
};

// ------------------------------------------------------------------ setup

void Server::read_compile_commands()
{
    if (opt_.compile_commands_dir.empty())
        return;
    const auto text = read_file(opt_.compile_commands_dir + "/compile_commands.json");
    if (!text)
        return;
    const json db = json::parse(*text, nullptr, false);
    if (!db.is_array() || db.empty())
        return;
    std::vector<std::string> args;
    if (db[0].contains("arguments"))
    {
        for (const json& a : db[0]["arguments"])
            if (a.is_string())
                args.push_back(a.get<std::string>());
    }
    else if (db[0].contains("command"))
    {
        std::istringstream ss(db[0].value("command", std::string()));
        for (std::string a; ss >> a;)
            args.push_back(a);
    }
    const std::string dir = db[0].value("directory", std::string("."));
    index_seed_ = db[0].value("file", std::string());
    if (!index_seed_.empty() && fs::path(index_seed_).is_relative())
        index_seed_ = (fs::path(dir) / index_seed_).lexically_normal().string();
    auto add_dir = [&](std::string d)
    {
        if (fs::path(d).is_relative())
            d = (fs::path(dir) / d).lexically_normal().string();
        for (const std::string& x : opt_.include_dirs)
            if (x == d)
                return;
        opt_.include_dirs.push_back(d);
    };
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        if (a == "-I" && i + 1 < args.size())
            add_dir(args[++i]);
        else if (a.compare(0, 2, "-I") == 0)
            add_dir(a.substr(2));
        else if ((a == "-isystem" || a == "-D") && i + 1 < args.size())
        {
            opt_.extra_flags.push_back(a);
            opt_.extra_flags.push_back(args[++i]);
        }
        else if (a.compare(0, 2, "-D") == 0)
            opt_.extra_flags.push_back(a);
    }
}

// clangd stores its background index in "<dir of compile_commands.json>/.cache/clangd/index".
// XDG_CACHE_HOME does not change this. The child of hot-ls gets its own directory under
// ".cache/hot/clangd" with a link to the real compile_commands.json (a copy on Windows). So its
// index does not mix with the index of a C++ clangd on the same database (DECISIONS 18).
#ifdef _WIN32
// A copy with the long names, not a link. clangd keys a file by its path. A database with the
// short 8.3 names ("RUNNER~1") gives a second key for a file that the editor and hot-ls open
// with the long name. Also, a link on Windows needs a special privilege. clangd reads the copy.
// A CMake reconfigure changes the source, so refresh_cdb_copy writes the copy again.
bool Server::write_cdb_copy(const fs::path& target, const fs::path& copy)
{
    const auto text = read_file(target.string());
    json db = text ? json::parse(*text, nullptr, false) : json();
    if (!db.is_array())
        return false;
    for (json& e : db)
    {
        if (!e.is_object())
            continue;
        const fs::path d = utf8_path(long_path(e.value("directory", std::string("."))));
        e["directory"] = path_utf8(d);
        if (!e.contains("file") || !e["file"].is_string())
            continue;
        const std::string old_file = e["file"].get<std::string>();
        fs::path f = utf8_path(old_file);
        if (f.is_relative())
            f = d / f;
        const std::string abs_file = path_utf8(f.lexically_normal());
        const std::string new_file = long_path(abs_file);
        e["file"] = new_file;
        // clangd compiles the source of the argument list: it gets the long name too.
        if (e.contains("arguments") && e["arguments"].is_array())
            for (json& a : e["arguments"])
                if (a.is_string() && (a.get<std::string>() == old_file || a.get<std::string>() == abs_file))
                    a = new_file;
    }
    std::string out_text;
    try
    {
        out_text = db.dump(2);
    }
    catch (const json::exception&)
    {
        return false; // a database that is not UTF-8
    }
    std::ofstream out(copy, std::ios::binary);
    out << out_text;
    out.close();
    return static_cast<bool>(out);
}

// The editor opens a document after a reconfigure. So a new document checks the source.
void Server::refresh_cdb_copy()
{
    if (cdb_target_.empty())
        return;
    std::error_code ec;
    const auto t = fs::last_write_time(cdb_target_, ec);
    if (ec || t == cdb_time_)
        return;
    if (write_cdb_copy(cdb_target_, cdb_copy_))
        cdb_time_ = t;
}
#endif

std::string Server::private_cdb_dir()
{
    if (roots_.empty())
        return opt_.compile_commands_dir;
    std::error_code ec;
    const fs::path dir = fs::path(roots_[0]) / ".cache" / "hot" / "clangd";
    fs::create_directories(dir, ec);
    if (ec)
        return opt_.compile_commands_dir;
    const fs::path link = dir / "compile_commands.json";
    const fs::path target = fs::absolute(fs::path(opt_.compile_commands_dir) / "compile_commands.json", ec);
    if (ec || !fs::exists(target))
        return opt_.compile_commands_dir;
    fs::remove(link, ec);
#ifdef _WIN32
    const auto t = fs::last_write_time(target, ec);
    if (ec || !write_cdb_copy(target, link))
        return opt_.compile_commands_dir;
    cdb_target_ = target;
    cdb_copy_ = link;
    cdb_time_ = t;
    return dir.string();
#else
    fs::create_symlink(target, link, ec);
    return ec ? opt_.compile_commands_dir : dir.string();
#endif
}

void Server::start_clangd(const json& init)
{
    if (opt_.clangd.empty())
        return;
    // "--limit-results": the server drops many clangd names (the generated names, the "::"
    // names). The index search marks an answer incomplete when more than the limit of names
    // match. The default of 100 is too small for a short prefix and the MSVC STL. An
    // incomplete answer stops the reuse of the answer while the prefix grows.
    std::vector<std::string> args = {opt_.clangd_log.empty() ? "--log=error" : "--log=info",
        "--header-insertion=never", "-j=2", "--limit-results=1000"};
    if (!opt_.compile_commands_dir.empty())
    {
        args.push_back("--compile-commands-dir=" + private_cdb_dir());
        args.push_back("--background-index");
    }
    else
    {
        args.push_back("--background-index=false");
    }
    auto c = std::make_unique<ClangdProcess>();
    if (!c->start(opt_.clangd, args, opt_.clangd_log, [this](const json& m) { on_clangd_notification(m); }))
        return;
    json params = {
        {"processId", hot::detail::process_id()},
        {"capabilities",
            {{"general", {{"positionEncodings", {"utf-8", "utf-16"}}}},
                {"textDocument",
                    {{"completion", {{"completionItem", {{"snippetSupport", false}}}}},
                        {"hover", {{"contentFormat", {"markdown", "plaintext"}}}},
                        {"publishDiagnostics", {{"versionSupport", true}}}}}}},
    };
    for (const char* k : {"rootUri", "rootPath", "workspaceFolders"})
        if (init.contains(k))
            params[k] = init[k];
    json r = c->request("initialize", params, 30000);
    if (!r.is_object() || !r.contains("result"))
    {
        c->stop();
        return;
    }
    clangd_utf8_ = r.value("/result/capabilities/positionEncoding"_json_pointer, std::string()) == "utf-8";
    c->notify("initialized", json::object());
    // clangd loads the compile_commands.json, and starts the background index, at the first
    // file that it opens. The server opens and closes one entry of the database for this.
    if (const auto seed = index_seed_.empty() ? std::nullopt : read_file(index_seed_))
    {
        const std::string uri = path_to_uri(index_seed_);
        c->notify("textDocument/didOpen",
            {{"textDocument", {{"uri", uri}, {"languageId", "cpp"}, {"version", 1}, {"text", *seed}}}});
        c->notify("textDocument/didClose", {{"textDocument", {{"uri", uri}}}});
    }
    clangd_ = std::move(c);
}

void Server::load_hosts()
{
    hosts_.clear();
    std::vector<std::string> dirs;
    if (!opt_.hosts_dir.empty())
        dirs.push_back(opt_.hosts_dir);
    else
        for (const std::string& r : roots_)
            dirs.push_back((fs::path(r) / ".cache" / "hot" / "hosts").string());
    for (const std::string& dir : dirs)
    {
        HostSet set;
        set.load(dir, true);
        hosts_.push_back(std::move(set));
    }
}

void Server::refresh_hosts(Doc* current)
{
    bool changed = false;
    for (HostSet& h : hosts_)
        changed = h.refresh() || changed;
    if (!changed)
        return;
    disk_facts_.clear();
    std::vector<std::shared_ptr<Doc>> redo;
    for (const auto& [uri, d] : docs_)
    {
        const auto now = host_of(d->path);
        if (same_host(now, d->host))
            continue;
        d->host = now;
        d->no_host_told = false;
        if (d.get() != current)
            log_host(*d, now); // the caller logs the current document
        // clangd gets the new compile command at the next send_generated.
        if (clangd_ && d->in_clangd)
            clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", d->gen_uri}}}});
        d->in_clangd = false;
        preamble_ready_.erase(d->gen_uri);
        if (d.get() != current)
            redo.push_back(d);
    }
    for (const auto& d : redo)
        update(*d);
}

void Server::on_watched_files(const json& p)
{
    bool host_file = false;
    for (const json& c : p.value("changes", json::array()))
    {
        if (!c.is_object() || !c.value("uri", json()).is_string())
            continue;
        const std::string path = uri_to_path(c["uri"].get<std::string>());
        const std::string ext = fs::path(path).extension().string();
        if (ext == ".json" || ext == ".defines" || ext == ".options" || ext == ".includes")
        {
            for (HostSet& h : hosts_)
                host_file = h.force_reload_for(path) || host_file;
        }
        else if (ext == ".hot")
        {
            // An open document owns its text. For a closed file, the cache of the analysis goes.
            // A shard of a deleted file goes at the next index_.prune().
            disk_facts_.erase(path);
        }
    }
    if (host_file)
        refresh_hosts(nullptr);
}

json Server::on_initialize(const json& p)
{
    const json encodings = p.value("/capabilities/general/positionEncodings"_json_pointer, json::array());
    for (const json& e : encodings)
        if (e == "utf-8")
            utf8_ = true;
    if (const json o = p.value("initializationOptions", json::object()); o.is_object())
    {
        for (const json& d : o.value("includeDirs", json::array()))
            opt_.include_dirs.push_back(d.get<std::string>());
        if (o.contains("clangd"))
            opt_.clangd = o.value("clangd", opt_.clangd);
        if (o.contains("hostsDir"))
            opt_.hosts_dir = o.value("hostsDir", std::string());
        if (o.contains("compileCommandsDir"))
        {
            opt_.compile_commands_dir = o.value("compileCommandsDir", std::string());
            read_compile_commands();
        }
    }
    roots_.clear();
    if (p.contains("workspaceFolders") && p["workspaceFolders"].is_array())
    {
        for (const json& f : p["workspaceFolders"])
            if (f.is_object() && f.value("uri", std::string()).size())
                roots_.push_back(uri_to_path(f.value("uri", std::string())));
    }
    if (roots_.empty() && p.value("rootUri", json()).is_string())
        roots_.push_back(uri_to_path(p["rootUri"].get<std::string>()));
    if (roots_.empty() && p.value("rootPath", json()).is_string())
        roots_.push_back(p["rootPath"].get<std::string>());
    if (!opt_.index_dir.empty())
        index_.open(opt_.index_dir);
    else if (!roots_.empty())
        index_.open((fs::path(roots_[0]) / ".cache" / "hot" / "index").string());
    index_.prune();
    load_hosts();
    start_clangd(p);
    json tokens = json::array();
    for (const std::string& t : semantic_token_types())
        tokens.push_back(t);
    return {
        {"capabilities",
            {{"positionEncoding", utf8_ ? "utf-8" : "utf-16"},
                {"textDocumentSync", {{"openClose", true}, {"change", 1}}},
                {"completionProvider", {{"triggerCharacters", {".", ":"}}}}, {"hoverProvider", true},
                {"signatureHelpProvider", {{"triggerCharacters", {"(", ","}}}}, {"documentFormattingProvider", true},
                {"codeActionProvider", {{"codeActionKinds", {"quickfix"}}}}, {"definitionProvider", true},
                {"referencesProvider", true}, {"documentSymbolProvider", true}, {"renameProvider", true},
                {"workspaceSymbolProvider", true},
                {"semanticTokensProvider",
                    {{"legend", {{"tokenTypes", tokens}, {"tokenModifiers", json::array()}}}, {"full", true}}},
                {"experimental", {{"hotToScript", true}, {"clangd", clangd_ != nullptr}}}}},
        {"serverInfo", {{"name", "hot-ls"}, {"version", "0.1"}}},
    };
}

// ------------------------------------------------------------------ documents and diagnostics

void Server::send_generated(Doc& d, const std::string& cpp)
{
    if (!clangd_ || !clangd_->running())
        return;
    const int v = ++d.sent_version;
    if (!d.in_clangd)
    {
        // The compile command of the backend for the virtual file (design 10.1).
        hot::ClangFilesOptions co;
        const Effective e = effective_options(d.path);
        co.include_dirs = e.include_dirs;
        co.resource_dir = e.resource_dir;
        co.std_version = e.std_version;
        co.extra_flags = e.extra_flags;
        const std::vector<std::string> cmd = hot::compile_command(co, d.gen_path, d.gen_path + ".so");
        clangd_->notify("workspace/didChangeConfiguration",
            {{"settings",
                {{"compilationDatabaseChanges",
                    {{d.gen_path,
                        {{"workingDirectory", fs::path(d.gen_path).parent_path().string()},
                            {"compilationCommand", cmd}}}}}}}});
        clangd_->notify("textDocument/didOpen",
            {{"textDocument", {{"uri", d.gen_uri}, {"languageId", "cpp"}, {"version", v}, {"text", cpp}}}});
        d.in_clangd = true;
    }
    else
    {
        clangd_->notify("textDocument/didChange",
            {{"textDocument", {{"uri", d.gen_uri}, {"version", v}}},
                {"contentChanges", json::array({{{"text", cpp}}})}});
    }
}

// The update of a ".cpp" document. A script (a document with a host) gets the hotc errors as
// diagnostics. Any other ".cpp" document gets nothing. The state of clangd is not touched: clangd
// owns the other features of the file.
void Server::update_cpp(Doc& d)
{
    if (!d.host)
    {
        std::lock_guard<std::mutex> lock(docs_mutex_);
        d.facts = hotc::AnalyzeResult();
        if (d.cpp_published)
        {
            d.cpp_published = false;
            send_client({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
                {"params", {{"uri", d.uri}, {"diagnostics", json::array()}}}});
        }
        return;
    }
    hotc::AnalyzeResult facts = hotc::analyze_cpp(d.text.s, d.path, translate_options(d), session_);
    facts.diagnostics.clear(); // facts.errors has the errors of this file
    std::lock_guard<std::mutex> lock(docs_mutex_);
    d.facts = std::move(facts);
    d.cpp_published = true;
    send_client(publish_message(d));
}

void Server::update(Doc& d)
{
    if (d.cpp)
        return update_cpp(d);
    hotc::AnalyzeResult facts = hotc::analyze(d.text.s, translate_options(d), session_);
    std::string cpp = facts.cpp;
    {
        std::lock_guard<std::mutex> lock(docs_mutex_);
        d.facts = std::move(facts);
        if (!cpp.empty())
        {
            d.gen = Text(cpp);
            d.map = std::make_shared<const hotc::SourceMap>(d.facts.source_map);
            d.gen_script = d.text;
            d.gen_version = d.sent_version + 1;
            send_generated(d, cpp);
        }
        d.clang_diags = json::array();
        d.clang_import_diags.clear();
        d.set_edit();
        // clangd sends no new diagnostics when the new version has the same ones. So the last
        // ones go to the new text. A new publish of clangd replaces them.
        if (!cpp.empty())
            map_clangd_diagnostics(d);
        send_client(publish_message(d));
        publish_imports(d);
    }
}

static std::string script_file_path(const Doc& d, const std::string& name);

json Server::analyze_diagnostics(const Doc& d) const
{
    json out = json::array();
    std::map<std::string, std::pair<int, std::string>> headers; // the first error of each C++ header
    auto add = [&](Pos p, const std::string& message, int severity)
    {
        const auto [b, e] = word_at(d, p);
        const Pos end = b <= p.col && e > p.col ? Pos{p.line, e} : Pos{p.line, p.col + 1};
        out.push_back(
            {{"range", script_range(d, p, end)}, {"severity", severity}, {"source", "hot"}, {"message", message}});
    };
    for (const hotc::AnalyzeError& e : d.facts.errors)
        add({e.pos.line - 1, e.pos.col - 1}, e.message, 1);
    // The translation errors: "name:line:col: error: text", one line each.
    static const std::regex re(R"(^(.*):(\d+):(\d+): (fatal error|error|warning): (.*)$)");
    std::istringstream ss(d.facts.diagnostics);
    for (std::string l; std::getline(ss, l);)
    {
        std::smatch m;
        if (!std::regex_match(l, m, re))
            continue;
        if (m[1] != d.path)
        {
            // An imported script: import_diagnostics() has it. An error in a C++ header does not
            // go to the header (the C++ tools own it): it shows at the "using" line of the script.
            const std::string path = script_file_path(d, m[1]);
            if (!is_served_path(path) && path != d.path && m[4] != "warning")
                headers.emplace(path, std::make_pair(std::stoi(m[2]), m[5].str()));
            continue;
        }
        add({std::stoi(m[2]) - 1, std::stoi(m[3]) - 1}, m[5], m[4] == "warning" ? 2 : 1);
    }
    for (const auto& [uri, diags] : d.clang_import_diags)
    {
        const std::string path = uri_to_path(uri);
        if (is_served_path(path) || diags.empty())
            continue;
        const json& first = diags[0];
        headers.emplace(path,
            std::make_pair(first["range"]["start"]["line"].get<int>() + 1, first.value("message", std::string())));
    }
    for (const auto& [path, err] : headers)
    {
        const std::string name = fs::path(path).filename().string();
        int line = 0;
        for (int i = 0; i < static_cast<int>(d.text.starts.size()); ++i)
        {
            const std::string_view l = d.text.line(i);
            if (l.find("using") != std::string_view::npos && l.find(name) != std::string_view::npos)
            {
                line = i;
                break;
            }
        }
        const int len = std::max<int>(1, static_cast<int>(d.text.line(line).size()));
        out.push_back({{"range", script_range(d, {line, 0}, {line, len})}, {"severity", 1}, {"source", "hot"},
            {"message",
                "error in header '" + name + "': " + err.second + " (line " + std::to_string(err.first) + ")"}});
    }
    // A script without a host and without -I options has no include directories.
    if (opt_.include_dirs.empty() && opt_.compile_commands_dir.empty() && fs::path(d.path).extension() == ".hot"
        && !host_of(d.path))
        out.push_back(
            {{"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 0}}}}},
                {"severity", 3}, {"source", "hot-ls"},
                {"message", "No script host for this file. Add hot_script_host(...) in CMake and run CMake again."}});
    return out;
}

// The path of a script that the translator names, as an absolute path.
static std::string script_file_path(const Doc& d, const std::string& name)
{
    fs::path f(name);
    if (f.is_relative())
    {
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
    static const std::regex re(R"(^(.*):(\d+):(\d+): (fatal error|error|warning): (.*)$)");
    std::istringstream ss(d.facts.diagnostics);
    std::map<std::string, Text> texts;
    for (std::string l; std::getline(ss, l);)
    {
        std::smatch m;
        if (!std::regex_match(l, m, re) || m[1] == d.path || m[1].length() == 0)
            continue;
        const std::string path = script_file_path(d, m[1]);
        const std::string uri = path_to_uri(path);
        if (uri == d.uri)
            continue;
        auto t = texts.find(path);
        if (t == texts.end())
            t = texts.emplace(path, Text(read_file(path).value_or(std::string()))).first;
        const int line = std::stoi(m[2]) - 1, col = std::stoi(m[3]) - 1;
        const std::string_view text = t->second.line(line);
        int end = std::min<int>(col, static_cast<int>(text.size()));
        while (end < static_cast<int>(text.size()) && is_id_char(text[static_cast<size_t>(end)]))
            ++end;
        if (end <= col)
            end = col + 1;
        const json range = {
            {"start", to_lsp(t->second, {line, col}, utf8_)}, {"end", to_lsp(t->second, {line, end}, utf8_)}};
        out[uri].push_back(
            {{"range", range}, {"severity", m[4] == "warning" ? 2 : 1}, {"source", "hot"}, {"message", m[5].str()}});
    }
    for (const auto& [uri, diags] : d.clang_import_diags)
    {
        if (uri == d.uri)
            continue;
        for (const json& x : diags)
            out[uri].push_back(x);
    }
    // A C++ header is not a script: the C++ tools show its errors. hot-ls does not publish for it.
    for (auto it = out.begin(); it != out.end();)
    {
        if (is_served_path(uri_to_path(it->first)))
            ++it;
        else
            it = out.erase(it);
    }
    return out;
}

// Publishes the translator errors of the imported scripts. A script that is open has its own
// diagnostics: it gets none here. A script that had errors and has none now gets an empty list.
void Server::publish_imports(Doc& d)
{
    std::map<std::string, json> now = import_diagnostics(d);
    for (auto it = now.begin(); it != now.end();)
    {
        if (docs_.count(it->first))
            it = now.erase(it);
        else
            ++it;
    }
    auto send = [&](const std::string& uri, const json& diags)
    {
        send_client({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", uri}, {"diagnostics", diags}}}});
    };
    for (const auto& [uri, diags] : now)
        send(uri, diags);
    for (const std::string& uri : d.import_uris)
        if (!now.count(uri))
            send(uri, json::array());
    d.import_uris.clear();
    for (const auto& [uri, diags] : now)
        d.import_uris.insert(uri);
}

// A place in the generated text that comes from an imported script, as an LSP location in that
// script, or null. The range is the word at the place if "whole_word" is true.
json Server::imported_location(const Doc& d, const json& gen_lsp_pos, bool whole_word) const
{
    if (!d.map)
        return nullptr;
    const Pos g = from_lsp(d.gen, gen_lsp_pos, clangd_utf8_);
    const auto loc = d.map->locate(g.line + 1, g.col + 1);
    if (!loc || loc->file == 0 || loc->file > d.map->files().size())
        return nullptr;
    const hotc::SourceFile& f = d.map->files()[loc->file - 1];
    const Text t(f.text);
    const Pos b{loc->pos.line - 1, loc->pos.col - 1};
    const std::string_view text = t.line(b.line);
    int end = std::min<int>(b.col, static_cast<int>(text.size()));
    if (whole_word)
        while (end < static_cast<int>(text.size()) && is_id_char(text[static_cast<size_t>(end)]))
            ++end;
    const std::string path = script_file_path(d, f.name);
    return {{"uri", path_to_uri(path)},
        {"range", {{"start", to_lsp(t, b, utf8_)}, {"end", to_lsp(t, {b.line, std::max(end, b.col)}, utf8_)}}}};
}

json Server::publish_message(const Doc& d) const
{
    json diags = analyze_diagnostics(d);
    for (const json& x : d.clang_diags)
        diags.push_back(x);
    return {{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
        {"params", {{"uri", d.uri}, {"diagnostics", diags}}}};
}

// Maps the raw clangd diagnostics of the document to the script text. The caller holds docs_mutex_.
void Server::map_clangd_diagnostics(Doc& d)
{
    const json raw = d.raw_clang_diags;
    json out = json::array();
    std::map<std::string, json> from_imports;
    for (const json& g : raw)
    {
        Pos b, e;
        const auto s = to_script(d, g["range"]["start"]);
        if (!s)
        {
            // The place can be in an imported script: the diagnostic goes to that script.
            const json at = imported_location(d, g["range"]["start"], true);
            if (!at.is_null())
            {
                from_imports[at.value("uri", std::string())].push_back(
                    {{"range", at["range"]}, {"severity", g.value("severity", 1)}, {"source", "clangd"},
                        {"message", hotc::rewrite_clang_message(g.value("message", std::string()))}});
                continue;
            }
        }
        if (s)
        {
            b = *s;
            const auto se = to_script(d, g["range"]["end"]);
            e = se && se->line == b.line && b.col < se->col ? *se : Pos{b.line, word_at(d, b).second};
            if (e.col <= b.col)
                e.col = b.col + 1;
        }
        else
        {
            // No segment: the "#line" directives give the script line only. The column is the
            // column of the same word in the script line. Else it is the text of the line.
            const Pos gs = from_lsp(d.gen, g["range"]["start"], clangd_utf8_);
            const auto t = line_directive_target(d.gen.s, gs.line);
            if (!t || t->file != d.path)
                continue;
            const auto at = d.gen_script_to_text({t->line, 0});
            const int line = at ? at->line : t->line;
            const std::string_view sl = d.text.line(line), gl = d.gen.line(gs.line);
            int wb = std::min<int>(gs.col, static_cast<int>(gl.size())), we = wb;
            while (we < static_cast<int>(gl.size()) && is_id_char(gl[static_cast<size_t>(we)]))
                ++we;
            const std::string_view w = gl.substr(static_cast<size_t>(wb), static_cast<size_t>(we - wb));
            size_t found = std::string_view::npos;
            for (size_t from = 0; !w.empty() && (found = sl.find(w, from)) != std::string_view::npos; from = found + 1)
            {
                const bool left = found == 0 || !is_id_char(sl[found - 1]);
                const bool right = found + w.size() >= sl.size() || !is_id_char(sl[found + w.size()]);
                if (left && right)
                    break;
            }
            if (found != std::string_view::npos && !w.empty())
            {
                b = {line, static_cast<int>(found)};
                e = {line, static_cast<int>(found + w.size())};
            }
            else
            {
                int first = 0;
                while (first < static_cast<int>(sl.size())
                    && (sl[static_cast<size_t>(first)] == ' ' || sl[static_cast<size_t>(first)] == '\t'))
                    ++first;
                b = {line, first};
                e = {line, std::max(first + 1, static_cast<int>(sl.size()))};
            }
        }
        out.push_back({{"range", script_range(d, b, e)}, {"severity", g.value("severity", 1)}, {"source", "clangd"},
            {"message", hotc::rewrite_clang_message(g.value("message", std::string()))}});
    }
    d.clang_diags = std::move(out);
    d.clang_import_diags = std::move(from_imports);
}

void Server::on_clangd_notification(const json& msg)
{
    if (msg.value("method", std::string()) != "textDocument/publishDiagnostics")
        return;
    const json& p = msg["params"];
    std::lock_guard<std::mutex> lock(docs_mutex_);
    const auto it = gen_docs_.find(p.value("uri", std::string()));
    if (it == gen_docs_.end())
        return;
    Doc& d = *it->second;
    // A diagnostic of a completion text (or of an old text) is not shown.
    if (p.contains("version") && p["version"].is_number_integer() && p["version"].get<int>() != d.gen_version)
        return;
    d.raw_clang_diags = p.value("diagnostics", json::array());
    map_clangd_diagnostics(d);
    send_client(publish_message(d));
    publish_imports(d);
}

// ------------------------------------------------------------------ features

json Server::map_location(const Doc& d, const json& loc) const
{
    const std::string uri =
        loc.contains("targetUri") ? loc.value("targetUri", std::string()) : loc.value("uri", std::string());
    const json range = loc.contains("targetSelectionRange") ? loc["targetSelectionRange"] : loc.value("range", json());
    if (uri != d.gen_uri)
        return {{"uri", uri}, {"range", range}}; // a native place: no change
    const auto s = to_script(d, range["start"]);
    if (!s)
        return imported_location(d, range["start"], true); // null when not in an import
    const auto [b, e] = word_at(d, *s);
    return {{"uri", d.uri}, {"range", script_range(d, {s->line, b}, {s->line, std::max(e, b)})}};
}

// A word that the translator or the shell makes. It is not a script name (see design 4.2).
static bool is_generated_name(const std::string& n)
{
    static const char* const parts[] = {
        "__", "_Shell", "_SLOT_", "_thunk", "_slot", "_layout", "_fields", "_g1", "_vtable"};
    for (const char* x : parts)
        if (n.find(x) != std::string::npos)
            return true;
    return n[0] == '_' || n.compare(0, 7, "hot_mod") == 0 || n.compare(0, 7, "hot_gen") == 0 ||
           n.compare(0, 9, "HotModule") == 0 || n == "hot";
}

static bool has_prefix(const std::string& name, const std::string& prefix)
{
    if (name.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

json Server::on_completion(const Doc& d, const json& p)
{
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    const std::string& t = d.text.s;
    const size_t off = d.text.offset(cur.line, cur.col);
    // The word at the cursor is [s, e). The typed prefix is [s, off).
    size_t s = off, e = off;
    while (s > 0 && is_id_char(t[s - 1]))
        --s;
    while (e < t.size() && is_id_char(t[e]))
        ++e;
    const std::string prefix = t.substr(s, off - s);
    const bool after_dot = s > 0 && t[s - 1] == '.';
    const bool after_scope = s >= 2 && t[s - 1] == ':' && t[s - 2] == ':';
    const json empty = {{"isIncomplete", false}, {"items", json::array()}};
    // A single ":" is a trigger character only for "::".
    if (p.value("/context/triggerCharacter"_json_pointer, std::string()) == ":" && !after_scope)
        return empty;
    // No completion in a line comment.
    const size_t line_begin = d.text.starts[static_cast<size_t>(cur.line)];
    if (t.substr(line_begin, off - line_begin).find("//") != std::string::npos)
        return empty;

    const bool member = after_dot || after_scope;
    json items = json::array();
    std::set<std::string> names;
    auto add = [&](const std::string& name, int kind, const std::string& detail, const std::string& sort,
                   const std::string& label = std::string())
    {
        if (name.empty() || !has_prefix(name, prefix) || !names.insert(name).second)
            return;
        json item = {{"label", label.empty() ? name : label}, {"kind", kind}, {"insertText", name},
            {"filterText", name}, {"sortText", sort + name}};
        if (!detail.empty())
            item["detail"] = detail;
        items.push_back(std::move(item));
    };

    // ---- the script facts (they work without clangd)
    const ScriptScope scope = scan_scope(t, s, s);
    // The members of a script class and of its script bases. clangd does not see them on the shell.
    auto add_members = [&](const std::string& class_name)
    {
        std::set<std::string> seen;
        for (std::string n = class_name; !n.empty() && seen.insert(n).second;)
        {
            const hotc::AnalyzedClass* cls = nullptr;
            for (const hotc::AnalyzedClass& c : d.facts.classes)
                if (c.name == n)
                    cls = &c;
            if (!cls)
                break;
            for (const hotc::AnalyzedMember& m : cls->members)
            {
                int kind = 0;
                if (m.kind == "field")
                    kind = 5;
                else if (m.kind == "method")
                    kind = 2;
                else if (m.kind == "property")
                    kind = 10;
                if (kind)
                    add(m.name, kind, "script " + m.kind + " of " + cls->name, "0");
            }
            n = cls->base;
        }
    };
    if (after_dot)
    {
        size_t ob = s - 1;
        while (ob > 0 && is_id_char(t[ob - 1]))
            --ob;
        const std::string obj = t.substr(ob, s - 1 - ob);
        if (obj == "this")
        {
            const hotc::AnalyzedClass* cls = nullptr;
            for (const hotc::AnalyzedClass& c : d.facts.classes)
                if (Pos{c.begin.line - 1, c.begin.col - 1} < Pos{cur.line, cur.col + 1})
                    cls = &c;
            if (cls)
                add_members(cls->name);
        }
        else if (const auto en = scope.enum_members.find(obj); en != scope.enum_members.end())
        {
            for (const std::string& m : en->second)
                add(m, 20, "enumerator of " + obj, "0");
        }
        else
        {
            std::string type;
            for (const ScopeName& n : scope.locals)
                if (n.name == obj)
                    type = n.type;
            for (const ScopeName& n : scope.module_vars)
                if (n.name == obj && type.empty())
                    type = n.type;
            if (!type.empty())
                add_members(type);
        }
    }
    else if (!member)
    {
        for (const ScopeName& n : scope.locals)
            add(n.name, 6, n.detail + (n.type.empty() ? "" : ": " + n.type), "0");
        for (const ScopeName& n : scope.module_vars)
            add(n.name, 6, n.detail + (n.type.empty() ? "" : ": " + n.type), "0");
        for (const std::string& n : scope.enums)
            add(n, 13, "script enum", "1");
        for (const std::string& n : scope.classes)
            add(n, 7, "script class", "1");
        for (const std::string& n : scope.structs)
            add(n, 22, "script struct", "1");
    }

    // ---- the clangd part. The word at the cursor is replaced by the typed prefix and a placeholder
    // name, so the statement translates and clangd completes at its generated place. The real text
    // comes back after. The last answer is reused while only the prefix grows: clangd filters by
    // the prefix, so an answer that was not cut by the limit holds the longer prefix too.
    bool incomplete = false;
    if (clangd_ && clangd_->running())
    {
        Doc::CompletionCache& cc = d.completion;
        const std::string rest = t.substr(0, s) + '\x01' + t.substr(e);
        const bool hit =
            cc.valid && cc.complete && cc.rest == rest && prefix.compare(0, cc.prefix.size(), cc.prefix) == 0;
        if (!hit)
        {
            ++completion_translations_;
            size_t n = e;
            while (n < t.size() && (t[n] == ' ' || t[n] == '\t' || t[n] == '\r' || t[n] == '\n'))
                ++n;
            const bool end =
                n < t.size() && std::string_view(";),]([+-*/%=<>!&|^?:.").find(t[n]) != std::string_view::npos;
            // The brackets that the statement before the word leaves open.
            std::string closers;
            if (!end)
            {
                std::vector<char> open;
                for (size_t i = s; i > 0 && t[i - 1] != ';' && t[i - 1] != '{' && t[i - 1] != '}'; --i)
                {
                    const char c = t[i - 1];
                    if (c == ')' || c == ']')
                        open.push_back(c == ')' ? '(' : '[');
                    else if (c == '(' || c == '[')
                    {
                        if (!open.empty() && open.back() == c)
                            open.pop_back();
                        else
                            closers += c == '(' ? ')' : ']';
                    }
                }
            }
            const std::string patched = t.substr(0, s) + prefix + kHole + closers + (end ? "" : ";") + t.substr(e);
            hotc::AnalyzeResult facts = hotc::analyze(patched, translate_options(d), session_);
            const Pos hole = {cur.line, static_cast<int>(s - d.text.starts[static_cast<size_t>(cur.line)])};
            const auto g =
                facts.cpp.empty() ? std::nullopt : facts.source_map.map_to_generated(hole.line + 1, hole.col + 1);
            std::optional<json> r;
            if (g)
            {
                Doc& md = const_cast<Doc&>(d);
                const Text gen(facts.cpp);
                send_generated(md, facts.cpp);
                // Before its first preamble, clangd gives only "fallback" words of the file. A
                // request that needs the AST waits for the preamble.
                if (preamble_ready_.insert(d.gen_uri).second)
                {
                    clangd_result("textDocument/documentSymbol", {{"textDocument", {{"uri", d.gen_uri}}}});
                    wait_for_preamble_index();
                }
                r = clangd_result("textDocument/completion",
                    {{"textDocument", {{"uri", d.gen_uri}}},
                        {"position",
                            to_lsp(gen, {g->line - 1, g->col - 1 + static_cast<int>(prefix.size())}, clangd_utf8_)}});
                std::lock_guard<std::mutex> lock(docs_mutex_);
                md.gen_version = md.sent_version + 1; // before the send: the diagnostics can come at once
                send_generated(md, d.gen.s);
            }
            cc.rest = rest;
            cc.prefix = prefix;
            cc.valid = true;
            cc.list = r ? (r->is_array() ? *r : r->value("items", json::array())) : json::array();
            // A translation that fails stays failed while the prefix grows. An empty answer is not
            // complete: clangd gives the names of the headers from its index, and it fills the index
            // after the preamble. An early answer can be empty, so the next request asks again.
            cc.complete = !g || (r && !cc.list.empty() && !(r->is_object() && r->value("isIncomplete", false)));
        }
        incomplete = !cc.complete;
        for (const json& it : cc.list)
        {
            std::string name = it.value("filterText", std::string());
            if (name.empty())
                name = it.value("insertText", it.value("label", std::string()));
            const int kind = it.value("kind", 1);
            // The names of the generated code and of the shell are not script names.
            if (name.empty() || name.find(kHole) != std::string::npos || is_generated_name(name)
                || name.compare(0, 8, "operator") == 0 || name[0] == '~' || kind == 4
                || name.find("::") != std::string::npos)
                continue;
            if (after_dot && (name == "cls" || name == "state"))
                continue; // hot::ShellBase (runtime/shell.h)
            if (!member)
            {
                // A name in scope: a variable, a function, a type, an enumerator, a namespace.
                static const std::set<int> kinds = {3, 6, 7, 9, 13, 20, 21, 22};
                if (!kinds.count(kind) || name.compare(0, 2, "__") == 0)
                    continue;
            }
            std::string label = it.value("label", name);
            while (!label.empty() && (label[0] == ' '))
                label.erase(0, 1);
            if (label.compare(0, 3, "\xE2\x80\xA2") == 0)
                label.erase(0, 3); // the include bullet
            if (!has_prefix(name, prefix) || !names.insert(name).second)
                continue;
            // A name from a namespace comes as "std::string": the script form is "std.string".
            std::string insert = it.value("insertText", name);
            insert = insert.find("::") != std::string::npos ? hotc::rewrite_generated_names(insert) : name;
            json item = {{"label", hotc::rewrite_generated_names(label)}, {"kind", kind}, {"insertText", insert},
                {"filterText", name}, {"sortText", "2" + it.value("sortText", name)}};
            if (it.contains("detail"))
                item["detail"] = hotc::rewrite_generated_names(it.value("detail", std::string()));
            items.push_back(std::move(item));
        }
    }

    // ---- the keywords and the builtin types
    if (!member)
    {
        // "self" is a stub attribute. It is also the name of a generated helper, so it is not offered.
        for (const std::string& k : script_keywords())
            if (k != "self")
                add(k, 14, "keyword", "3");
        // The global helpers of a script (include/hot/runtime/enumerate.h).
        for (const std::string& h : script_helpers())
            add(h, 3, "global function", "3");
        for (const std::string& k : builtin_type_names())
            add(k, 25, "builtin type", "3");
    }
    return {{"isIncomplete", incomplete}, {"items", items}};
}

json Server::on_hover(const Doc& d, const json& p)
{
    const auto g = to_gen(d, from_lsp(d.text, p["position"], utf8_));
    if (!g)
        return nullptr;
    auto r =
        clangd_result("textDocument/hover", {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, *g)}});
    if (!r || !r->contains("contents"))
        return nullptr;
    json c = (*r)["contents"];
    if (c.is_object() && c.contains("value"))
        c["value"] = hotc::rewrite_generated_names(c.value("value", std::string()));
    else if (c.is_string())
        c = hotc::rewrite_generated_names(c.get<std::string>());
    json out = {{"contents", c}};
    if (r->contains("range"))
    {
        const auto b = to_script(d, (*r)["range"]["start"]);
        if (b)
        {
            const auto [wb, we] = word_at(d, *b);
            out["range"] = script_range(d, {b->line, wb}, {b->line, we});
        }
    }
    return out;
}

// The signature of the function that the cursor is in. The call is in the script. clangd gets the
// place after the "(" of the generated call. The active parameter is the number of commas
// between the "(" and the cursor, so it does not depend on the generated arguments.
json Server::on_signature_help(const Doc& d, const json& p)
{
    const std::string& t = d.text.s;
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    size_t i = d.text.offset(cur.line, cur.col), open = std::string::npos;
    int depth = 0, commas = 0;
    while (i > 0)
    {
        const char c = t[--i];
        if (c == ')' || c == ']' || c == '}')
        {
            ++depth;
        }
        else if (c == '(' || c == '[' || c == '{')
        {
            if (depth == 0)
            {
                if (c == '(')
                    open = i;
                break;
            }
            --depth;
        }
        else if (depth == 0 && c == ',')
        {
            ++commas;
        }
        else if (depth == 0 && c == ';')
        {
            break;
        }
    }
    if (open == std::string::npos)
        return nullptr;
    // The name of the function is before the "(".
    size_t name_end = open;
    while (name_end > 0 && (t[name_end - 1] == ' ' || t[name_end - 1] == '\t'))
        --name_end;
    size_t name_begin = name_end;
    while (name_begin > 0 && is_id_char(t[name_begin - 1]))
        --name_begin;
    if (name_begin == name_end)
        return nullptr;
    const auto [nl, nc] = d.text.pos(name_begin);
    const auto g = to_gen(d, {nl, nc});
    if (!g)
        return nullptr;
    // The "(" of the generated call is the first one after the name.
    const std::string_view gl = d.gen.line(g->line);
    const size_t paren = gl.find('(', static_cast<size_t>(g->col));
    if (paren == std::string_view::npos)
        return nullptr;
    auto r = clangd_result("textDocument/signatureHelp",
        {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, {g->line, static_cast<int>(paren) + 1})}});
    if (!r || !r->contains("signatures") || (*r)["signatures"].empty())
        return nullptr;
    json out = *r;
    for (json& sig : out["signatures"])
    {
        sig["label"] = hotc::rewrite_generated_names(sig.value("label", std::string()));
        sig.erase("activeParameter");
        if (sig.contains("parameters"))
            for (json& prm : sig["parameters"])
                if (prm.contains("label") && prm["label"].is_string())
                    prm["label"] = hotc::rewrite_generated_names(prm["label"].get<std::string>());
    }
    out["activeParameter"] = commas;
    return out;
}

// The format of a script: the indent follows the braces, a line inside open brackets has one
// more level, a line has no trailing white space, and the text ends with a new line. The text
// of a string, of a char and of a comment does not change.
static std::string format_script(const std::string& text, int tab_size, bool use_spaces)
{
    const std::string unit = use_spaces ? std::string(static_cast<size_t>(std::max(tab_size, 1)), ' ') : "\t";
    std::string out;
    int braces = 0, parens = 0;
    bool in_block = false;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr)
            line.pop_back();
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
            ++first;
        std::string body = line.substr(first);
        if (in_block)
        {
            // The inside of a block comment keeps its own indent.
        }
        else if (!body.empty())
        {
            int b = braces, q = parens;
            if (body[0] == '}')
                b = std::max(b - 1, 0);
            if (body[0] == ')' || body[0] == ']')
                q = std::max(q - 1, 0);
            int level = b + (q > 0 ? 1 : 0);
            std::string indent;
            for (int k = 0; k < level; ++k)
                indent += unit;
            line = indent + body;
        }
        else
        {
            line.clear();
        }
        // Scan the line for the brackets.
        char quote = 0;
        for (size_t k = 0; k < body.size(); ++k)
        {
            const char c = body[k];
            if (in_block)
            {
                if (c == '*' && k + 1 < body.size() && body[k + 1] == '/')
                {
                    in_block = false;
                    ++k;
                }
            }
            else if (quote)
            {
                if (c == '\\')
                    ++k;
                else if (c == quote)
                    quote = 0;
            }
            else if (c == '"' || c == '\'')
            {
                quote = c;
            }
            else if (c == '/' && k + 1 < body.size() && body[k + 1] == '/')
            {
                break;
            }
            else if (c == '/' && k + 1 < body.size() && body[k + 1] == '*')
            {
                in_block = true;
                ++k;
            }
            else if (c == '{')
            {
                ++braces;
            }
            else if (c == '}')
            {
                braces = std::max(braces - 1, 0);
            }
            else if (c == '(' || c == '[')
            {
                ++parens;
            }
            else if (c == ')' || c == ']')
            {
                parens = std::max(parens - 1, 0);
            }
        }
        out += line;
        if (!last)
            out += cr ? "\r\n" : "\n";
        else if (!line.empty() || !out.empty())
            out += cr ? "\r\n" : "\n";
        pos = end + 1;
    }
    return out;
}

json Server::on_formatting(const Doc& d, const json& p)
{
    const json o = p.value("options", json::object());
    const std::string formatted = format_script(d.text.s, o.value("tabSize", 4), o.value("insertSpaces", true));
    if (formatted == d.text.s)
        return json::array();
    const int last = static_cast<int>(d.text.starts.size()) - 1;
    const Pos end = {last, static_cast<int>(d.text.line(last).size())};
    return json::array({{{"range", script_range(d, {0, 0}, end)}, {"newText", formatted}}});
}

// The quick fixes for the diagnostics of the request: a missing ";" and a name that clangd
// suggests ("did you mean 'x'").
json Server::on_code_action(const Doc& d, const json& p)
{
    json actions = json::array();
    const std::string& t = d.text.s;
    auto add = [&](const std::string& title, const json& diag, const json& edit)
    {
        actions.push_back({{"title", title}, {"kind", "quickfix"}, {"diagnostics", json::array({diag})},
            {"edit", {{"changes", {{d.uri, json::array({edit})}}}}}});
    };
    static const std::regex suggestion(R"(did you mean '([^']+)')");
    for (const json& diag : p.value("/context/diagnostics"_json_pointer, json::array()))
    {
        const std::string message = diag.value("message", std::string());
        const json range = diag.value("range", json::object());
        if (!range.contains("start") || !range.contains("end"))
            continue;
        const Pos b = from_lsp(d.text, range["start"], utf8_), e = from_lsp(d.text, range["end"], utf8_);
        if (diag.value("source", "") == "hot" && message == "expected ';'")
        {
            // The error is at the next token. The ";" goes after the last token before it.
            size_t at = d.text.offset(b.line, b.col);
            while (at > 0 && std::isspace(static_cast<unsigned char>(t[at - 1])))
                --at;
            const auto [l, c] = d.text.pos(at);
            const json pt = to_lsp(d.text, {l, c}, utf8_);
            add("Insert ';'", diag, {{"range", {{"start", pt}, {"end", pt}}}, {"newText", ";"}});
            continue;
        }
        std::smatch m;
        if (diag.value("source", "") == "clangd" && std::regex_search(message, m, suggestion) && b.line == e.line
            && b < e)
            add("Replace with '" + m[1].str() + "'", diag, {{"range", range}, {"newText", m[1].str()}});
    }
    return actions;
}

// True for a word that can name a native or script symbol: not a keyword, a builtin type, a
// number or a name that the translator makes.
static bool is_symbol_word(const std::string& w)
{
    const auto is_one_of = [&w](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), w) != v.end(); };
    return !w.empty() && (std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_') && !is_generated_name(w)
        && !is_one_of(script_keywords()) && !is_one_of(builtin_type_names());
}

// Replaces the text of the string literals, the character literals and the comments of a line by
// spaces. "in_block" is true inside a block comment that goes on to the next line.
static std::string blank_non_code(std::string_view l, bool& in_block)
{
    std::string out(l);
    for (size_t i = 0; i < l.size();)
    {
        if (in_block)
        {
            const size_t e = l.find("*/", i);
            const size_t end = e == std::string_view::npos ? l.size() : e + 2;
            std::fill(out.begin() + static_cast<long>(i), out.begin() + static_cast<long>(end), ' ');
            in_block = e == std::string_view::npos;
            i = end;
        }
        else if (l.compare(i, 2, "//") == 0)
        {
            std::fill(out.begin() + static_cast<long>(i), out.end(), ' ');
            break;
        }
        else if (l.compare(i, 2, "/*") == 0)
            in_block = true;
        else if (l[i] == '"' || l[i] == '\'')
        {
            size_t e = i + 1;
            while (e < l.size() && l[e] != l[i])
                e += l[e] == '\\' ? 2 : 1;
            e = std::min(e + 1, l.size());
            std::fill(out.begin() + static_cast<long>(i), out.begin() + static_cast<long>(e), ' ');
            i = e;
        }
        else
            ++i;
    }
    return out;
}

// The generated places of the word at a script position, for a word with no map segment. The
// "#line" directives give the script line of each generated line. A generated line of the same
// script line that has the word gives a place. A word in a string or a comment is not a place.
// A line that follows a directive at once is in the mapped region of the script line: its places
// come first. The wrapper text of the translator (the shell, the stubs) comes after. In the
// mapped region, the place with the same number of the word as in the script line comes first
// when both have the same count of the word. The caller asks the first places only.
std::vector<Pos> Server::gen_word_candidates(const Doc& d, Pos script_pos) const
{
    std::vector<Pos> out;
    const auto [wb, we] = word_at(d, script_pos);
    if (we <= wb)
        return out;
    const std::string w = word(d, script_pos);
    if (!is_symbol_word(w))
        return out;
    const auto old = d.text_to_gen_script({script_pos.line, wb});
    if (!old)
        return out;
    auto whole_words = [&w](std::string_view l)
    {
        std::vector<int> cols;
        for (size_t from = 0, at; (at = l.find(w, from)) != std::string_view::npos; from = at + 1)
            if ((at == 0 || !is_id_char(l[at - 1])) && (at + w.size() >= l.size() || !is_id_char(l[at + w.size()])))
                cols.push_back(static_cast<int>(at));
        return cols;
    };
    bool script_block = false;
    const std::vector<int> in_script = whole_words(blank_non_code(d.gen_script.line(old->line), script_block));
    const auto nth = std::find(in_script.begin(), in_script.end(), old->col);
    // Walk the generated lines. A directive sets the script line of the lines after it. A
    // directive of the .cpp file marks text of the translator: it has no script line.
    struct Place
    {
        Pos pos;
        bool mapped; // the line follows a directive at once
    };
    std::vector<Place> places;
    int script_line = -1, last_directive = -1;
    bool in_block = false;
    for (int l = 0; l < static_cast<int>(d.gen.starts.size()); ++l)
    {
        const std::string_view t = d.gen.line(l);
        size_t i = 0;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t'))
            ++i;
        if (!in_block && i < t.size() && t[i] == '#')
        {
            ++i;
            while (i < t.size() && t[i] == ' ')
                ++i;
            if (t.compare(i, 4, "line") == 0)
            {
                i += 4;
                const int n = std::atoi(std::string(t.substr(i)).c_str());
                const size_t q1 = t.find('"', i), q2 = t.rfind('"');
                std::string file;
                for (size_t k = q1 + 1; q1 != std::string_view::npos && k < q2; ++k)
                {
                    if (t[k] == '\\' && k + 1 < q2)
                        ++k;
                    file += t[k];
                }
                script_line = file == d.path ? n - 1 : -1;
                last_directive = l;
            }
            continue; // a directive or an include line
        }
        const std::string code = blank_non_code(t, in_block);
        if (script_line < 0 || script_line + (l - last_directive - 1) != old->line)
            continue;
        for (const int c : whole_words(code))
            places.push_back({{l, c}, l == last_directive + 1});
    }
    // In each group, a place that is not followed by "(" comes before a call, a constructor or a
    // destructor name.
    auto is_call = [&](const Pos& p)
    {
        const std::string_view t = d.gen.line(p.line);
        size_t e = static_cast<size_t>(p.col) + w.size();
        while (e < t.size() && t[e] == ' ')
            ++e;
        return e < t.size() && t[e] == '(';
    };
    std::stable_sort(places.begin(), places.end(),
        [&](const Place& a, const Place& b)
        {
            if (a.mapped != b.mapped)
                return a.mapped;
            return !is_call(a.pos) && is_call(b.pos);
        });
    const size_t mapped = static_cast<size_t>(
        std::count_if(places.begin(), places.end(), [](const Place& p) { return p.mapped; }));
    if (nth != in_script.end() && mapped == in_script.size())
    {
        // The generated order of the mapped places is the script order. Sort them by position
        // first, then take the one of the same number.
        std::stable_sort(places.begin(), places.begin() + static_cast<long>(mapped),
            [](const Place& a, const Place& b) { return a.pos < b.pos; });
        std::rotate(places.begin(), places.begin() + (nth - in_script.begin()),
            places.begin() + (nth - in_script.begin()) + 1);
    }
    constexpr size_t kMaxAsks = 4;
    for (size_t k = 0; k < places.size() && out.size() < kMaxAsks; ++k)
        out.push_back(places[k].pos);
    return out;
}

json Server::on_locations(const Doc& d, const json& p, const std::string& method)
{
    json out = json::array();
    const Pos here = from_lsp(d.text, p["position"], utf8_);
    const bool is_ref = method == "textDocument/references";
    const std::string here_word = word(d, here);
    if (is_ref && !here_word.empty() && is_script_class(d, here_word))
    {
        // A script class: the places come from the tokens (see class_places).
        auto add = [&](const Doc& doc, const std::set<Pos>& places)
        {
            for (const Pos& s : places)
                out.push_back({{"uri", doc.uri},
                    {"range", script_range(doc, s, {s.line, s.col + static_cast<int>(here_word.size())})}});
        };
        add(d, class_places(d, here_word));
        if (roots_.size())
            for_other_scripts(d, {here_word, ""}, here_word, add);
        return out;
    }
    // The places in the generated text to ask clangd. A word without a map segment (a type name,
    // a base class, a name after "new") has the places of the same word in the generated lines
    // of its script line. The first place that gives an answer wins.
    std::vector<Pos> asks;
    bool by_word = false;
    if (const auto g = to_gen(d, here))
        asks.push_back(*g);
    else if (method == "textDocument/definition")
    {
        asks = gen_word_candidates(d, here);
        by_word = true;
    }
    // The asks of a word share a short time budget: a place that clangd does not answer in time
    // does not hold the editor.
    constexpr int kWordBudgetMs = 3000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWordBudgetMs);
    std::set<std::string> seen;
    for (const Pos& g : asks)
    {
        int timeout_ms = 60000;
        if (by_word)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0)
                break;
            timeout_ms = static_cast<int>(left);
        }
        json params = {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, g)}};
        if (method == "textDocument/references")
            params["context"] = {{"includeDeclaration", true}};
        auto r = clangd_result(method, params, timeout_ms);
        if (!r)
            continue;
        json list = r->is_array() ? *r : json::array({*r});
        for (const json& loc : list)
        {
            json m = map_location(d, loc);
            if (m.is_null() || !seen.insert(m.dump()).second)
                continue;
            out.push_back(std::move(m));
        }
        if (!out.empty())
            break;
    }
    // A symbol word with a place in the generated text (or no generated text at all, because the
    // translation never worked), but no answer, and no host in use (a host that
    // matches, or the nearest host): tell the user one time why F12 cannot find the symbol.
    if (out.empty() && (!asks.empty() || !d.map) && method == "textDocument/definition" && !d.no_host_told
        && is_symbol_word(here_word) && !is_script_class(d, here_word) && !host_of(d.path) && !nearest_host(d.path))
    {
        d.no_host_told = true;
        send_client({{"jsonrpc", "2.0"}, {"method", "window/showMessage"},
            {"params",
                {{"type", 3},
                    {"message",
                        "hot-ls: No script host matches " + d.path
                            + ", so F12 may not find native symbols. Add hot_add_host(...) or hot_script_host(...) in "
                              "CMake and run CMake again."}}}});
    }
    if (asks.empty())
        return out;
    if (method == "textDocument/references" && roots_.size())
    {
        const Pos cur = from_lsp(d.text, p["position"], utf8_);
        const std::string name = word(d, cur);
        if (const auto key = name.empty() ? std::nullopt : key_at(d, cur))
            for_other_scripts(d, *key, name, [&](const Doc& doc, const std::set<Pos>& places)
            {
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
    constexpr int kClassToken = 2; // the index of "class" in semantic_token_types()
    std::set<Pos> out;
    for (const SemanticToken& t : script_tokens(d.text.s, d.facts))
        if (t.type == kClassToken
            && d.text.line(t.line).substr(static_cast<size_t>(t.col), static_cast<size_t>(t.len)) == name)
            out.insert({t.line, t.col});
    return out;
}

bool Server::is_script_class(const Doc& d, const std::string& name) const
{
    return std::any_of(
        d.facts.classes.begin(), d.facts.classes.end(), [&](const hotc::AnalyzedClass& c) { return c.name == name; });
}

std::variant<std::set<Pos>, std::string> Server::symbol_places(
    const Doc& d, Pos cur, const std::string& old_name, Pos* decl)
{
    if (is_script_class(d, old_name))
    {
        if (decl)
            for (const hotc::AnalyzedClass& c : d.facts.classes)
                if (c.name == old_name)
                    *decl = class_name_pos(d, c);
        return class_places(d, old_name);
    }
    const auto g = to_gen(d, {cur.line, word_at(d, cur).first});
    if (!g || !clangd_)
        return std::string("no script symbol at the position");
    const json tdp = {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, *g)}};
    const auto defs = clangd_result("textDocument/definition", tdp);
    const json def_list = defs ? (defs->is_array() ? *defs : json::array({*defs})) : json::array();
    if (def_list.empty())
        return std::string("no script symbol at the position");
    for (const json& loc : def_list)
    {
        const std::string uri =
            loc.contains("targetUri") ? loc.value("targetUri", std::string()) : loc.value("uri", std::string());
        // clangd can answer with the same symbol in the generated file of another script.
        if (uri != d.gen_uri && !gen_docs_.count(uri))
            return std::string("cannot rename a native symbol: " + old_name);
        if (uri == d.gen_uri && map_location(d, loc).is_null())
            return std::string("cannot rename a generated name: " + old_name);
    }
    if (decl)
    {
        *decl = {-1, -1};
        const std::string uri = def_list[0].contains("targetUri") ? def_list[0].value("targetUri", std::string())
                                                                  : def_list[0].value("uri", std::string());
        if (uri == d.gen_uri)
        {
            const json m = map_location(d, def_list[0]);
            *decl = from_lsp(d.text, m["range"]["start"], utf8_);
        }
    }
    json params = tdp;
    params["context"] = {{"includeDeclaration", true}};
    const auto refs = clangd_result("textDocument/references", params);
    std::set<Pos> starts;
    for (const json& loc : refs ? *refs : json::array())
    {
        if (loc.value("uri", std::string()) != d.gen_uri)
            continue;
        const auto s = to_script(d, loc["range"]["start"]);
        if (!s)
            continue;
        // The mapped start can be the start of the expression ("this" of "this.heal"). Take the
        // first whole word "old_name" on the line from there.
        const std::string_view l = d.text.line(s->line);
        for (size_t at = l.find(old_name, static_cast<size_t>(s->col)); at != std::string_view::npos;
            at = l.find(old_name, at + 1))
        {
            const bool left = at == 0 || !is_id_char(l[at - 1]);
            const bool right = at + old_name.size() >= l.size() || !is_id_char(l[at + old_name.size()]);
            if (left && right)
            {
                starts.insert({s->line, static_cast<int>(at)});
                break;
            }
        }
    }
    return starts;
}

std::optional<SymbolKey> Server::symbol_key(const Doc& d, Pos decl, const std::string& name) const
{
    if (decl.line < 0)
    {
        // The declaration is in another script. Take the member of this script, if it is the only one.
        std::optional<SymbolKey> found;
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            for (const hotc::AnalyzedMember& m : c.members)
                if (m.name == name)
                {
                    if (found)
                        return std::nullopt;
                    found = SymbolKey{c.name, name};
                }
        return found;
    }
    for (const hotc::AnalyzedClass& c : d.facts.classes)
    {
        if (c.name == name && c.begin.line - 1 == decl.line)
            return SymbolKey{name, ""};
        for (const hotc::AnalyzedMember& m : c.members)
            if (m.name == name && m.begin.line - 1 == decl.line)
                return SymbolKey{c.name, name};
    }
    return std::nullopt;
}

std::optional<SymbolKey> Server::key_at(const Doc& d, Pos cur)
{
    const std::string name = word(d, cur);
    if (name.empty())
        return std::nullopt;
    Pos decl;
    const auto r = symbol_places(d, cur, name, &decl);
    if (std::holds_alternative<std::string>(r))
        return std::nullopt;
    return symbol_key(d, decl, name);
}

Pos Server::class_name_pos(const Doc& d, const hotc::AnalyzedClass& c) const
{
    // The begin of a class is at the keyword "class". The name comes after it on the line.
    const Pos b = {c.begin.line - 1, c.begin.col - 1};
    const std::string_view l = d.text.line(b.line);
    for (size_t at = l.find(c.name, static_cast<size_t>(b.col)); at != std::string_view::npos;
        at = l.find(c.name, at + 1))
        if ((at == 0 || !is_id_char(l[at - 1]))
            && (at + c.name.size() >= l.size() || !is_id_char(l[at + c.name.size()])))
            return {b.line, static_cast<int>(at)};
    return b;
}

std::optional<Pos> Server::key_position(const Doc& d, const SymbolKey& key) const
{
    for (const hotc::AnalyzedClass& c : d.facts.classes)
    {
        if (c.name != key.cls)
            continue;
        if (key.member.empty())
            return class_name_pos(d, c);
        for (const hotc::AnalyzedMember& m : c.members)
            if (m.name == key.member)
                return Pos{m.begin.line - 1, m.begin.col - 1};
    }
    return std::nullopt;
}

std::vector<std::string> Server::workspace_scripts() const
{
    std::vector<std::string> out;
    for (const std::string& root : roots_)
    {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            std::error_code ec2;
            if (it->is_directory(ec2))
            {
                if ((!name.empty() && name[0] == '.') || name == "node_modules")
                    it.disable_recursion_pending();
                continue;
            }
            if (it->path().extension() == ".hot" && it->is_regular_file(ec2))
                out.push_back(it->path().lexically_normal().string());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Server::for_other_scripts(const Doc& origin, const SymbolKey& key, const std::string& name,
    const std::function<void(const Doc&, const std::set<Pos>&)>& fn)
{
    index_.prune(); // the shard of a file that is gone
    for (const std::string& path : workspace_scripts())
    {
        if (path == origin.path)
            continue;
        std::shared_ptr<Doc> open;
        for (const auto& [uri, doc] : docs_)
            if (doc->path == path)
                open = doc;
        if (open)
        {
            const auto at = key_position(*open, key);
            if (!at || word(*open, *at) != name)
                continue;
            const auto r = symbol_places(*open, *at, name, nullptr);
            if (const auto* places = std::get_if<std::set<Pos>>(&r); places && !places->empty())
                fn(*open, *places);
            continue;
        }
        const auto text = read_file(path);
        if (!text)
            continue;
        Doc tmp;
        tmp.uri = path_to_uri(path);
        tmp.path = path;
        tmp.gen_path = path + ".cpp";
        tmp.gen_uri = path_to_uri(tmp.gen_path);
        tmp.text = Text(*text);
        bool analyzed = false;
        auto analyze_tmp = [&]
        {
            tmp.facts = hotc::analyze(tmp.text.s, translate_options(tmp), session_);
            analyzed = true;
            ++closed_analyses_;
        };
        // The saved shard of the file holds the symbols and the references of the last analyze.
        // The digest holds the import files. The flags of the host also go in, as the one-argument digest.
        const Effective eff = effective_options(path);
        std::string flags_text;
        for (const std::string& f : eff.extra_flags)
            flags_text += f + '\n';
        const std::string digest = SymbolIndex::digest(SymbolIndex::digest(path, *text, eff.include_dirs) + flags_text);
        auto shard = index_.load(path, digest);
        if (!shard)
        {
            if (text->find(name) == std::string::npos)
                continue;
            analyze_tmp();
            shard = shard_of_doc(tmp);
            index_.store(path, digest, *shard);
        }
        IndexClass* cls = shard->find_class(key.cls);
        if (!cls)
            continue;
        std::set<Pos> places;
        if (key.member.empty())
        {
            for (const IndexPos& r : cls->refs)
                places.insert({r.line, r.col});
        }
        else
        {
            IndexMember* m = cls->find_member(key.member);
            if (!m)
                continue;
            if (m->refs)
            {
                for (const IndexPos& r : *m->refs)
                    places.insert({r.line, r.col});
            }
            else
            { // a member needs clangd. The result goes into the shard.
                if (!analyzed)
                    analyze_tmp();
                const auto at = key_position(tmp, key);
                if (!at || tmp.facts.cpp.empty())
                    continue;
                tmp.gen = Text(tmp.facts.cpp);
                tmp.map = std::make_shared<const hotc::SourceMap>(tmp.facts.source_map);
                tmp.gen_script = tmp.text;
                tmp.gen_version = 1;
                send_generated(tmp, tmp.facts.cpp);
                const auto r = symbol_places(tmp, *at, name, nullptr);
                if (const auto* found = std::get_if<std::set<Pos>>(&r))
                    places = *found;
                if (clangd_ && tmp.in_clangd)
                    clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", tmp.gen_uri}}}});
                if (!places.empty())
                {
                    m->refs = std::vector<IndexPos>();
                    for (const Pos& q : places)
                        m->refs->push_back({q.line, q.col});
                    index_.store(path, digest, *shard);
                }
            }
        }
        if (!places.empty())
            fn(tmp, places);
    }
}

IndexShard Server::shard_of_doc(const Doc& d) const
{
    IndexShard shard;
    for (const hotc::AnalyzedClass& c : d.facts.classes)
    {
        IndexClass ic;
        ic.name = c.name;
        const Pos b = class_name_pos(d, c);
        ic.begin = {b.line, b.col};
        for (const Pos& q : class_places(d, c.name))
            ic.refs.push_back({q.line, q.col});
        for (const hotc::AnalyzedMember& m : c.members)
            ic.members.push_back({m.name, m.kind, {m.begin.line - 1, m.begin.col - 1}, std::nullopt});
        shard.classes.push_back(std::move(ic));
    }
    return shard;
}

std::variant<json, std::string> Server::on_rename(const Doc& d, const json& p)
{
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    const std::string old_name = word(d, cur);
    const std::string new_name = p.value("newName", std::string());
    if (old_name.empty())
        return std::string("no symbol at the position");
    if (new_name.empty() || !std::all_of(new_name.begin(), new_name.end(), is_id_char)
        || (new_name[0] >= '0' && new_name[0] <= '9'))
        return std::string("the new name is not an identifier: " + new_name);
    Pos decl;
    const auto r = symbol_places(d, cur, old_name, &decl);
    if (const auto* e = std::get_if<std::string>(&r))
        return *e;
    auto edits_of = [&](const Doc& doc, const std::set<Pos>& places)
    {
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
        for_other_scripts(d, *key, old_name,
            [&](const Doc& doc, const std::set<Pos>& places) { changes[doc.uri] = edits_of(doc, places); });
    return json{{"changes", changes}};
}

json Server::on_document_symbol(const Doc& d)
{
    json out = json::array();
    for (const hotc::AnalyzedClass& c : d.facts.classes)
    {
        const Pos b = {c.begin.line - 1, c.begin.col - 1};
        Pos e = {b.line, b.col + static_cast<int>(c.name.size())};
        json children = json::array();
        for (const hotc::AnalyzedMember& m : c.members)
        {
            int kind = 6; // Method
            if (m.kind == "field")
                kind = 8;
            else if (m.kind == "constructor")
                kind = 9;
            else if (m.kind == "property")
                kind = 7;
            const Pos mb = {m.begin.line - 1, m.begin.col - 1}, me = {m.end.line - 1, m.end.col - 1};
            if (e < me)
                e = me;
            const std::string shown = m.kind == "destructor" && m.name[0] != '~' ? "~" + m.name : m.name;
            children.push_back({{"name", shown}, {"kind", kind}, {"detail", m.kind}, {"range", script_range(d, mb, me)},
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
    for (const SemanticToken& t : script_tokens(d.text.s, d.facts))
    {
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

// A place in a generated file on disk (from "hotc --compile-commands") as a script location.
// The "#line" directives give the script and the line.
json disk_to_script(const std::string& cpp_path, int line)
{
    const auto text = read_file(cpp_path);
    if (!text)
        return nullptr;
    const auto t = line_directive_target(*text, line);
    if (!t)
        return nullptr;
    fs::path f = t->file;
    if (f.is_relative())
    {
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
    if (uri.empty() || dot == std::string::npos)
        return nullptr;
    const std::string cls = script_name.substr(0, dot), member = script_name.substr(dot + 1);
    const hotc::AnalyzeResult* facts = nullptr;
    Text text;
    if (const auto it = docs_.find(uri); it != docs_.end())
    {
        facts = &it->second->facts;
        text = it->second->text;
    }
    else
    {
        const std::string path = uri_to_path(uri);
        const auto t = read_file(path);
        if (!t)
            return nullptr;
        auto& cached = disk_facts_[path];
        if (cached.first != *t)
        {
            Doc tmp;
            tmp.path = path;
            cached = {*t, hotc::analyze(*t, translate_options(tmp), session_)};
        }
        facts = &cached.second;
        text = Text(*t);
    }
    for (const hotc::AnalyzedClass& c : facts->classes)
    {
        if (c.name != cls)
            continue;
        for (const hotc::AnalyzedMember& m : c.members)
        {
            if (m.name != member)
                continue;
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
    if (!r || !r->is_array())
        return out;
    for (const json& s : *r)
    {
        json loc = s.value("location", json());
        const std::string uri = loc.value("uri", std::string());
        const std::string name = s.value("name", std::string());
        const std::string container = s.value("containerName", std::string());
        const std::string script_name = hotc::rewrite_generated_names(name);
        const std::string script_container = hotc::rewrite_generated_names(container);
        // A symbol of a generated file is a script symbol when its name, or the name of its
        // container (a state struct), is a generated name. Else it is a part of the plumbing.
        const bool script = script_name != name || (script_container != container && name.compare(0, 5, "SLOT_") != 0);
        json mapped;
        bool generated = false;
        if (const auto it = gen_docs_.find(uri); it != gen_docs_.end())
        {
            generated = true;
            if (script)
                mapped = map_location(*it->second, loc);
        }
        else if (uri.size() > 4 && uri.compare(uri.size() - 4, 4, ".cpp") == 0)
        {
            // A generated file of the workspace from the background index.
            const std::string path = uri_to_path(uri);
            const auto text = read_file(path);
            generated = text && text->find("// Generated by hotc") == 0;
            if (generated && script)
                mapped = disk_to_script(path, loc.value("/range/start/line"_json_pointer, 0));
        }
        if (generated && !mapped.is_null())
        {
            // A field of a state struct: the container gives the class.
            const bool field = script_name.find('.') == std::string::npos && script_container != container;
            mapped = script_member_location(
                mapped.value("uri", std::string()), field ? script_container + "." + script_name : script_name);
        }
        if (generated && mapped.is_null())
            continue;
        if (mapped.is_null())
            mapped = loc; // a native symbol
        json item = s;
        item["name"] = script_name;
        if (s.contains("containerName"))
            item["containerName"] = script_container;
        item["location"] = mapped;
        out.push_back(std::move(item));
    }
    return out;
}

json Server::on_to_script(const json& p)
{
    const std::string uri = p.value("/textDocument/uri"_json_pointer, std::string());
    const json pos = p.value("position", json::object());
    if (const auto it = gen_docs_.find(uri); it != gen_docs_.end())
    {
        const Doc& d = *it->second;
        if (const auto s = to_script(d, pos))
            return {{"uri", d.uri}, {"range", script_range(d, *s, *s)}};
        if (json imp = imported_location(d, pos, false); !imp.is_null())
            return imp;
        const auto t = line_directive_target(d.gen.s, pos.value("line", 0));
        if (!t || t->file != d.path)
            return nullptr;
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
    if (method.empty())
        return; // a response to a request that the server did not send

    if (method == "initialize")
        return reply(id, on_initialize(p));
    if (method == "shutdown")
    {
        shutdown_ = true;
        if (clangd_)
            clangd_->stop();
        return reply(id, nullptr);
    }
    if (method == "textDocument/didOpen")
    {
#ifdef _WIN32
        refresh_cdb_copy();
#endif
        const json& td = p["textDocument"];
        auto d = std::make_shared<Doc>();
        d->uri = td.value("uri", std::string());
        d->path = uri_to_path(d->uri);
        if (!is_served_path(d->path))
            return; // a header or other file: the C++ tools own it
        d->cpp = fs::path(d->path).extension() == ".cpp";
        if (!d->cpp)
        {
            d->gen_path = d->path + ".cpp";
            d->gen_uri = path_to_uri(d->gen_path);
        }
        d->text = Text(td.value("text", std::string()));
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            docs_[d->uri] = d;
            if (!d->cpp)
                gen_docs_[d->gen_uri] = d;
        }
        d->host = host_of(d->path);
        refresh_hosts(d.get());
        log_host(*d, d->host);
        return update(*d);
    }
    if (method == "textDocument/didChange")
    {
        auto d = doc_of(p);
        if (!d)
            return;
        const json changes = p.value("contentChanges", json::array());
        if (changes.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            d->text = Text(changes.back().value("text", std::string()));
        }
        const auto host_before = d->host;
        refresh_hosts(d.get());
        if (!same_host(host_before, d->host))
            log_host(*d, d->host);
        return update(*d);
    }
    if (method == "textDocument/didClose")
    {
        auto d = doc_of(p);
        if (!d)
            return;
        if (clangd_ && d->in_clangd)
            clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", d->gen_uri}}}});
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            docs_.erase(d->uri);
            if (!d->cpp)
                gen_docs_.erase(d->gen_uri);
        }
        if (d->cpp && !d->cpp_published)
            return; // an ignored ".cpp" document: the server never published for it
        send_client({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", d->uri}, {"diagnostics", json::array()}}}});
        for (const std::string& uri : d->import_uris)
            send_client({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
                {"params", {{"uri", uri}, {"diagnostics", json::array()}}}});
        return;
    }
    if (method == "workspace/didChangeWatchedFiles")
        return on_watched_files(p);
    if (!is_request)
        return; // initialized, $/cancelRequest and other notifications
    if (method == "workspace/symbol")
        return reply(id, on_workspace_symbol(p));
    if (method == "hot/indexStats")
        return reply(id,
            {{"analyzed", closed_analyses_}, {"loaded", index_.loads()}, {"stored", index_.stores()},
                {"dir", index_.dir()}, {"completions", completion_translations_}});
    if (method == "hot/toScript")
        return reply(id, on_to_script(p));

    static const std::set<std::string> doc_methods = {"textDocument/completion", "textDocument/hover",
        "textDocument/definition", "textDocument/references", "textDocument/documentSymbol",
        "textDocument/semanticTokens/full", "textDocument/rename", "textDocument/signatureHelp",
        "textDocument/formatting", "textDocument/codeAction"};
    if (!doc_methods.count(method))
        return reply_error(id, kMethodNotFound, "unknown method: " + method);
    auto d = doc_of(p);
    if (!d)
    {
        // A file that hot-ls does not serve (a header, for example) gets an empty answer.
        if (!is_served_path(uri_to_path(p.value("/textDocument/uri"_json_pointer, std::string()))))
            return reply(id, nullptr);
        return reply_error(id, kInvalidParams, "the document is not open");
    }
    if (d->cpp)
        return reply(id, nullptr); // a ".cpp" document: clangd owns the features, hot-ls only publishes errors
    if (method == "textDocument/completion")
        return reply(id, on_completion(*d, p));
    if (method == "textDocument/hover")
        return reply(id, on_hover(*d, p));
    if (method == "textDocument/signatureHelp")
        return reply(id, on_signature_help(*d, p));
    if (method == "textDocument/formatting")
        return reply(id, on_formatting(*d, p));
    if (method == "textDocument/codeAction")
        return reply(id, on_code_action(*d, p));
    if (method == "textDocument/definition" || method == "textDocument/references")
        return reply(id, on_locations(*d, p, method));
    if (method == "textDocument/documentSymbol")
        return reply(id, on_document_symbol(*d));
    if (method == "textDocument/semanticTokens/full")
        return reply(id, on_semantic_tokens(*d));
    auto r = on_rename(*d, p);
    if (auto* e = std::get_if<std::string>(&r))
        return reply_error(id, kRequestFailed, *e);
    return reply(id, std::get<json>(r));
}

int Server::run()
{
    MessageReader reader(in_);
    std::string body;
    while (reader.read(body))
    {
        const json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded() || !msg.is_object())
            continue;
        if (msg.value("method", std::string()) == "exit")
            break;
        try
        {
            handle(msg);
        }
        catch (const std::exception& e)
        {
            if (msg.contains("id"))
                reply_error(msg["id"], kRequestFailed, std::string("internal error: ") + e.what());
        }
    }
    if (clangd_)
        clangd_->stop();
    return shutdown_ ? 0 : 1;
}

} // namespace

std::optional<ScriptLine> line_directive_target(const std::string& cpp, int line)
{
    static const std::regex re(R"(^\s*#\s*line\s+(\d+)\s+"((?:[^"\\]|\\.)*)\")");
    const std::vector<size_t> starts = line_starts(cpp);
    // Walk back to the nearest directive. A directive that names the .cpp file itself marks
    // translator text: the last script directive before it gives the line then.
    bool in_cpp_text = false;
    for (int l = std::min(line, static_cast<int>(starts.size()) - 1); l >= 0; --l)
    {
        const std::string_view t = line_text(cpp, starts, l);
        if (t.find("#") == std::string_view::npos || t.find("line") == std::string_view::npos)
            continue;
        std::cmatch m;
        if (!std::regex_search(t.data(), t.data() + t.size(), m, re))
            continue;
        std::string file;
        for (size_t i = 0; i < static_cast<size_t>(m[2].length()); ++i)
        {
            const char c = m[2].first[i];
            if (c == '\\' && i + 1 < static_cast<size_t>(m[2].length()))
                ++i;
            file += m[2].first[i];
        }
        const bool is_cpp = file.size() > 4 && file.compare(file.size() - 4, 4, ".cpp") == 0;
        if (is_cpp)
        {
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
#ifdef _WIN32
    // The messages hold "\r\n". The text mode of the C runtime changes them.
    _setmode(in_fd, _O_BINARY);
    _setmode(out_fd, _O_BINARY);
#else
    signal(SIGPIPE, SIG_IGN); // a write to a clangd that stopped gives an error, not a signal
#endif
    Server server(in_fd, out_fd, options);
    return server.run();
}

const char* usage_text()
{
    return "usage: hot-ls [--stdio] [--clangd path] [-I dir]... [--resource-dir dir] [--cxxflag flag]...\n"
           "              [--compile-commands-dir dir] [--index-dir dir]\n"
           "              [--hosts-dir dir] [--clangd-log file]\n"
           "  --clangd path      the clangd binary (default: clangd from PATH). \"\" turns clangd off.\n"
           "  -I dir             an include dir for the script imports and for clangd.\n"
           "  --compile-commands-dir dir\n"
           "                     the directory of compile_commands.json from hotc --compile-commands.\n"
           "                     clangd runs its background index on it.\n"
           "  --index-dir dir    the directory of the saved symbol index (default:\n"
           "                     .cache/hot/index in the first workspace folder).\n"
           "  --hosts-dir dir    the directory of the host files from hot_script_host (default:\n"
           "                     .cache/hot/hosts in each workspace folder).\n";
}

bool parse_arguments(const std::vector<std::string>& args, ServerOptions& out, std::string& error)
{
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        auto next = [&](std::string& v)
        {
            if (i + 1 >= args.size())
            {
                error = "missing value after " + a;
                return false;
            }
            v = args[++i];
            return true;
        };
        std::string v;
        if (a == "--stdio")
            continue;
        if (a == "--clangd")
        {
            if (!next(out.clangd))
                return false;
        }
        else if (a == "-I")
        {
            if (!next(v))
                return false;
            out.include_dirs.push_back(v);
        }
        else if (a.compare(0, 2, "-I") == 0)
        {
            out.include_dirs.push_back(a.substr(2));
        }
        else if (a == "--resource-dir")
        {
            if (!next(out.resource_dir))
                return false;
        }
        else if (a == "--cxxflag")
        {
            if (!next(v))
                return false;
            out.extra_flags.push_back(v);
        }
        else if (a == "--compile-commands-dir")
        {
            if (!next(out.compile_commands_dir))
                return false;
        }
        else if (a == "--index-dir")
        {
            if (!next(out.index_dir))
                return false;
        }
        else if (a == "--hosts-dir")
        {
            if (!next(out.hosts_dir))
                return false;
        }
        else if (a == "--clangd-log")
        {
            if (!next(out.clangd_log))
                return false;
        }
        else
        {
            error = "unknown argument: " + a;
            return false;
        }
    }
    return true;
}

} // namespace hot::ls
