// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "server.h"

#include "attributes.h"
#include "clangd_process.h"
#include "completion_context.h"
#include "format.h"
#include "hosts.h"
#include "jsonrpc.h"
#include "long_path.h"
#include "script_text.h"
#include "semantic.h"
#include "hot/clang_files_backend.h"
#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/paths.h"
#include "symbol_index.h"
#include "workspace_index.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <cctype>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <cstring>
#include <regex>
#include <set>
#include <signal.h>
#include <sstream>
#include <tuple>
#include <variant>
#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

#include "hot/runtime/process.h"

namespace fs = std::filesystem;

namespace hot::ls
{

static std::atomic<int> g_import_parses{0};

int import_parse_count()
{
    return g_import_parses;
}

using json = nlohmann::json;

namespace
{

// The name that the completion puts in place of the word at the cursor (see on_completion).
const std::string kHole = "__hot_complete";

// LSP error codes.
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kRequestFailed = -32803;
constexpr int kRequestCancelled = -32800;
constexpr int kContentModified = -32801;

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
#endif
    // clangd writes the canonical form: the long names on Windows, no symbolic links elsewhere.
    r = long_path(r);
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
    // The real path without symbolic links, as clangd writes it (long_path.h).
    for (char ch : long_path(path))
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
    int facts_version = 0; // grows at each new "facts"
    // The table of the resolved names of "facts" (Server::file_refs) and its facts_version.
    mutable std::shared_ptr<const FileRefs> table;
    mutable int table_version = -1;
    // The field names of each class when the document opened, in declaration order. The rename
    // warning compares them with the current fields. A save does not refresh them.
    std::map<std::string, std::vector<std::string>> open_fields;
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
    // The header that clangd has for the shell of the unit (Server::send_generated): its path and the hash of its text.
    std::string shell_path;
    size_t shell_hash = 0;
    std::string shell_old; // the header of the last text, removed when the new one is open
    bool shell_active = false; // the unit of clangd includes shell_path
    bool shell_changed = false; // the header changed: the next send reopens the document
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

// The string member of a JSON object, or an empty string. A missing member, a null and any other
// type give the empty string, so a forged request cannot throw.
static std::string json_str(const json& j, const char* key)
{
    if (!j.is_object())
        return {};
    const auto f = j.find(key);
    return f != j.end() && f->is_string() ? f->get<std::string>() : std::string();
}

// The valid entry of a path in an index snapshot, or null when the snapshot has none.
static std::shared_ptr<const Entry> entry_in(const IndexSnapshot& snap, const std::string& path)
{
    const auto it = snap.entries.find(fs::path(path).lexically_normal().string());
    return it != snap.entries.end() ? it->second : nullptr;
}

// The valid entry of a closed path, or null. An entry with a text that is not the text on disk
// ("disk") is dropped from the snapshot: the caller then uses the disk text.
static std::shared_ptr<const Entry> checked_entry(IndexSnapshot& snap, const std::string& path, const std::string& disk)
{
    std::shared_ptr<const Entry> e = entry_in(snap, path);
    if (!e)
        return nullptr;
    if (disk == e->text)
        return e;
    snap.entries.erase(fs::path(path).lexically_normal().string());
    return nullptr;
}

// A method or a constructor of a document, with the byte range of its declaration. A destructor is
// not in the call hierarchy.
struct Callable
{
    const hotc::AnalyzedClass* cls;
    const hotc::AnalyzedMember* m;
    size_t begin, end;
};

std::vector<Callable> callables_of(const Doc& d)
{
    std::vector<Callable> out;
    for (const hotc::AnalyzedClass& c : d.facts.classes)
        for (const hotc::AnalyzedMember& m : c.members)
            if (m.kind == "method" || m.kind == "constructor")
                out.push_back({&c, &m, d.text.offset(m.begin.line - 1, m.begin.col - 1),
                    d.text.offset(m.end.line - 1, m.end.col - 1)});
    return out;
}

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

static bool has_word(std::string_view text, const std::string& name);

// The state of one native rename. The helpers of Server::native_rename share it.
// True for the name of a file that "hotc --compile-commands" writes: "<stem>.g<N>.cpp".
bool is_hotc_output(const std::string& path)
{
    const std::string name = fs::path(path).filename().string();
    const std::string tail = ".cpp";
    if (name.size() < 8 || name.compare(name.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    size_t end = name.size() - tail.size(), b = end;
    while (b > 0 && std::isdigit(static_cast<unsigned char>(name[b - 1])))
        --b;
    return b < end && b >= 2 && name[b - 1] == 'g' && name[b - 2] == '.';
}

struct NativeRenameJob
{
    std::string old_name, new_name;
    // The start of the refusal texts.
    std::string what;
    // The script where the rename starts.
    const Doc* start = nullptr;
    // All the clangd calls of one rename share this time.
    std::chrono::steady_clock::time_point deadline;
    // The edits by uri.
    json changes = json::object();
    // The text of the real C++ files. A file that is not readable has no text.
    std::map<std::string, std::shared_ptr<Text>> cpp_texts;
    // The lines of a script without the comments and the strings.
    std::map<const Doc*, std::vector<std::string>> code_lines;
    // The declaration places of the symbol: the uri, the line and the character.
    std::vector<std::tuple<std::string, int, int>> target;
    // The scripts of the generated files that an answer changes (phase 2).
    std::vector<std::string> extra_script_paths;
};

namespace
{
// The identity of this build: the size and the mtime of the running executable (HOT-33).
std::string executable_build_id()
{
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    const fs::path exe;
#else
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
#endif
    if (exe.empty())
        return "unknown";
    const auto size = fs::file_size(exe, ec);
    const auto time = fs::last_write_time(exe, ec).time_since_epoch().count();
    return std::to_string(size) + ':' + std::to_string(time);
}
} // namespace

// The split of the generated unit in a shell header and the rest. The environment variable HOT_LS_SPLIT_SHELL=0
// turns it off: clangd then gets one unit.
// HOT_LS_TIMING=<file> appends the phases of a completion to the file.
struct PhaseTimer
{
    const char* label;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit PhaseTimer(const char* l) : label(l) {}
    ~PhaseTimer()
    {
        static const char* const file = std::getenv("HOT_LS_TIMING");
        if (file)
            if (FILE* f = std::fopen(file, "a"))
            {
                std::fprintf(f, "PHASE %-22s %7.1f ms\n", label,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                std::fclose(f);
            }
    }
};

static bool lite_doc_enabled()
{
    const char* e = std::getenv("HOT_LS_LITE_DOC");
    return e && std::string(e) == "1"; // off by default (HOT-43): the corpus check decides
}

static bool split_shell_enabled()
{
    const char* e = std::getenv("HOT_LS_SPLIT_SHELL");
    return !(e && std::string(e) == "0");
}

class Server
{
public:
    Server(int in, int out, ServerOptions o)
        : in_(in)
        , out_(out)
        , opt_(std::move(o))
    {
        if (opt_.build_id.empty())
            opt_.build_id = executable_build_id();
        read_compile_commands();
    }
    ~Server()
    {
        windex_.stop(); // before the members die and before clangd stops
        if (clangd_)
            clangd_->stop();
        // The stop of clangd ends the requests of the workers.
        for (TokenWorker& w : token_workers_)
            if (w.thread.joinable())
                w.thread.join();
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

    // ---- the reader thread, the queue and the cancel (HOT-36)
    // A request that the client cancels leaves its handler with this exception. The run loop
    // replies with the code -32800.
    struct RequestCancelled
    {
    };
    // The id of the request that the current thread runs. Empty for a thread with no request.
    static inline thread_local std::string request_key_;
    // True when the current thread is the slow worker. No slow worker exists yet.
    static inline thread_local bool is_slow_worker = false;
    // The epoch that the slow worker saw at the start of its request.
    static inline thread_local uint64_t request_epoch_ = 0;
    // True when the client cancelled the request that the current thread runs.
    bool cancelled()
    {
        if (!any_cancel_.load() || request_key_.empty())
            return false;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return cancelled_ids_.count(request_key_) != 0;
    }
    void check_cancel()
    {
        if (cancelled())
            throw RequestCancelled{};
    }
    // The state lock. The thread that holds it owns the state of the server.
    class StateLock
    {
    public:
        // The state thread waits as a fast waiter: it goes before the slow worker.
        void lock_fast()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++fast_waiting_;
            cv_.wait(lock, [this] { return !busy_; });
            --fast_waiting_;
            busy_ = true;
        }
        // The slow worker waits until no fast waiter exists.
        void lock_slow()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !busy_ && fast_waiting_ == 0; });
            busy_ = true;
        }
        void unlock()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
            }
            cv_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        bool busy_ = false;
        int fast_waiting_ = 0;
    };
    // The slow worker releases the state lock for a long wait. On the state thread it does nothing.
    class Yield
    {
    public:
        explicit Yield(Server& s) : lock_(is_slow_worker ? &s.state_lock_ : nullptr)
        {
            if (lock_)
                lock_->unlock();
        }
        ~Yield()
        {
            if (lock_)
                lock_->lock_slow();
        }
        Yield(const Yield&) = delete;
        Yield& operator=(const Yield&) = delete;

    private:
        StateLock* lock_;
    };
    // A request of the slow worker leaves its handler with this exception when the state changed.
    // The run loop replies with the code -32801.
    struct StaleRequest
    {
    };
    void check_epoch()
    {
        if (is_slow_worker && epoch_.load() != request_epoch_)
            throw StaleRequest{};
    }
    StateLock state_lock_;
    std::atomic<uint64_t> epoch_{0}; // grows at each client notification and at each send_generated
    void run_request(const json& msg);
    void reader_loop();
    // Takes the next message. Joins the later didChange of the same uri into the last one.
    bool next_message(json& out);
    // The id of the message as a key. Empty for a message without an id.
    static std::string id_key(const json& msg)
    {
        return msg.contains("id") ? msg["id"].dump() : std::string();
    }
    void finish_request(const std::string& key);

    static constexpr size_t kMaxCancelled = 1024;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<json> queue_;
    bool queue_closed_ = false;
    std::set<std::string> pending_ids_;   // the ids of the requests that are queued or run
    std::set<std::string> cancelled_ids_; // the ids that the client cancelled
    std::atomic<bool> any_cancel_{false};
    std::thread::id state_thread_;

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
    hotc::TranslateOptions translate_options(const Doc& d, bool with_preludes = true) const
    {
        hotc::TranslateOptions o;
        o.script_name = d.path;
        o.generation = 1;
        o.resolve_refs = true; // the table of the resolved names (ref_resolver.h)
        o.split_shell = split_shell_enabled();
        const Effective e = effective_options(d.path);
        o.include_dirs = e.include_dirs;
        o.resource_dir = e.resource_dir;
        o.std_version = e.std_version;
        // The parse of the native headers sees the macros of the host, as the compile does.
        for (const std::string& f : e.extra_flags)
            if (f.rfind("-D", 0) == 0 || f.rfind("-U", 0) == 0)
                o.defines.push_back(f);
        o.format_lib = hot::format_lib_name();
        o.view_type = hot::view_type_name();
        // The preludes of the host, as at run time. A prelude file that is open is analyzed
        // as a script, so it does not also get its own text as a prelude.
        if (with_preludes)
            for (const std::string& p : prelude_files(e, d.path))
                if (const auto text = read_file(p))
                o.preludes.emplace_back(p, *text);
        return o;
    }
    // ---- hosts
    struct Effective
    {
        std::vector<std::string> include_dirs, extra_flags;
        std::string resource_dir;
        std::string std_version; // the "std" of the host file (empty: the standard of the build)
        std::vector<std::string> preludes; // the prelude files: of the host, else of the options
        std::string prelude_suffix; // the suffix of the user preludes of the script
    };
    // The prelude files of a host without the script itself. The paths only, no text.
    static std::vector<std::string> prelude_files(const Effective& e, const std::string& script)
    {
        std::vector<std::string> out;
        for (const std::string& p : e.preludes)
            if (fs::path(p).lexically_normal() != fs::path(script).lexically_normal())
                out.push_back(p);
        return out;
    }
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
                const fs::path root = utf8_path(r).lexically_normal();
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
            e.preludes = host->preludes;
        }
        if (e.preludes.empty())
            e.preludes = opt_.preludes;
        // The user preludes: the files with the suffix in the script dirs, sorted by path, after
        // the preludes of the host. The search runs for each analysis, so a new file works at once.
        const std::string suffix = host && !host->prelude_suffix.empty() ? host->prelude_suffix : opt_.prelude_suffix;
        e.prelude_suffix = suffix;
        if (!suffix.empty())
        {
            std::vector<std::string> found;
            for (const std::string& dir : host ? host->script_dirs : roots_)
            {
                std::error_code ec;
                fs::recursive_directory_iterator it(utf8_path(dir), fs::directory_options::skip_permission_denied, ec);
                for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    const std::string name = path_utf8(it->path().filename());
                    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0
                        && it->is_regular_file(ec))
                        found.push_back(path_utf8(it->path().lexically_normal()));
                }
            }
            std::sort(found.begin(), found.end());
            for (const std::string& f : found)
                if (std::find(e.preludes.begin(), e.preludes.end(), f) == e.preludes.end())
                    e.preludes.push_back(f);
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
            && a->include_dirs == b->include_dirs && a->cxx_flags() == b->cxx_flags() && a->preludes == b->preludes;
    }
    void load_hosts();
    void log_info(const std::string& text)
    {
        send_client({{"jsonrpc", "2.0"}, {"method", "window/logMessage"},
            {"params", {{"type", 3}, {"message", "hot-ls: " + text}}}});
    }
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
    void refresh_hosts(Doc* current, bool force = false);
    // The notification workspace/didChangeWatchedFiles: new, changed and deleted files on disk.
    void on_watched_files(const json& p);
    void update(Doc& d);
    void update_cpp(Doc& d);
    void send_generated(Doc& d, const std::string& cpp);
    std::string unit_for_clangd(Doc& d, const std::string& cpp);
    // The lite document (HOT-43): the uri that hover, definition and completion ask for the place "gen_line0"
    // (a line of "full", the generated text). It has the bodies of the other own functions as empty lines. The
    // full uri when there is no lite document for the place.
    std::string lite_uri(const Doc& d, const std::string& full, int gen_line0, int wait_ms = kNativeBudgetMs);
    std::string shell_dir_;
    const std::string& shell_dir();
    void remove_shell(Doc& d)
    {
        std::error_code ec;
        for (std::string* p : {&d.shell_path, &d.shell_old})
            if (!p->empty())
                fs::remove(*p, ec);
        d.shell_path.clear();
        d.shell_old.clear();
        d.shell_hash = 0;
        d.shell_active = false;
    }
    struct LiteState
    {
        bool open = false;
        size_t text_hash = 0, shell_hash = 0;
        bool ready = false; // the AST of the lite document is there
        int version = 0;
        std::string uri; // the uri of the lite document
        unsigned long long used = 0; // the last use (the oldest document is closed first)
    };
    static constexpr size_t kMaxLiteDocs = 2;
    unsigned long long lite_tick_ = 0;
    std::atomic<int> lite_hits_{0}, lite_fallbacks_{0}, lite_mismatches_{0};
    static bool lite_check_enabled()
    {
        const char* e = std::getenv("HOT_LS_LITE_CHECK");
        return e && std::string(e) == "1";
    }
    // HOT_LS_LITE_CHECK=1: the same request goes to the full document and the answers are compared.
    // A difference is a log message and a count (hot/indexStats "liteMismatch").
    void lite_check(const char* method, const json& params, const Doc& d, const json& lite_answer, const char* what);
    std::string lite_uri_impl(const Doc& d, const std::string& full, int gen_line0, int wait_ms);
    std::map<std::string, LiteState> lite_; // by the gen uri
    // The caller holds lite_mutex_.
    void drop_lite_locked(std::map<std::string, LiteState>::iterator it)
    {
        if (clangd_)
        {
            if (it->second.open)
                clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", it->second.uri}}}});
            clangd_->clear_uri_alias(it->second.uri);
        }
        lite_.erase(it);
    }
    void close_lite(const Doc& d)
    {
        std::lock_guard<std::mutex> lock(lite_mutex_);
        const auto it = lite_.find(d.gen_uri);
        if (it != lite_.end())
            drop_lite_locked(it);
    }
    std::mutex lite_mutex_;
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
    void wait_for_preamble_index(int limit_ms = 5000)
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(limit_ms);
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
            Yield yield(*this);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // The short time budget of the clangd requests of one editor request (hover, F12, inlay hints,
    // call hierarchy, semantic tokens). A slow clangd then does not hold the editor.
    static constexpr int kNativeBudgetMs = 3000;
    // The wait for the first preamble of a generated document.
    static constexpr int kPreambleWaitMs = 60000;
    // The wait of a completion for the AST and the answer on the patched text, after the preamble.
    static constexpr int kCompletionAstMs = 5000;

    // Before its first preamble, clangd answers a request that needs the AST only after the
    // preamble. On a slow machine the first preamble takes longer than kNativeBudgetMs. So the first
    // native request of a generated document waits for the preamble and the index, outside the
    // budget, the same as completion. Each document waits one time: an edit that makes a new
    // preamble does not stop the answers, because clangd uses the old preamble until the new one
    // is ready. A new open of the document in clangd removes the mark (preamble_ready_), so the
    // next request waits again.
    void wait_for_native_ready(const Doc& d)
    {
        if (!clangd_ || !clangd_->running() || !d.in_clangd)
            return;
        if (!preamble_ready_.insert(d.gen_uri).second)
            return;
        try
        {
            clangd_result("textDocument/documentSymbol", {{"textDocument", {{"uri", d.gen_uri}}}}, kPreambleWaitMs);
            wait_for_preamble_index();
        }
        catch (...)
        {
            preamble_ready_.erase(d.gen_uri); // the preamble is not there: the next request waits again
            throw;
        }
    }

    // The same wait, but with a deadline. The mark is set only when the document is ready before
    // the deadline, so a later request waits again. Gives true when the document is ready.
    bool wait_for_native_ready_until(const Doc& d, std::chrono::steady_clock::time_point deadline)
    {
        using clock = std::chrono::steady_clock;
        auto left = [&] { return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count()); };
        if (!clangd_ || !clangd_->running() || !d.in_clangd)
            return true;
        if (preamble_ready_.count(d.gen_uri))
            return true;
        if (left() <= 0 || !clangd_result("textDocument/documentSymbol", {{"textDocument", {{"uri", d.gen_uri}}}}, left()))
            return false;
        wait_for_preamble_index(std::max(0, left()));
        preamble_ready_.insert(d.gen_uri); // the preamble is there, also when the time is over
        return left() > 0;
    }

    // Every request to clangd goes through here: it is a yield point of the slow worker.
    json clangd_request(const std::string& method, json params, int timeout_ms)
    {
        Yield yield(*this);
        return clangd_->request(method, std::move(params), timeout_ms);
    }

    std::optional<json> clangd_result(const std::string& method, json params, int timeout_ms = 60000)
    {
        if (!clangd_ || !clangd_->running())
            return std::nullopt;
        json r = clangd_request(method, std::move(params), timeout_ms);
        if (!r.is_object() || !r.contains("result") || r["result"].is_null())
            return std::nullopt;
        return r["result"];
    }

    // ---- features
    json on_initialize(const json& p);
    // The notification workspace/didChangeWorkspaceFolders.
    void on_workspace_folders(const json& p);
    json on_completion(const Doc& d, const json& p);
    json using_completion(const Doc& d, Pos cur, size_t off, size_t line_begin);
    json on_hover(const Doc& d, const json& p);
    // True when the table of the script is not usable (degraded mode).
    static bool has_no_table(const Doc& d)
    {
        return !d.facts.errors.empty() || (d.facts.refs.empty() && !d.facts.ok);
    }
    json on_signature_help(const Doc& d, const json& p);
    json on_formatting(const Doc& d, const json& p);
    json on_range_formatting(const Doc& d, const json& p);
    json on_code_action(const Doc& d, const json& p);
    json on_locations(const Doc& d, const json& p, const std::string& method);
    std::vector<Pos> gen_word_candidates(const Doc& d, Pos script_pos) const;
    // The places in the generated text to ask clangd about a script position. "by_word" is set when
    // the position has no map segment and the places come from the word.
    std::vector<Pos> gen_asks(const Doc& d, Pos script_pos, bool& by_word) const;
    // The script lines (in gen_script) of the generated lines, or -1 for the text of the translator.
    std::vector<int> gen_script_lines(const Doc& d) const;
    // The script place of a use of a word with no map segment, by the k-th use rule.
    std::optional<Pos> script_use_of_gen_word(
        const Doc& d, const std::vector<int>& script_lines, int gl, int gcol, const std::string& word) const;
    json on_document_symbol(const Doc& d);
    json on_semantic_tokens(const Doc& d);
    json on_inlay_hint(const Doc& d, const json& p);
    json on_code_lens(const Doc& d);
    json on_document_link(const Doc& d);
    json on_prepare_call_hierarchy(const Doc& d, const json& p);
    json on_call_hierarchy(const std::string& method, const json& p);
    json call_item(const Doc& d, const Callable& c) const;
    json use_range(const Doc& d, const NameUse& u) const;
    // The calls with the name "name" in the methods and constructors of one document, as the
    // "incomingCalls" entries.
    json incoming_by_name(const Doc& d, const std::string& name) const;
    // The native items (clangd) of a call hierarchy for the word at a script position.
    json native_call_items(const Doc& d, Pos here, int budget_ms);
    json native_item(const Doc& d, json item) const;
    bool is_generated_uri(const std::string& uri) const;
    json on_native_call_hierarchy(bool incoming, const json& data);
    // The parameter names of the native callee of a call, from the signature help of clangd.
    std::vector<std::string> native_param_names(const Doc& d, const CallSite& site, size_t arg_count, int budget_ms);
    json on_workspace_symbol(const json& p);
    // The script symbols of the .hot files of the workspace folders. A closed file counts only if
    // the index has a valid shard for it. This request never analyzes a closed file.
    void map_clangd_symbols(const json& symbols, json& out);
    void add_script_symbols(const std::string& query, json& out);
    // The digest of a closed script for the index.
    std::string shard_digest(const std::string& path, const std::string& text);
    // Stores the shard of a document that closes, if the text is the text on disk.
    void store_closed_shard(const Doc& d);
    json script_member_location(const std::string& uri, const std::string& script_name);
    // The analyze facts and the text of the script "uri": an open document or the file on disk.
    // Null when the file is not readable.
    const hotc::AnalyzeResult* script_facts(const std::string& uri, Text& text);
    json on_to_script(const json& p);
    // Gives the edit, or an error message.
    std::variant<json, std::string> on_rename(const Doc& d, const json& p);
    // Gives the range of the word that a rename changes, or an error message.
    std::variant<json, std::string> on_prepare_rename(const Doc& d, const json& p);
    // The rename of an imported C++ symbol through clangd (see doc/tools-ls.md). "cur" is the
    // place of the word "old_name" in the script. Gives the edit, or an error message. "no_symbol"
    // is the message when the word is not a symbol that clangd knows.
    std::variant<json, std::string> native_rename(
        const Doc& d, Pos cur, const std::string& old_name, const std::string& new_name, const std::string& no_symbol,
        bool prepare);
    // The helpers of native_rename. Each gives a refusal text, or an empty string when it is done.
    // The edits of one script, mapped to script places and merged in the job.
    std::string map_script_edits(NativeRenameJob& job, const Doc& o, const json& edits);
    // The edits of a real C++ file: checked, then merged in the job.
    std::string add_cpp_edits(NativeRenameJob& job, const Doc& d, const std::string& uri, const json& edits);
    // The edits of one answer of clangd for the script "asked".
    std::string add_answer(NativeRenameJob& job, const Doc& d, const Doc& asked, const json& result);
    // True when the range of a real C++ file holds exactly the old name.
    bool is_old_name(NativeRenameJob& job, const std::string& uri, const json& range);
    std::string rename_other_scripts(NativeRenameJob& job, const Doc& d);
    void native_uses_in_scripts(const Doc& d, Pos ask, const std::string& word_name, json& out);
    // True when a stored table (cache or shard, no analyze) of the closed script shows that the
    // script has no code word "word" that is not a resolved script ref. False when no table is available.
    bool table_lacks_word(const std::string& path, const std::string& text, const std::string& word);
    // clangd renames the class name only. This adds the names of the constructors and the
    // destructor: in the class body, and in the out-of-class definitions ("Old::Old(").
    void add_ctor_edits(NativeRenameJob& job);
    // True when the file is in a workspace folder.
    bool in_workspace(const std::string& path) const;
    // The host for the scope rule of a native edit: the host of the document path, else the
    // nearest host when the document is in a workspace folder, else none.
    std::shared_ptr<const Host> scope_host(const Doc& d) const;
    // The reason why a native rename must not edit the file, or an empty string when it may.
    std::string native_edit_refusal(const Doc& d, const std::string& path) const;
    // The tokens of the native names of a document: clangd classifies the generated text. The
    // names that the script tokens did not classify get a token.
    std::vector<SemanticToken> native_tokens(const Doc& d, const std::vector<SemanticToken>& known);
    // The tokens (with the names) that the answer "data" of clangd gives for the document.
    std::vector<std::pair<SemanticToken, std::string>> derive_native_tokens(const Doc& d, const json& data);
    // Asks clangd for the tokens of the generated text in a worker thread. A late answer goes to the cache.
    void start_token_worker(const std::string& gen_uri, int version, uint64_t nonce);

    // ---- symbols across the .hot files of the workspace (S18)
    // The script places of the symbol at "cur": the declaration and each use. Or an error
    // message. "decl" gets the script place of the declaration.
    // "generated" (if not null) is set when the error is a name that the translator makes.
    std::variant<std::set<Pos>, std::string> symbol_places(
        const Doc& d, Pos cur, const std::string& name, Pos* decl, bool* generated = nullptr);
    // The class (and the member) that is declared at "decl", or no value for a local symbol.
    std::optional<SymbolKey> symbol_key(const Doc& d, Pos decl, const std::string& name) const;
    // False when no workspace root and no host script dir is known: the importers are unknown.
    bool importers_known() const;
    // The rename of the script symbol at "cur" through the table (resolve_at, places_of). No value:
    // the word is not in the table, and the native path is used. With "prepare" the answer is the
    // range and the placeholder of prepareRename, and "new_name" is not used.
    std::optional<std::variant<json, std::string>> table_rename(
        const Doc& d, Pos cur, const std::string& new_name, bool prepare);
    std::optional<SymbolKey> key_at(const Doc& d, Pos cur);
    std::optional<Pos> key_position(const Doc& d, const SymbolKey& key) const;
    Pos class_name_pos(const Doc& d, const hotc::AnalyzedClass& c) const;
    std::set<Pos> class_places(const Doc& d, const std::string& name) const;
    bool is_script_class(const Doc& d, const std::string& name) const;
    // A static member of a script class (a constant, a static field or a static method) at the
    // position: the word after "Class." or a bare name inside the class. The answer holds the
    // declaration and the member.
    struct StaticHit
    {
        Pos decl;
        const hotc::AnalyzedMember* member;
    };
    // The answer is empty when the bare word names a static but a same-name token of the function
    // may declare a local.
    std::optional<StaticHit> static_member_at(const Doc& d, Pos cur) const;
    // The scripts in the workspace folders. "with_import_dirs" adds the script dirs and the
    // include dirs of the hosts and of the options: a script there can import a workspace script.
    std::vector<std::string> workspace_scripts(bool with_import_dirs = false) const;
    // The hotc::ref_file_id of each direct script import of "text", the text of the script "d". The
    // search is the search of the translator (hotc::find_import) with the open documents. A path
    // that no file has is left out.
    std::vector<std::string> script_imports(const Doc& d, const std::string& text);
    // The table of the resolved names of "d", from its facts.
    FileRefs table_of_doc(const Doc& d);
    IndexShard shard_of_doc(const Doc& d);
    // The open document of the script at "path", or null.
    std::shared_ptr<Doc> open_doc_at(const std::string& path) const;
    // A document for a closed script: the paths and the text, with no facts and no clangd state.
    Doc closed_doc(const std::string& path, const std::string& text) const;
    // The table of the resolved names of the script at "path". An open document gives the table
    // of its facts. A closed file gives the table of the memory cache, of the index shard or of a
    // new analyze (the shard of the analyze goes to the index). Null when the file is not readable.
    // "text" (if given) is the text of a closed file, and file_refs does not read the file. Else
    // file_refs reads it. "used" (if given) gets the text of the table: the table offsets are
    // offsets in that text.
    std::shared_ptr<const FileRefs> file_refs(
        const std::string& path, const std::string* text = nullptr, Text* used = nullptr);
    // The script imports of the workspace scripts (workspace_scripts()), of each open script and
    // of the script at "also" (if given). The ids are hotc::ref_file_id. A closed file is read
    // again when its size or its time changes, an open document when its facts change. With
    // "fresh" the walk of the roots is always new.
    ImportGraph& import_graph(const std::string& also = std::string(), bool fresh = false);
    // Drops the cached imports of the script at "path", or of all scripts when "path" is empty.
    void drop_import_graph(const std::string& path);
    // Gives the closed script "tmp" (with its facts) to clangd for the call of "fn". Then closes
    // it in clangd. Gives false, and does not call "fn", when the facts have no C++.
    bool with_temp_clangd(Doc& tmp, const std::function<void()>& fn);

    // ---- the names through the tables of the resolved names (ref_resolver.h)
    // A resolved name: the ref and the word of the name.
    struct Resolved
    {
        hotc::ResolvedRef ref;
        std::string name;
    };
    // One window/showMessage for a rename of a class with an attribute or a tagged field (HOT-10).
    void warn_attribute_rename(const Resolved& r);
    // The conflict check for the members of the native bases (HOT-6). Gives a refusal text, or an
    // empty string when the rename can go on. rename_conflict does not see a native class, so the
    // workspace index of clangd gives the members: a symbol "<Base>::<new_name>". A hit refuses.
    // Fail closed: when clangd is not ready, gives an error or does not answer in time, the
    // rename is refused ("the members of the native base B are unknown"). A definitive empty
    // answer lets the rename go on. Limits: only the direct native base of a script class (no walk
    // to the base of a native base), and the index must know the header of the base.
    std::string native_member_conflict(const Doc& d, const std::vector<std::string>& bases, const std::string& new_name);
    // The places of a symbol, by the URI of the script. The positions are the starts of the names.
    using PlaceMap = std::map<std::string, std::set<Pos>>;
    // What clangd tells about a name of the generated text: the script ref of its declaration, a
    // declaration that is not a script ref (for example native), or no answer.
    struct ClangdRef
    {
        enum Kind
        {
            Found,
            Other,
            Unknown,
        } kind = Unknown;
        hotc::ResolvedRef ref;
    };
    // Asks clangd for the declaration of the name at "at" in "d" (an open document, or a closed
    // script in clangd through with_temp_clangd). Then finds the ref there (ref_at).
    ClangdRef clangd_ref(const Doc& d, Pos at);
    // The table of "d": file_refs for an open document, else the table of the facts of "d".
    std::shared_ptr<const FileRefs> refs_of(const Doc& d);
    // The symbol at "cur": the table first. A name of the table of unresolved names, or in a raw
    // body, goes to clangd. No value: no script symbol (a native name or no name).
    std::optional<Resolved> resolve_at(const Doc& d, Pos cur);
    // The places of the symbol in the scripts. A local or a parameter: the file of the
    // declaration only. Else the file of the declaration and its importers (import_graph). An
    // unresolved name or a raw body word with the same name goes to clangd. "strict" (rename):
    // a word that clangd cannot resolve gives an error with the file and the line. Not strict
    // (references): the word is left out. "also" (if given) is the script of the request: its
    // imports go to the graph also when it is not in the roots. "texts" (if given) gets the text
    // of each URI of the result: the positions are positions in that text. "files" (if given)
    // replaces the file of the declaration and its importers: the caller did the import walk.
    std::variant<PlaceMap, std::string> places_of(const Resolved& r, bool strict, const std::string& also = std::string(),
        std::map<std::string, Text>* texts = nullptr, const std::vector<std::string>* files = nullptr,
        const IndexSnapshot* given = nullptr);
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
    // The workspace folders (paths). They come from initialize and from didChangeWorkspaceFolders.
    // The index directory and the database directory of the clangd child are in the first folder
    // of initialize. Each is one flat directory for all folders, so it stays for the whole session.
    std::vector<std::string> roots_;
    std::vector<HostSet> hosts_; // the host files: one set for each hosts dir
    bool utf8_ = false, clangd_utf8_ = false, shutdown_ = false;
    std::vector<std::string> clangd_token_types_; // the legend of the semantic tokens of clangd
    std::set<std::string> preamble_ready_;
    // The documents whose patched completion text has had an AST wait (see on_completion).
    std::set<std::string> completion_synced_;
    // The facts of the imported scripts ("using "x.hot";") by file. An entry is valid while the text
    // hash of the file stays the same. import_parse_count() counts the parses (tests read it).
    struct ImportEntry
    {
        uint64_t text_hash = 0;
        std::shared_ptr<const hotc::ImportFacts> facts;
    };
    std::map<std::string, ImportEntry> import_cache_;
    // The file system of the import search: the text of an open document, not the disk. The disk
    // is the fallback of the search itself. Only the thread of the requests uses it.
    struct OpenDocsFs : hot::FileSystem
    {
        const Server* server = nullptr;
        bool read(const std::string& path, std::string& out) override
        {
            const std::string norm = fs::path(path).lexically_normal().string();
            for (const auto& [uri, doc] : server->docs_)
                if (fs::path(doc->path).lexically_normal().string() == norm)
                {
                    out = doc->text.s;
                    return true;
                }
            return false;
        }
        hot::VfsStat stat(const std::string& path) override { return hot::disk_file_system()->stat(path); }
    };
    std::shared_ptr<OpenDocsFs> open_docs_fs_;
    // The paths of the "using "x.hot";" lines. A line in a comment, and a "using" in a string, do not count.
    static std::vector<std::string> using_paths(const std::string& src)
    {
        std::vector<std::string> r;
        static const std::regex re("^[ \\t]*using[ \\t]*\"([^\"\\n]+\\.hot)\"");
        bool block = false;
        for (size_t b = 0; b < src.size();)
        {
            size_t e = src.find('\n', b);
            if (e == std::string::npos)
                e = src.size();
            const std::string line = src.substr(b, e - b);
            const std::string code = blank_non_code(line, block);
            const size_t first = line.find_first_not_of(" \t");
            std::smatch m;
            if (first != std::string::npos && code.compare(first, 5, "using") == 0 && std::regex_search(line, m, re))
                r.push_back(m[1].str());
            b = e + 1;
        }
        return r;
    }
    // The facts of the scripts that "text" imports, and of the scripts that they import. A cycle
    // and a missing file are skipped. "depths" gets the import depth of each entry: 0 for a direct
    // import. The translator gives an importer the functions and the variables of the direct imports only.
    std::vector<std::shared_ptr<const hotc::ImportFacts>> imported_facts(
        const Doc& d, const std::string& text, std::vector<int>* depths = nullptr)
    {
        std::vector<std::shared_ptr<const hotc::ImportFacts>> out;
        if (!open_docs_fs_)
        {
            open_docs_fs_ = std::make_shared<OpenDocsFs>();
            open_docs_fs_->server = this;
        }
        hotc::TranslateOptions o = translate_options(d);
        o.fs = open_docs_fs_;
        std::set<std::string> seen;
        seen.insert(fs::path(d.path).lexically_normal().string());
        struct Todo
        {
            std::string path, dir;
            int depth;
        };
        std::vector<Todo> todo;
        for (const std::string& pth : using_paths(text))
            todo.push_back({pth, fs::path(d.path).parent_path().string(), 0});
        for (size_t i = 0; i < todo.size() && i < 64; ++i)
        {
            std::string itext, file, unit;
            if (!hotc::find_import(o, todo[i].path, todo[i].dir, std::string(), itext, file, unit))
                continue;
            const std::string key = fs::path(file).lexically_normal().string();
            if (!seen.insert(key).second)
                continue;
            const uint64_t h = hotc::text_hash(itext);
            std::shared_ptr<const hotc::ImportFacts> facts;
            if (const auto it = import_cache_.find(key); it != import_cache_.end() && it->second.text_hash == h)
            {
                facts = it->second.facts;
            }
            else
            {
                facts = std::make_shared<const hotc::ImportFacts>(hotc::analyze_imports(itext, std::string()));
                ++g_import_parses;
                import_cache_[key] = {h, facts};
            }
            out.push_back(facts);
            if (depths)
                depths->push_back(todo[i].depth);
            for (const std::string& pth : facts->imports)
                if (pth.size() > 4 && pth.compare(pth.size() - 4, 4, ".hot") == 0)
                    todo.push_back({pth, fs::path(file).parent_path().string(), todo[i].depth + 1});
        }
        return out;
    }
    // The analyze facts of the scripts on disk that are not open (script_member_location).
    std::map<std::string, std::pair<std::string, hotc::AnalyzeResult>> disk_facts_;
    std::string index_seed_; // the first file of compile_commands.json (start_clangd)  // the generated files with a
                             // first completion
    // The saved symbol index of the scripts on disk (S28, symbol_index.h).
    SymbolIndex index_;
    // The answer of clangd for the semantic tokens of a generated text, by the URI of that text.
    struct NativeTokenCache
    {
        int version = -1; // the gen_version of "data" (-1: no answer)
        json data; // the answer of clangd, until the tokens are derived from it
        bool pending = false; // a worker thread waits for clangd
        uint64_t nonce = 0; // identifies this entry: the entry of a closed and reopened document is new
        int last_version = -1; // the gen_version of "last"
        std::vector<std::pair<SemanticToken, std::string>> last; // the tokens of the script and their names
    };
    struct TokenWorker
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::map<std::string, NativeTokenCache> token_cache_;
    uint64_t token_nonce_ = 0; // the last nonce of a cache entry (token_mutex_)
    std::mutex token_mutex_;
    std::condition_variable token_cv_;
    std::vector<TokenWorker> token_workers_;
    std::atomic<int> native_token_requests_{0}; // the requests of tokens to clangd (hot/indexStats)
    int completion_translations_ = 0; // the translations of a completion text (hot/indexStats)
    // The translations of the server (all analyze calls) and their total time (hot/indexStats).
    mutable std::atomic<int> translations_{0};
    mutable std::atomic<long long> translate_us_{0};
    template <class F> auto timed_translate(F&& f) const
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = f();
        translate_us_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        ++translations_;
        return r;
    }
    // The timing of the last update() of an open script (hot/indexStats): the sum of the translate
    // phases in nanoseconds, the passes, the parses, and the time after analyze().
    struct LastUpdate
    {
        hotc::TranslateTiming timing;
        int passes = 0;
        long long post_ns = 0, analyze_ns = 0;
        long long seq = 0;
    };
    mutable std::mutex last_update_mutex_;
    LastUpdate last_update_;
    int closed_analyses_ = 0; // the analyze calls of closed files: for_other_scripts and file_refs (hot/indexStats)
    // The tables of the closed scripts (file_refs), by path. "digest" is the shard digest.
    struct TableEntry
    {
        std::string digest;
        std::shared_ptr<const FileRefs> table;
    };
    std::map<std::string, TableEntry> table_cache_;
    // The tables that file_refs gives from a cache or a shard, and the tables that it makes from
    // an analyze result (hot/indexStats).
    int table_hits_ = 0, table_analyses_ = 0;
    int entry_hits_ = 0, entry_misses_ = 0; // the index entry lookups of closed scripts (hot/indexStats, state thread only)
    // The direct imports of each workspace script (import_graph), by path.
    struct GraphEntry
    {
        bool open = false; // the imports come from an open document
        int version = -1; // the facts_version of the open document
        uintmax_t size = 0;
        fs::file_time_type mtime;
    };
    std::map<std::string, GraphEntry> graph_files_;
    ImportGraph graph_;
    // The last walk of the roots (import_graph) and its time. drop_import_graph clears it.
    std::vector<std::string> graph_walk_;
    std::chrono::steady_clock::time_point graph_walk_at_;
    bool graph_walk_valid_ = false;
    hotc::TranslatorSession session_;
    // ---- the background index (HOT-34). It comes after session_ and index_: it dies first.
    // The workers use only the JobConfig and session_ / index_ (thread safe), and send_client.
    WorkspaceIndex windex_;
    bool progress_wanted_ = false; // the client has window.workDoneProgress
    // The progress of the index. The state thread and the workers call index_progress().
    std::mutex progress_mutex_;
    json progress_create_id_;
    bool progress_created_ = false, progress_begun_ = false, progress_ended_ = false;
    std::chrono::steady_clock::time_point progress_last_;
    void start_background_index();
    JobConfig index_job_config(const std::string& path) const;
    // The valid index entries of the closed paths. An open path is in neither "entries" nor "missing".
    // The server never gives an open path to wait_for. The index always describes the disk.
    // An inverted map is exact only for a path set that wait_for validated with an empty "missing" list.
    IndexSnapshot index_entries(const std::vector<std::string>& paths, std::chrono::steady_clock::time_point deadline);
    // The index key of a script path: the form of workspace_scripts().
    static std::string index_key(const std::string& path) { return fs::path(path).lexically_normal().string(); }
    bool index_active() const { return opt_.background_index && (windex_.started() || windex_.stats().total > 0); }
    // Queues all workspace scripts again with a new config. A host or an option change calls it.
    void reschedule_index();
    std::string host_signature() const;
    std::string index_signature_; // what the index jobs use from the hosts, at the last schedule
    void index_progress();
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
// XDG_CACHE_HOME does not change this. By default the child of hot-ls uses the real dir, so it
// shares the index with a C++ clangd on the same database. The shards are written to a temporary
// file and renamed, so two clangd processes can write one dir. With "private_clangd_dir" the
// child gets its own directory under ".cache/hot/clangd" with a link to the real
// compile_commands.json (a copy on Windows). Then its index does not mix with the other index.
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
    if (!opt_.private_clangd_dir || roots_.empty())
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
    // "--rename-file-limit=0": the default of 50 files makes clangd refuse a rename that
    // changes more files. A rename of a widely used native symbol needs more.
    std::vector<std::string> args = {opt_.clangd_log.empty() ? "--log=error" : "--log=info",
        "--header-insertion=never", "-j=2", "--limit-results=1000", "--rename-file-limit=0"};
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
                        {"semanticTokens", {{"requests", {{"full", true}}}, {"tokenTypes", json::array()}, {"tokenModifiers", json::array()}, {"formats", {"relative"}}}},
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
    clangd_token_types_.clear();
    for (const json& t : r.value("/result/capabilities/semanticTokensProvider/legend/tokenTypes"_json_pointer, json::array()))
        clangd_token_types_.push_back(t.is_string() ? t.get<std::string>() : std::string());
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
    // After the start: the request "initialize" is not cancellable. A cancel ends a later wait for clangd.
    c->wait_poll = [this] { check_cancel(); };
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

void Server::refresh_hosts(Doc* current, bool force)
{
    bool changed = force;
    for (HostSet& h : hosts_)
        changed = h.refresh() || changed;
    if (!changed)
        return;
    disk_facts_.clear();
    drop_import_graph(std::string());
    reschedule_index();
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
        close_lite(*d);
        remove_shell(*d);
        d->in_clangd = false;
        preamble_ready_.erase(d->gen_uri);
        completion_synced_.erase(d->gen_uri);
        if (d.get() != current)
            redo.push_back(d);
    }
    for (const auto& d : redo)
        update(*d);
}

void Server::on_watched_files(const json& p)
{
    bool host_file = false;
    // The prelude suffixes of the options and of all the hosts. A file that has none of them is
    // not a prelude, so it needs no walk of the scripts.
    std::set<std::string> suffixes, changed_suffixes;
    if (!opt_.prelude_suffix.empty())
        suffixes.insert(opt_.prelude_suffix);
    for (const HostSet& hs : hosts_)
        for (const auto& h : hs.all())
            if (!h->prelude_suffix.empty())
                suffixes.insert(h->prelude_suffix);
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
            // A new or a deleted file can change the import search of all the scripts.
            const int type = c["type"].is_number_integer() ? c["type"].get<int>() : 0;
            drop_import_graph(type == 2 ? path : std::string());
            if (index_active())
            {
                const std::string key = index_key(path);
                windex_.invalidate(key); // before forget(), so the importers of a deleted file queue
                if (type == 3)
                    windex_.forget(key);
                else if (type == 1 && in_workspace(key))
                    windex_.schedule(key, index_job_config(key), IndexLevel::Importer);
                // A new or a deleted user prelude changes the preludes of all the scripts.
                if (type == 1 || type == 3)
                    for (const std::string& suffix : suffixes)
                        if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
                            changed_suffixes.insert(suffix);
            }
        }
    }
    // One walk of the scripts for the whole notification.
    if (!changed_suffixes.empty() && index_active())
        for (const std::string& script : workspace_scripts())
        {
            const std::string& suffix = effective_options(script).prelude_suffix;
            if (changed_suffixes.count(suffix))
                windex_.schedule(script, index_job_config(script), IndexLevel::Rest);
        }
    if (host_file)
        refresh_hosts(nullptr);
}

void Server::on_workspace_folders(const json& p)
{
    const json ev = p.value("event", json::object());
    if (!ev.is_object())
        return;
    // A folder is known by its normalized path: "/a/b/" and "/a/b" are the same folder.
    auto norm = [](const std::string& path)
    {
        std::string s = fs::path(path).lexically_normal().string();
        while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
            s.pop_back();
        return s;
    };
    auto path_of = [](const json& f)
    { return f.is_object() ? uri_to_path(f.value("uri", std::string())) : std::string(); };
    auto find_root = [&](const std::string& path)
    {
        return std::find_if(roots_.begin(), roots_.end(), [&](const std::string& r) { return norm(r) == norm(path); });
    };
    std::vector<std::string> removed;
    for (const json& f : ev.value("removed", json::array()))
    {
        const std::string path = path_of(f);
        const auto it = path.empty() ? roots_.end() : find_root(path);
        if (it != roots_.end())
        {
            removed.push_back(norm(*it));
            roots_.erase(it);
        }
    }
    for (const json& f : ev.value("added", json::array()))
    {
        const std::string path = path_of(f);
        if (!path.empty() && find_root(path) == roots_.end())
            roots_.push_back(path);
    }
    // No folder at initialize: the index was off. The first folder now gives the index directory.
    // The clangd child keeps the database directory that it has: it is not started again.
    if (!index_.enabled() && opt_.index_dir.empty() && !roots_.empty())
    {
        index_.open((fs::path(roots_[0]) / ".cache" / "hot" / "index").string());
        index_.prune();
    }
    // The shards of the files that are in a removed folder, and in no other folder, go.
    auto under = [&](const std::string& file, const std::string& root)
    {
        const fs::path rel = fs::path(norm(file)).lexically_relative(root);
        return !rel.empty() && *rel.begin() != "..";
    };
    index_.remove_if(
        [&](const std::string& file)
        {
            const auto in = [&](const std::string& r) { return under(file, norm(r)); };
            return std::any_of(removed.begin(), removed.end(), in) && !std::any_of(roots_.begin(), roots_.end(), in);
        });
    disk_facts_.clear();
    drop_import_graph(std::string());
    load_hosts();
    refresh_hosts(nullptr, true);
}

json Server::on_initialize(const json& p)
{
    progress_wanted_ = p.value("/capabilities/window/workDoneProgress"_json_pointer, false) == true;
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
        for (const json& f : o.value("preludes", json::array()))
            opt_.preludes.push_back(f.get<std::string>());
        if (o.contains("preludeSuffix"))
            opt_.prelude_suffix = o.value("preludeSuffix", std::string());
        if (o.contains("hostsDir"))
            opt_.hosts_dir = o.value("hostsDir", std::string());
        if (o.contains("privateClangdDir") && o["privateClangdDir"].is_boolean())
            opt_.private_clangd_dir = o["privateClangdDir"].get<bool>();
        if (o.contains("nativeSemanticTokens") && o["nativeSemanticTokens"].is_boolean())
            opt_.native_semantic_tokens = o["nativeSemanticTokens"].get<bool>();
        if (o.contains("renameHostHeaders") && o["renameHostHeaders"].is_boolean())
            opt_.rename_host_headers = o["renameHostHeaders"].get<bool>();
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
                {"completionProvider", {{"triggerCharacters", {".", ":"}}}}, {"hoverProvider", true}, {"documentLinkProvider", {{"resolveProvider", false}}},
                {"inlayHintProvider", true}, {"codeLensProvider", {{"resolveProvider", false}}},
                {"callHierarchyProvider", true}, {"signatureHelpProvider", {{"triggerCharacters", {"(", ","}}}}, {"documentFormattingProvider", true},
                {"documentRangeFormattingProvider", true},
                {"codeActionProvider", {{"codeActionKinds", {"quickfix"}}}}, {"definitionProvider", true},
                {"referencesProvider", true}, {"documentSymbolProvider", true}, {"renameProvider", {{"prepareProvider", true}}},
                {"workspaceSymbolProvider", true},
                {"workspace", {{"workspaceFolders", {{"supported", true}, {"changeNotifications", true}}}}},
                {"semanticTokensProvider",
                    {{"legend", {{"tokenTypes", tokens}, {"tokenModifiers", json::array()}}}, {"full", true}}},
                {"experimental", {{"hotToScript", true}, {"clangd", clangd_ != nullptr}}}}},
        {"serverInfo", {{"name", "hot-ls"}, {"version", "0.1"}}},
    };
}

// ------------------------------------------------------------------ documents and diagnostics

// The marker line that the translator writes in the split mode (TranslateOptions::split_shell).
static const char kShellEnd[] = "\n// @hot-shell-end\n";

// Writes the file with a temporary name and a rename, so clangd never reads a half file.
static bool write_file_atomic(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    static std::atomic<unsigned> counter{0};
    const fs::path tmp = path.string() + ".tmp" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()))
        + "_" + std::to_string(counter++);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f)
            return false;
    }
    fs::rename(tmp, path, ec);
    if (ec)
        fs::remove(tmp, ec);
    return !ec;
}

// The unit text with the bodies of the own functions empty, but the body that has the line "keep_line0". Each body
// is between the lines "// @hot-body-begin" (with "keep" for a body that stays) and "// @hot-body-end". The lines stay: a position of the unit is the same
// position in the result. Gives nothing when the line is in no body.
static std::optional<std::string> blank_other_bodies(const std::string& unit, int keep_line0)
{
    static const std::string begin = "\n// @hot-body-begin", keep_tag = " keep", end = "\n// @hot-body-end\n";
    std::string out;
    out.reserve(unit.size());
    size_t at = 0;
    int line = 0; // the line of unit[at]
    bool kept = false;
    for (;;)
    {
        const size_t b = unit.find(begin, at);
        if (b == std::string::npos)
            break;
        // The emitter writes "keep" for a body that is never empty (a deduced return type).
        const bool deduced = unit.compare(b + begin.size(), keep_tag.size(), keep_tag) == 0;
        const size_t line_end = unit.find('\n', b + 1);
        if (line_end == std::string::npos)
            return std::nullopt;
        const size_t s0 = line_end + 1;
        const size_t e = unit.find(end, s0 - 1);
        if (e == std::string::npos)
            return std::nullopt;
        const size_t s1 = e + 1;
        out.append(unit, at, s0 - at);
        line += static_cast<int>(std::count(unit.begin() + static_cast<std::ptrdiff_t>(at), unit.begin() + static_cast<std::ptrdiff_t>(s0), '\n'));
        const int lines = static_cast<int>(std::count(unit.begin() + static_cast<std::ptrdiff_t>(s0), unit.begin() + static_cast<std::ptrdiff_t>(s1), '\n'));
        if (deduced || (keep_line0 >= line && keep_line0 < line + lines))
        {
            out.append(unit, s0, s1 - s0);
            kept = kept || (keep_line0 >= line && keep_line0 < line + lines);
        }
        else
            out.append(static_cast<size_t>(lines), '\n');
        line += lines;
        at = s1;
    }
    out.append(unit, at, std::string::npos);
    if (!kept)
        return std::nullopt;
    return out;
}

std::string Server::lite_uri(const Doc& d, const std::string& full, int gen_line0, int wait_ms)
{
    std::string r = lite_uri_impl(d, full, gen_line0, wait_ms);
    if (lite_doc_enabled())
        ++(r == d.gen_uri ? lite_fallbacks_ : lite_hits_);
    return r;
}

void Server::lite_check(const char* method, const json& params, const Doc& d, const json& lite_answer, const char* what)
{
    if (!lite_check_enabled() || params["textDocument"].value("uri", std::string()) == d.gen_uri)
        return;
    json full_params = params;
    full_params["textDocument"]["uri"] = d.gen_uri;
    const auto full = clangd_result(method, full_params, kNativeBudgetMs);
    auto norm = [](json j)
    {
        // A list of completion items: the sorted labels. Another answer: as it is.
        json items = j.is_array() ? j : j.is_object() && j.contains("items") ? j["items"] : json();
        if (items.is_array())
        {
            std::vector<std::string> labels;
            for (const json& i : items)
                labels.push_back(i.value("label", std::string()));
            std::sort(labels.begin(), labels.end());
            return json(labels);
        }
        return j;
    };
    if (!full || norm(*full) == norm(lite_answer))
        return;
    ++lite_mismatches_;
    log_info(std::string("lite mismatch (") + what + ") " + d.path + ": lite " + norm(lite_answer).dump().substr(0, 300)
        + " full " + norm(*full).dump().substr(0, 300));
}

std::string Server::lite_uri_impl(const Doc& d, const std::string& full, int gen_line0, int wait_ms)
{
    if (!lite_doc_enabled() || !clangd_ || !clangd_->running() || !preamble_ready_.count(d.gen_uri))
        return d.gen_uri; // a cold document: the requests follow the budget of the full document
    const size_t at = full.find(kShellEnd);
    if (at == std::string::npos || !d.shell_active)
        return d.gen_uri;
    // The header is the one that send_generated wrote for the full document. A different header: the full document.
    if (std::hash<std::string>()(full.substr(0, at + 1)) != d.shell_hash)
        return d.gen_uri;
    {
        std::error_code ec;
        if (!fs::exists(d.shell_path, ec))
            return d.gen_uri; // the header is gone
    }
    const auto unit = shell_unit_with_include(d.shell_path, full, at);
    if (!unit)
        return d.gen_uri;
    const auto lite = blank_other_bodies(*unit, gen_line0);
    if (!lite)
        return d.gen_uri; // the place is in no function body
    const std::string path = d.gen_path + ".lite.cpp", uri = path_to_uri(path);
    const size_t text_hash = std::hash<std::string>()(*lite);
    bool wait = false;
    {
        std::lock_guard<std::mutex> lock(lite_mutex_);
        if (!lite_.count(d.gen_uri) && lite_.size() >= kMaxLiteDocs)
        {
            auto oldest = lite_.begin();
            for (auto it = lite_.begin(); it != lite_.end(); ++it)
                if (it->second.used < oldest->second.used)
                    oldest = it;
            drop_lite_locked(oldest);
        }
        LiteState& st = lite_[d.gen_uri];
        st.used = ++lite_tick_;
        st.uri = uri;
        if (!st.open || st.text_hash != text_hash)
        {
            clangd_->set_uri_alias(uri, d.gen_uri, true);
            if (st.open && st.shell_hash != d.shell_hash)
            {
                // A new header: the document starts again, so it has no old preamble (see send_generated).
                clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", uri}}}});
                st.open = false;
                st.ready = false;
            }
            const int v = ++st.version;
            if (!st.open)
            {
                hot::ClangFilesOptions co;
                const Effective e = effective_options(d.path);
                co.include_dirs = e.include_dirs;
                co.resource_dir = e.resource_dir;
                co.std_version = e.std_version;
                co.extra_flags = e.extra_flags;
                const std::vector<std::string> cmd = hot::compile_object_command(co, path, path + ".o", std::string());
                clangd_->notify("workspace/didChangeConfiguration",
                    {{"settings",
                        {{"compilationDatabaseChanges",
                            {{path,
                                {{"workingDirectory", fs::path(path).parent_path().string()},
                                    {"compilationCommand", cmd}}}}}}}});
                clangd_->notify("textDocument/didOpen",
                    {{"textDocument", {{"uri", uri}, {"languageId", "cpp"}, {"version", v}, {"text", *lite}}}});
                st.open = true;
            }
            else
                clangd_->notify("textDocument/didChange",
                    {{"textDocument", {{"uri", uri}, {"version", v}}},
                        {"contentChanges", json::array({{{"text", *lite}}})}});
            st.text_hash = text_hash;
            st.shell_hash = d.shell_hash;
        }
        wait = !st.ready;
    }
    // A new document has no preamble. Before the first preamble clangd gives only words of the file, so the
    // request waits for the AST here, but not longer than half of its own budget. After a time-out the request uses the
    // full document, and the lite document finishes its open in the background.
    if (wait)
    {
        // Half of the budget at most: the other half is for the answer of the full document after a time-out.
        if (wait_ms < 2 || !clangd_result("textDocument/documentSymbol", {{"textDocument", {{"uri", uri}}}}, wait_ms / 2))
            return d.gen_uri;
        std::lock_guard<std::mutex> lock(lite_mutex_);
        lite_[d.gen_uri].ready = true;
    }
    return uri;
}

// The base of the header directories: one for each user. "HOT_LS_SHELL_DIR" is for the tests.
static fs::path shell_base_dir()
{
    std::error_code ec;
    if (const char* env_dir = std::getenv("HOT_LS_SHELL_DIR"); env_dir && *env_dir)
        return env_dir;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return fs::path(xdg) / "hot-ls";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".cache" / "hot-ls";
#ifdef _WIN32
    return fs::temp_directory_path(ec) / "hot-ls-shell";
#else
    return fs::temp_directory_path(ec) / ("hot-ls-shell-" + std::to_string(static_cast<long long>(::getuid())));
#endif
}

static bool process_is_alive(long long pid)
{
#ifdef _WIN32
    (void)pid;
    return true; // no prune on Windows
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

// The directory of the headers of this server: "<base>/<pid>-<n>", mode 0700, in the canonical form (no symbolic
// link). It starts by the removal of the directories of dead processes. The server removes its own at exit.
const std::string& Server::shell_dir()
{
    if (!shell_dir_.empty())
        return shell_dir_;
    static std::atomic<int> counter{0};
    std::error_code ec;
    const fs::path base = shell_base_dir();
    fs::create_directories(base, ec);
    fs::permissions(base, fs::perms::owner_all, ec);
    const fs::path canon = fs::weakly_canonical(base, ec);
    for (const auto& e : fs::directory_iterator(canon, ec))
    {
        const std::string name = e.path().filename().string();
        const size_t dash = name.find('-');
        if (dash == std::string::npos || !e.is_directory(ec)
            || name.find_first_not_of("0123456789", 0) != dash)
            continue;
        if (!process_is_alive(std::atoll(name.substr(0, dash).c_str())))
            fs::remove_all(e.path(), ec);
    }
#ifdef _WIN32
    const long long pid = 0;
#else
    const long long pid = static_cast<long long>(::getpid());
#endif
    const fs::path dir = canon / (std::to_string(pid) + "-" + std::to_string(counter++));
    fs::create_directories(dir, ec);
    fs::permissions(dir, fs::perms::owner_all, ec);
    shell_dir_ = dir.string();
    return shell_dir_;
}

// The text that clangd gets for the unit. With the marker, the text before it goes to a header and the unit
// starts with an include of it. The unit keeps the lines of the full text: the include line and empty lines
// take the place of the shell. So each position of the full text is the same position in the unit, and a
// place in the header is the same place in the full text. The header name has the hash of its content, so two
// processes never write the same name with different content. Only send_generated calls this function.
std::string Server::unit_for_clangd(Doc& d, const std::string& cpp)
{
    d.shell_active = false;
    const size_t at = cpp.find(kShellEnd);
    if (at == std::string::npos)
        return cpp;
    const std::string shell = cpp.substr(0, at + 1);
    const size_t hash = std::hash<std::string>()(shell);
    char name[48];
    std::snprintf(name, sizeof name, "%016zx-%016zx.shell.h", std::hash<std::string>()(d.gen_path), hash);
    const std::string path = (fs::path(shell_dir()) / name).string();
    const auto unit = shell_unit_with_include(path, cpp, at);
    if (!unit)
        return cpp; // a path that the include cannot hold: clangd gets the full text
    std::error_code ec;
    if (path != d.shell_path || !fs::exists(path, ec))
    {
        // The name has the hash of the content: an equal file stays, also when the rename fails.
        if (!fs::exists(path, ec) && !write_file_atomic(path, shell) && !fs::exists(path, ec))
            return cpp; // no header: clangd gets the full text
        if (!d.shell_path.empty() && d.shell_path != path)
            d.shell_old = d.shell_path;
        d.shell_path = path;
        d.shell_hash = hash;
        // clangd builds a new preamble for the new header. The next request waits for it, as for a new document.
        preamble_ready_.erase(d.gen_uri);
        completion_synced_.erase(d.gen_uri);
        d.shell_changed = true;
    }
    d.shell_active = true;
    clangd_->set_uri_alias(path_to_uri(d.shell_path), d.gen_uri);
    return *unit;
}

void Server::send_generated(Doc& d, const std::string& full)
{
    if (!is_slow_worker)
        ++epoch_;
    if (!clangd_ || !clangd_->running())
        return;
    const std::string cpp = unit_for_clangd(d, full);
    if (d.shell_changed)
    {
        // clangd can answer a completion with the old preamble while it builds the new one. A reopened
        // document has no old preamble: the first requests wait for the new one.
        d.shell_changed = false;
        if (d.in_clangd)
        {
            clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", d.gen_uri}}}});
            d.in_clangd = false;
        }
    }
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
        const std::vector<std::string> cmd = hot::compile_object_command(co, d.gen_path, d.gen_path + ".o", std::string());
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
    // The new header is open now. The header of the old text is not needed (best effort).
    if (!d.shell_old.empty())
    {
        std::error_code ec;
        fs::remove(d.shell_old, ec);
        d.shell_old.clear();
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
        ++d.facts_version;
        if (d.cpp_published)
        {
            d.cpp_published = false;
            send_client({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
                {"params", {{"uri", d.uri}, {"diagnostics", json::array()}}}});
        }
        return;
    }
    hotc::AnalyzeResult facts = timed_translate([&] { return hotc::analyze_cpp(d.text.s, d.path, translate_options(d), session_); });
    facts.diagnostics.clear(); // facts.errors has the errors of this file
    std::lock_guard<std::mutex> lock(docs_mutex_);
    d.facts = std::move(facts);
    ++d.facts_version;
    d.cpp_published = true;
    send_client(publish_message(d));
}

void Server::update(Doc& d)
{
    if (d.cpp)
        return update_cpp(d);
    const auto update_t0 = std::chrono::steady_clock::now();
    hotc::AnalyzeResult facts =
        timed_translate([&] { return hotc::analyze(d.text.s, translate_options(d), session_); });
    const auto post_t0 = std::chrono::steady_clock::now();
    LastUpdate last;
    last.timing = facts.timing;
    last.passes = facts.passes;
    last.analyze_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(post_t0 - update_t0).count();
    std::string cpp = facts.cpp;
    {
        std::lock_guard<std::mutex> lock(docs_mutex_);
        d.facts = std::move(facts);
        ++d.facts_version;
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
    last.post_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - post_t0).count();
    std::lock_guard<std::mutex> lock(last_update_mutex_);
    last.seq = last_update_.seq + 1;
    last_update_ = last;
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
        add({e.pos.line - 1, e.pos.col - 1}, e.message, e.note ? 3 : 1);
    // A removed field and an added field without [alias] make a pair, in declaration order. The
    // value of the old field is lost at reload. A field with [alias] covers its old name.
    for (const hotc::AnalyzedClass& c : d.facts.classes)
    {
        const auto base = d.open_fields.find(c.name);
        if (base == d.open_fields.end())
            continue;
        std::set<std::string> now, covered;
        for (const hotc::AnalyzedMember& m : c.members)
            if (m.kind == "field")
            {
                now.insert(m.name);
                if (!m.alias.empty())
                    covered.insert(m.alias);
            }
        std::vector<std::string> removed;
        for (const std::string& n : base->second)
            if (!now.count(n) && !covered.count(n))
                removed.push_back(n);
        size_t next = 0;
        for (const hotc::AnalyzedMember& m : c.members)
        {
            if (next >= removed.size())
                break;
            if (m.kind != "field" || !m.alias.empty()
                || std::find(base->second.begin(), base->second.end(), m.name) != base->second.end())
                continue;
            add({m.begin.line - 1, m.begin.col - 1},
                "field '" + removed[next] + "' is renamed to '" + m.name + "' without [alias]; the value is lost at hot reload",
                2);
            ++next;
        }
    }
    // The translation errors: "name:line:col: error: text", one line each.
    static const std::regex re(R"(^(.*):(\d+):(\d+): (fatal error|error|warning|note): (.*)$)");
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
            if (!is_served_path(path) && path != d.path && m[4] != "warning" && m[4] != "note")
                headers.emplace(path, std::make_pair(std::stoi(m[2]), m[5].str()));
            continue;
        }
        add({std::stoi(m[2]) - 1, std::stoi(m[3]) - 1}, m[5], m[4] == "note" ? 3 : m[4] == "warning" ? 2 : 1);
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
    static const std::regex re(R"(^(.*):(\d+):(\d+): (fatal error|error|warning|note): (.*)$)");
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
        const int severity = m[4] == "note" ? 3 : m[4] == "warning" ? 2 : 1;
        out[uri].push_back({{"range", range}, {"severity", severity}, {"source", "hot"}, {"message", m[5].str()}});
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
        // An error inside the shell header: clangd reports it at the include line of the unit (line 0).
        if (is_shell_include_diagnostic(d.shell_active, g["range"]["start"].value("line", 0)))
            continue;
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
                        {"message", hotc::rewrite_clang_message(g.value("message", std::string()), &d.facts.indirect_names)}});
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
            {"message", hotc::rewrite_clang_message(g.value("message", std::string()), &d.facts.indirect_names)}});
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
    // A script class name cannot start with "hot_" (the prefix of the generated code).
    return n.empty() || n[0] == '_' || n.compare(0, 4, "hot_") == 0 || n.compare(0, 9, "HotModule") == 0 || n == "hot";
}

// A name of the C library that clangd offers in a bare completion because the generated code
// includes the system headers. A script does not use these names. The list is short on purpose: a
// native name of the host is never hidden.
static bool is_libc_name(const std::string& n)
{
    static const std::set<std::string> names = {"a64l", "aligned_alloc", "asprintf",
        "at_quick_exit", "atexit", "atof", "atoi", "atol", "atoll", "bsearch", "calloc", "clearenv",
        "clearerr", "ctermid", "cuserid", "dprintf", "drand48", "ecvt", "erand48", "explicit_bzero",
        "fclose", "fcloseall", "fcvt", "fdopen", "feof", "ferror", "fflush", "fgetc", "fgetpos", "fgets", "fileno",
        "flockfile", "fmemopen", "fopen", "fopencookie", "fprintf", "fputc", "fputs", "fread", "freopen",
        "fscanf", "fseek", "fseeko", "fsetpos", "ftell", "ftello", "ftrylockfile", "funlockfile", "fwrite", "gcvt",
        "getdelim", "getenv", "getloadavg", "getpt", "getsubopt", "getw", "grantpt",
        "initstate", "isalnum", "isalpha", "isascii", "isblank", "iscntrl", "isctype", "isdigit", "isgraph",
        "islower", "isprint", "ispunct", "isspace", "isupper", "isxdigit", "jrand48", "l64a", "lcong48",
        "ldiv", "llabs", "lldiv", "localeconv", "lrand48", "malloc", "mblen", "mbstowcs", "mbtowc", "memalignment",
        "memccpy", "memchr", "memcmp", "memcpy", "memfrob", "memmem", "memmove", "mempcpy", "memrchr", "memset",
        "memset_explicit", "mkdtemp", "mkostemp", "mkostemps", "mkstemp", "mkstemps", "mrand48", "newlocale",
        "nrand48", "obstack_printf", "obstack_vprintf", "on_exit", "open_memstream", "pclose",
        "posix_memalign", "posix_openpt", "printf", "putc", "putchar", "putw", "qsort",
        "rand_r", "realloc", "reallocarray", "realpath",
        "scanf", "setbuffer", "setenv", "setlinebuf", "setvbuf", "snprintf", "sprintf",
        "srand48", "srandom", "sscanf", "strcasecmp", "strcat", "strchr", "strcmp", "strcoll", "strcpy", "strcspn",
        "strdup", "strdupa", "strerror", "strlen", "strncasecmp", "strncat", "strncmp", "strncpy", "strndup",
        "strndupa", "strnlen", "strpbrk", "strrchr", "strsep", "strspn", "strstr", "strtod", "strtof", "strtok",
        "strtol", "strtold", "strtoll", "strtoul", "strtoull", "WEXITSTATUS", "WIFCONTINUED",
        "WIFEXITED", "WIFSIGNALED", "WIFSTOPPED", "WSTOPSIG", "WTERMSIG", "tmpnam", 
        "ungetc", "unsetenv", "vasprintf", "vdprintf", "vfprintf", "vfscanf", "vprintf", "vscanf", "vsnprintf",
        "vsprintf", "vsscanf", "wctomb", "wcstombs", "drand48_data", "pthread_attr_t"};
    // The macros of <stdint.h>, <endian.h> and <sys/wait.h>.
    static const std::regex macro("U?INT(8|16|32|64|MAX)_C|(be|le)(16|32|64)toh|hto(be|le)(16|32|64)");
    return names.count(n) != 0 || std::regex_match(n, macro);
}

// The record and the create function that the translator makes for a script class: the name is
// "<ScriptClass>_record" or "<ScriptClass>_create". Other names with the suffix stay.
static bool is_generated_class_name(const Doc& d, const std::string& n)
{
    for (const char* suffix : {"_record", "_create"})
    {
        const std::string x = suffix;
        if (n.size() > x.size() && n.compare(n.size() - x.size(), x.size(), x) == 0)
        {
            const std::string cls = n.substr(0, n.size() - x.size());
            for (const hotc::AnalyzedClass& c : d.facts.classes)
                if (c.name == cls)
                    return true;
        }
    }
    return false;
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


json Server::using_completion(const Doc& d, Pos cur, size_t off, size_t line_begin)
{
    const std::string& t = d.text.s;
    const std::string_view before = std::string_view(t).substr(line_begin, off - line_begin);
    const size_t first = before.find_first_not_of(" \t");
    json items = json::array();
    const json empty = {{"isIncomplete", false}, {"items", items}};
    if (first == std::string_view::npos || before.compare(first, 5, "using") != 0)
        return empty;
    const size_t q = before.find('"', first + 5);
    const std::string cur_file = fs::path(d.path).lexically_normal().string();
    const Effective eff = effective_options(d.path);
    std::vector<std::string> dirs = {fs::path(d.path).parent_path().string()};
    dirs.insert(dirs.end(), eff.include_dirs.begin(), eff.include_dirs.end());
    if (q != std::string_view::npos)
    {
        // A path: the text after the quote is the partial path. Only it is replaced.
        const std::string partial(before.substr(q + 1));
        if (partial.find('"') != std::string::npos)
            return empty;
        const size_t slash = partial.find_last_of('/');
        const std::string dir_part = slash == std::string::npos ? std::string() : partial.substr(0, slash + 1);
        const std::string name_part = slash == std::string::npos ? partial : partial.substr(slash + 1);
        const Pos b = {cur.line, static_cast<int>(q + 1 + (line_begin - d.text.starts[static_cast<size_t>(cur.line)]))};
        const json range = {{"start", to_lsp(d.text, b, utf8_)}, {"end", to_lsp(d.text, cur, utf8_)}};
        std::set<std::string> seen;
        for (const std::string& base : dirs)
        {
            std::error_code ec;
            int n = 0;
            for (fs::directory_iterator it(fs::path(base) / dir_part, ec), end; !ec && it != end && n < 500; it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.' || name.compare(0, name_part.size(), name_part) != 0)
                    continue;
                const bool is_dir = it->is_directory(ec);
                const std::string ext = it->path().extension().string();
                static const std::set<std::string> kExt = {".hot", ".h", ".hpp", ".hxx", ".hh"};
                if (!is_dir && !kExt.count(ext))
                    continue;
                if (!is_dir && fs::path(it->path()).lexically_normal().string() == cur_file)
                    continue;
                const std::string label = dir_part + name + (is_dir ? "/" : "");
                if (!seen.insert(label).second)
                    continue;
                ++n;
                items.push_back({{"label", label}, {"kind", is_dir ? 19 : 17}, {"filterText", label},
                    {"sortText", (is_dir ? "1" : "0") + label}, {"textEdit", {{"range", range}, {"newText", label}}}});
            }
        }
        return {{"isIncomplete", false}, {"items", items}};
    }
    // A namespace: the qualified name before the cursor, split at the last separator.
    size_t ws = off;
    while (ws > line_begin && (is_id_char(t[ws - 1]) || t[ws - 1] == '.'))
        --ws;
    const std::string qual = t.substr(ws, off - ws);
    const size_t dot = qual.find_last_of('.');
    const std::string parent = dot == std::string::npos ? std::string() : qual.substr(0, dot + 1);
    const std::string name_part = dot == std::string::npos ? qual : qual.substr(dot + 1);
    std::set<std::string> found;
    if (parent.empty())
        found.insert("std");
    bool block = false;
    for (int l = 0; l < static_cast<int>(d.text.starts.size()); ++l)
    {
        const std::string code = blank_non_code(d.text.line(l), block);
        static const std::regex re("^[ \\t]*using[ \\t]*\"([^\"\\n]+)\"");
        std::smatch m;
        const std::string line(d.text.line(l));
        if (code.find("using") == std::string::npos || !std::regex_search(line, m, re))
            continue;
        const std::string file = m[1].str();
        if (file.size() < 2 || file.compare(file.size() - 4, 4, ".hot") == 0)
            continue;
        for (const std::string& base : dirs)
        {
            const auto h = read_file((fs::path(base) / file).string());
            if (!h)
                continue;
            // The stack of the open namespaces: the depth of its brace and its qualified name.
            std::vector<std::pair<int, std::string>> stack;
            int depth = 0;
            bool hb = false;
            size_t b = 0;
            while (b < h->size())
            {
                size_t e = h->find('\n', b);
                if (e == std::string::npos)
                    e = h->size();
                const std::string hl = blank_non_code(std::string_view(*h).substr(b, e - b), hb);
                static const std::regex ns_re("\\bnamespace[ \\t]+([A-Za-z_][A-Za-z0-9_]*(?:[ \\t]*::[ \\t]*[A-Za-z_][A-Za-z0-9_]*)*)[ \\t]*\\{");
                std::smatch nm;
                std::string rest = hl;
                for (size_t i = 0; i < hl.size(); ++i)
                {
                    if (hl[i] == '{')
                    {
                        // A namespace brace is handled by the match below.
                        if (!(std::regex_search(rest = hl.substr(0, i + 1), nm, ns_re) && static_cast<size_t>(nm.position(0) + nm.length(0)) == i + 1))
                            ++depth;
                        else
                        {
                            std::string n = nm[1].str();
                            n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' ' || c == '\t'; }), n.end());
                            for (size_t k; (k = n.find("::")) != std::string::npos;)
                                n.replace(k, 2, ".");
                            const std::string full = stack.empty() ? n : stack.back().second + "." + n;
                            ++depth;
                            stack.push_back({depth, full});
                            found.insert(full);
                        }
                    }
                    else if (hl[i] == '}')
                    {
                        if (!stack.empty() && stack.back().first == depth)
                            stack.pop_back();
                        --depth;
                    }
                }
                b = e + 1;
            }
            break;
        }
    }
    for (const std::string& n : found)
    {
        if (n.compare(0, parent.size(), parent) != 0)
            continue;
        const std::string tail = n.substr(parent.size());
        if (tail.find('.') != std::string::npos || tail.compare(0, name_part.size(), name_part) != 0)
            continue;
        items.push_back({{"label", tail}, {"kind", 9}, {"filterText", tail}, {"sortText", "0" + tail}});
    }
    return {{"isIncomplete", false}, {"items", items}};
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
    // No completion in a comment or in a string. The scan starts at the file start, so a block
    // comment from an earlier line counts. The path in a "using" line is text of a string, and it
    // completes.
    const size_t line_begin = d.text.starts[static_cast<size_t>(cur.line)];
    {
        bool block = false;
        for (int l = 0; l < cur.line; ++l)
            blank_non_code(d.text.line(l), block);
        const bool block_at_line_start = block;
        const std::string_view before = std::string_view(t).substr(line_begin, off - line_begin);
        const size_t first = before.find_first_not_of(" \t");
        // The exemption: the line starts with the word "using" and the cursor is in the first string
        // of the line. A block comment from an earlier line has no exemption.
        bool using_line = false;
        if (!block_at_line_start && first != std::string_view::npos && before.compare(first, 5, "using") == 0
            && (first + 5 >= before.size() || !is_id_char(before[first + 5])))
        {
            const size_t q = before.find('"', first + 5);
            using_line = q != std::string_view::npos && before.find('"', q + 1) == std::string_view::npos
                && before.find_first_not_of(" \t", first + 5) == q;
        }
        // A sentinel at the cursor shows whether the cursor is in text that the scan blanks.
        const std::string code = blank_non_code(std::string(before) + "x", block);
        if (!using_line && code.back() == ' ')
            return empty;
    }

    // After the word "override" in a class body: the virtual methods that the class can override.
    if (!after_dot && !after_scope)
    {
        size_t w = s;
        while (w > 0 && std::isspace(static_cast<unsigned char>(t[w - 1])))
            --w;
        if (w < s && w >= 8 && t.compare(w - 8, 8, "override") == 0 && (w == 8 || !is_id_char(t[w - 9])))
        {
            // The class whose body is at the cursor: the cursor is at brace depth 1, and the brace
            // is the first one after the start of the class. A top-level word and a method body give none.
            std::vector<size_t> braces;
            {
                bool block = false;
                for (int l = 0; l <= cur.line; ++l)
                {
                    const std::string code = blank_non_code(d.text.line(l), block);
                    const size_t lb = d.text.starts[static_cast<size_t>(l)];
                    for (size_t i = 0; i < code.size() && lb + i < s; ++i)
                        if (code[i] == '{')
                            braces.push_back(lb + i);
                        else if (code[i] == '}' && !braces.empty())
                            braces.pop_back();
                }
            }
            const hotc::AnalyzedClass* cls = nullptr;
            if (braces.size() == 1)
                for (const hotc::AnalyzedClass& c : d.facts.classes)
                {
                    const size_t cb = d.text.starts[static_cast<size_t>(c.begin.line - 1)] + static_cast<size_t>(c.begin.col - 1);
                    if (cb < braces[0] && t.find('{', cb) == braces[0])
                        cls = &c;
                }
            json list = json::array();
            if (cls)
                for (const hotc::VirtualMethod& m : hotc::virtual_methods(t, translate_options(d), session_, cls->name))
                {
                    if (!has_prefix(m.name, prefix))
                        continue;
                    std::string args;
                    for (size_t i = 0; i < m.param_types.size(); ++i)
                        args += (i ? ", " : "") + m.param_types[i] + " " + m.param_names[i];
                    list.push_back({{"label", m.name}, {"kind", 2}, {"detail", m.ret_script + m.signature + " of " + m.owner},
                        {"filterText", m.name}, {"sortText", "0" + m.name}, {"insertTextFormat", 2},
                        {"insertText", m.ret_script + " " + m.name + "(" + args + ") {\n\t$0\n}"}});
                }
            return {{"isIncomplete", false}, {"items", list}};
        }
    }

    // "using": a path in a string, or a namespace. A path comes from the directory of the file and
    // the include dirs. A namespace comes from the headers that the file imports and from "std".
    // The scan of a header is text only: it does not use clangd and it follows no include.
    if (!after_scope)
    {
        const CompletionContext uctx = classify(t, off);
        if (uctx.kind == CompletionContext::Kind::Using)
            return using_completion(d, cur, off, line_begin);
    }

    // The kind of the place. A place of a name that is new, an enum body and an attribute give no
    // generic list. A type place gives types only. A place of both kinds gives types and expressions.
    const CompletionContext ctx = classify(t, off);
    if (ctx.kind == CompletionContext::Kind::Attribute || ctx.kind == CompletionContext::Kind::AttributeArg)
        return attribute_completion(t, off, ctx.prefixStart, ctx.kind == CompletionContext::Kind::AttributeArg);
    if (ctx.kind == CompletionContext::Kind::EnumBody)
        return empty;
    const bool type_only = (ctx.kind == CompletionContext::Kind::Type || ctx.kind == CompletionContext::Kind::Base)
        && !ctx.exprAndType && !after_dot && !after_scope;
    const bool base_only = ctx.kind == CompletionContext::Kind::Base && !after_dot && !after_scope;
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
    std::vector<int> imported_depth;
    const auto imported = imported_facts(d, t, &imported_depth);
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
            {
                // A class of an import: its public members, then its base.
                const hotc::ImportClass* ic = nullptr;
                for (const auto& f : imported)
                    for (const hotc::ImportClass& c : f->classes)
                        if (!ic && c.name == n)
                            ic = &c;
                if (!ic)
                    break;
                for (const hotc::ImportMember& m : ic->members)
                {
                    if (m.is_static || m.kind == "constructor")
                        continue;
                    add(m.name, m.kind == "method" ? 2 : m.kind == "property" ? 10 : 5,
                        (m.type.empty() ? m.kind : m.type) + m.signature + " of " + ic->name, "0");
                }
                n = ic->base;
                continue;
            }
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
    // The statics and the constants of a script class and of its script bases.
    auto add_statics = [&](const std::string& class_name)
    {
        std::set<std::string> seen;
        for (std::string n = class_name; !n.empty() && seen.insert(n).second;)
        {
            if (const auto it = scope.class_statics.find(n); it != scope.class_statics.end())
                for (const ScopeName& m : it->second)
                    add(m.name, m.detail == "static method" ? 2 : m.detail == "static const" ? 21 : 5,
                        m.detail + " of " + n + (m.type.empty() ? "" : ": " + m.type), "0");
            std::string base;
            for (const hotc::AnalyzedClass& c : d.facts.classes)
                if (c.name == n)
                    base = c.base;
            for (const auto& f : imported)
                for (const hotc::ImportClass& c : f->classes)
                    if (c.name == n)
                    {
                        base = c.base;
                        for (const hotc::ImportMember& m : c.members)
                            if (m.is_static)
                                add(m.name, m.kind == "method" ? 2 : m.is_const ? 21 : 5,
                                    std::string(m.is_const ? "static const" : m.kind == "method" ? "static method" : "static field")
                                        + " of " + n + (m.type.empty() ? "" : ": " + m.type) + m.signature,
                                    "0");
                    }
            n = base;
        }
    };
    // The class whose body holds the position: the last class that starts before it.
    auto class_at = [&](Pos at) -> const hotc::AnalyzedClass*
    {
        const hotc::AnalyzedClass* cls = nullptr;
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            if (Pos{c.begin.line - 1, c.begin.col - 1} < Pos{at.line, at.col + 1})
                cls = &c;
        return cls;
    };
    if (after_dot)
    {
        // The safe-access dot "?." has the receiver before the "?".
        const size_t oe = s >= 2 && t[s - 2] == '?' ? s - 2 : s - 1;
        size_t ob = oe;
        while (ob > 0 && is_id_char(t[ob - 1]))
            --ob;
        const std::string obj = t.substr(ob, oe - ob);
        if (obj == "this")
        {
            if (const hotc::AnalyzedClass* cls = class_at(cur))
                add_members(cls->name);
        }
        else if (scope.class_statics.count(obj)
            || std::any_of(imported.begin(), imported.end(), [&](const auto& f)
                { return std::any_of(f->classes.begin(), f->classes.end(), [&](const hotc::ImportClass& c) { return c.name == obj; }); }))
        {
            add_statics(obj);
        }
        else if (std::any_of(imported.begin(), imported.end(), [&](const auto& f)
                     { return std::any_of(f->enums.begin(), f->enums.end(), [&](const hotc::ImportEnum& e) { return e.name == obj; }); }))
        {
            for (const auto& f : imported)
                for (const hotc::ImportEnum& e : f->enums)
                    if (e.name == obj)
                        for (const std::string& m : e.enumerators)
                            add(m, 20, "enumerator of " + obj, "0");
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
            // A field of the class: a field initializer has no generated place for the dot.
            if (type.empty())
                if (const hotc::AnalyzedClass* cls = class_at(cur))
                    if (const auto f = scope.class_fields.find(cls->name); f != scope.class_fields.end())
                        for (const ScopeName& n : f->second)
                            if (n.name == obj && type.empty())
                                type = n.type;
            if (!type.empty())
                add_members(type);
        }
    }
    else if (!member)
    {
        // Inside a method: the members of the class and of its script bases. The native members of
        // a base come from clangd below.
        if (const hotc::AnalyzedClass* cls = class_at(cur); cls && !type_only)
        {
            add_members(cls->name);
            add_statics(cls->name);
        }
        for (const ScopeName& n : scope.locals)
            if (!type_only)
                add(n.name, 6, n.detail + (n.type.empty() ? "" : ": " + n.type), "0");
        for (const ScopeName& n : scope.module_vars)
            if (!type_only)
                add(n.name, 6, n.detail + (n.type.empty() ? "" : ": " + n.type), "0");
        for (const std::string& n : scope.enums)
            add(n, 13, "script enum", "1");
        // The class that the base list declares is not a base of itself.
        std::string declared;
        if (base_only)
        {
            size_t c = s;
            while (c > 0 && t[c - 1] != ':' && t[c - 1] != '{' && t[c - 1] != ';' && t[c - 1] != '}')
                --c;
            if (c > 0 && t[c - 1] == ':')
            {
                size_t b = c - 1;
                while (b > 0 && std::isspace(static_cast<unsigned char>(t[b - 1])))
                    --b;
                size_t a = b;
                while (a > 0 && is_id_char(t[a - 1]))
                    --a;
                declared = t.substr(a, b - a);
            }
        }
        for (const std::string& n : scope.classes)
            if (n != declared)
                add(n, 7, "script class", "1");
        for (const std::string& n : scope.structs)
            add(n, 22, "script struct", "1");
        for (const std::string& n : scope.interfaces)
            add(n, 8, "script interface", "1");
        for (size_t fi = 0; fi < imported.size(); ++fi)
        {
            const auto& f = imported[fi];
            for (const hotc::ImportClass& c : f->classes)
                add(c.name, c.is_interface ? 8 : c.is_struct ? 22 : 7,
                    c.is_interface ? "imported interface" : c.is_struct ? "imported struct" : "imported class", "1");
            for (const hotc::ImportEnum& e : f->enums)
                add(e.name, 13, "imported enum", "1");
            for (const hotc::ImportVar& v : f->vars)
                if (!type_only && (v.is_const || imported_depth[fi] == 0))
                    add(v.name, v.is_const ? 21 : 6, std::string(v.is_const ? "imported const" : "imported variable") + ": " + v.type, "0");
            for (const hotc::ImportFunction& fn : f->functions)
                if (!type_only && imported_depth[fi] == 0)
                    add(fn.name, 3, fn.type + fn.signature + " (imported)", "0");
        }
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
            const bool comment = n + 1 < t.size() && t[n] == '/' && (t[n + 1] == '/' || t[n + 1] == '*');
            const bool end = !comment && n < t.size()
                && std::string_view(";),]([+-*/%=<>!&|^?:.").find(t[n]) != std::string_view::npos;
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
            // The angle brackets that the statement leaves open after a name, as in "a.b<c.".
            std::string angles;
            for (size_t i = s; i > 0 && t[i - 1] != ';' && t[i - 1] != '{' && t[i - 1] != '}'; --i)
            {
                if (t[i - 1] == '>' && i > 1 && is_id_char(t[i - 2]))
                    angles += '<';
                else if (t[i - 1] == '<' && i > 1 && is_id_char(t[i - 2]))
                {
                    if (!angles.empty() && angles.back() == '<')
                        angles.pop_back();
                    else
                        angles += '>';
                }
            }
            // The first text is the plain hole. When it gives no answer, the hole can be in a type
            // position (a field, a parameter, a generic argument), where a declaration needs a
            // name. The next texts add a dummy name.
            const std::string before = t.substr(0, s) + prefix + kHole;
            std::vector<std::string> texts = {before + closers + (end ? "" : ";") + t.substr(e)};
            if (!end)
            {
                texts.push_back(before + closers + t.substr(e)); // a name follows: "a.b F()"
                texts.push_back(before + angles + " hot_hole_" + closers + ";" + t.substr(e));
                if (!closers.empty())
                    texts.push_back(before + angles + " hot_hole_" + closers + " {}" + t.substr(e));
            }
            // A type place: the text with a dummy name comes first, the hole is a type in it.
            if (type_only)
            {
                std::vector<std::string> typed = {before + angles + " hot_hole_" + closers + (end ? "" : ";") + t.substr(e)};
                if (!end && !closers.empty())
                    typed.push_back(before + angles + " hot_hole_" + closers + " {}" + t.substr(e));
                typed.insert(typed.end(), texts.begin(), texts.end());
                texts = std::move(typed);
            }
            {
                std::vector<std::string> unique;
                for (std::string& x : texts)
                    if (std::find(unique.begin(), unique.end(), x) == unique.end())
                        unique.push_back(std::move(x));
                texts = std::move(unique);
            }
            // The last try of a type place: the word is a builtin type, so the text translates, and
            // a module variable at the file end holds the hole as a type. Its place has no map.
            size_t probe_index = texts.size();
            const bool probe = ctx.kind == CompletionContext::Kind::Type || ctx.kind == CompletionContext::Kind::Base;
            if (probe && ctx.kind == CompletionContext::Kind::Type)
                texts.push_back(t.substr(0, s) + "int" + closers + (end ? "" : ";") + t.substr(e) + "\n" + prefix + kHole
                    + " hot_hole_;\n");
            else if (probe)
            {
                // A base list: the list goes away, the text translates.
                size_t colon = s;
                while (colon > 0 && t[colon - 1] != ':' && t[colon - 1] != '{' && t[colon - 1] != ';' && t[colon - 1] != '}')
                    --colon;
                // The probe is the first try: a base list gives no native type in the other texts.
                if (colon > 0 && t[colon - 1] == ':')
                {
                    // The hole is a field type in the body of the class: clangd gives the native
                    // types there. With no body, the hole is a module variable at the file end.
                    const size_t brace = t.find('{', e);
                    texts.insert(texts.begin(),
                        brace != std::string::npos
                            ? t.substr(0, colon - 1) + " " + t.substr(e, brace + 1 - e) + "\n" + prefix + kHole
                                + " hot_hole_;" + t.substr(brace + 1)
                            : t.substr(0, colon - 1) + " " + t.substr(e) + "\n" + prefix + kHole + " hot_hole_;\n");
                    probe_index = 0;
                }
            }
            const Pos hole = {cur.line, static_cast<int>(s - d.text.starts[static_cast<size_t>(cur.line)])};
            std::optional<json> r;
            bool mapped = false; // a try had a place for the hole
            std::optional<std::chrono::steady_clock::time_point> t0; // the start of the clangd waits
            bool sent = false; // the generated text of a try is in clangd
            bool lite_sent = false; // the text of a try is in the lite document
            Doc& md = const_cast<Doc&>(d);
            // The real generated text goes back once, also when a request throws.
            struct Restore
            {
                std::function<void()> f;
                ~Restore() { f(); }
            } restore{[&]
                {
                    if (!sent)
                        return;
                    std::lock_guard<std::mutex> lock(docs_mutex_);
                    md.gen_version = md.sent_version + 1; // before the send: the diagnostics can come at once
                    send_generated(md, d.gen.s);
                }};
            for (size_t text_index = 0; text_index < texts.size(); ++text_index)
            {
                const std::string& patched = texts[text_index];
                // A completion needs no null check: the statement ends at the hole, so a local that
                // is null at this place must not stop the translation.
                hotc::TranslateOptions to = translate_options(d);
                to.null_warnings = true;
                hotc::AnalyzeResult facts = [&] { PhaseTimer pt("round analyze"); return timed_translate([&] { return hotc::analyze(patched, to, session_); }); }();
                const bool is_probe = probe && text_index == probe_index;
                std::optional<hotc::SourcePos> g = facts.cpp.empty() || is_probe
                    ? std::nullopt
                    : facts.source_map.map_to_generated(hole.line + 1, hole.col + 1);
                // A type text has no map of its own place (a field, a parameter, a return type). The
                // generated code holds the hole name after "::" there.
                if (!g && !facts.cpp.empty())
                {
                    const size_t o = facts.cpp.find("::" + prefix + kHole);
                    if (o != std::string::npos)
                    {
                        const auto lc = Text(facts.cpp).pos(o + 2);
                        g = hotc::SourcePos{lc.first + 1, lc.second + 1};
                    }
                }
                // A bare partial word of a type ("Nod" of a type position) has no "::" before it.
                if (!g && !facts.cpp.empty() && (type_only || is_probe))
                {
                    for (size_t o = facts.cpp.find(prefix + kHole); o != std::string::npos;
                         o = facts.cpp.find(prefix + kHole, o + 1))
                        if (o == 0 || !is_id_char(facts.cpp[o - 1]))
                        {
                            const auto lc = Text(facts.cpp).pos(o);
                            g = hotc::SourcePos{lc.first + 1, lc.second + 1};
                            break;
                        }
                }
                if (!g)
                    continue;
                mapped = true;
                const Text gen(facts.cpp);
                // Before its first preamble, clangd gives only "fallback" words of the file. A
                // request that needs the AST waits for the preamble. The wait is on the real text,
                // before the patched text goes to clangd: a new text would restart the preamble.
                // The wait has a budget. After it the reply has the script facts and "isIncomplete".
                // The wait for the real text uses the budget of the options. Then clangd has its
                // preamble, and the AST of the patched text needs kCompletionAstMs (std headers make it slow).
                // One deadline for all the tries of the request: the budget of the options for the
                // real text, then kCompletionAstMs more for the patched text.
                if (!t0)
                    t0 = std::chrono::steady_clock::now();
                const auto ready_end = *t0 + std::chrono::milliseconds(opt_.completion_budget_ms);
                const auto budget_end = ready_end + std::chrono::milliseconds(kCompletionAstMs);
                auto budget_left = [&]
                {
                    return std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        budget_end - std::chrono::steady_clock::now()).count()));
                };
                // The lite document has the patched text of the own body only. The full document is not touched, so
                // the request does not wait for its AST.
                if (std::chrono::steady_clock::now() < budget_end)
                {
                    std::string lu;
                    { PhaseTimer pt("round lite sync"); lu = lite_uri(md, facts.cpp, g->line - 1, budget_left()); }
                    if (lu != d.gen_uri)
                    {
                        lite_sent = true;
                        PhaseTimer pc("round clangd call");
                        r = clangd_result("textDocument/completion",
                            {{"textDocument", {{"uri", lu}}},
                                {"position",
                                    to_lsp(gen, {g->line - 1, g->col - 1 + static_cast<int>(prefix.size())}, clangd_utf8_)}},
                            budget_left());
                        if (r && (r->is_array() ? !r->empty() : !r->value("items", json::array()).empty()))
                            break;
                        continue;
                    }
                }
                if (std::chrono::steady_clock::now() >= budget_end || !wait_for_native_ready_until(d, ready_end))
                    break;
                sent = true;
                send_generated(md, facts.cpp);
                // The first patched text of a document waits for its AST (the budget holds).
                const bool first_sync = completion_synced_.insert(d.gen_uri).second;
                if (first_sync
                    && !clangd_result("textDocument/documentSymbol", {{"textDocument", {{"uri", d.gen_uri}}}}, budget_left()))
                {
                    completion_synced_.erase(d.gen_uri);
                    break;
                }
                if (first_sync)
                    wait_for_preamble_index(std::min(5000, budget_left()));
                r = clangd_result("textDocument/completion",
                    {{"textDocument", {{"uri", lite_uri(md, facts.cpp, g->line - 1, budget_left())}}},
                        {"position",
                            to_lsp(gen, {g->line - 1, g->col - 1 + static_cast<int>(prefix.size())}, clangd_utf8_)}},
                    budget_left());
                const bool got = r && (r->is_array() ? !r->empty() : !r->value("items", json::array()).empty());
                if (got)
                    break;
            }
            // The native members of the base are not names in the generated code of a method. A second
            // try with "this." gives them. Its items join the list.
            if (!member && !type_only && (sent || lite_sent) && r && class_at(cur))
            {
                const std::string with_this = t.substr(0, s) + "this." + prefix + kHole + closers + (end ? "" : ";") + t.substr(e);
                hotc::TranslateOptions to = translate_options(d);
                to.null_warnings = true;
                hotc::AnalyzeResult facts = [&] { PhaseTimer pt("this analyze"); return timed_translate([&] { return hotc::analyze(with_this, to, session_); }); }();
                const auto g = facts.cpp.empty()
                    ? std::nullopt
                    : facts.source_map.map_to_generated(hole.line + 1, hole.col + 1 + 5);
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *t0 + std::chrono::milliseconds(opt_.completion_budget_ms + kCompletionAstMs) - std::chrono::steady_clock::now()).count();
                if (g && left > 0)
                {
                    std::string lu;
                    { PhaseTimer pt("this lite sync"); lu = lite_uri(md, facts.cpp, g->line - 1, static_cast<int>(left)); }
                    if (lu == d.gen_uri)
                        send_generated(md, facts.cpp);
                    const Text gen(facts.cpp);
                    PhaseTimer pc("this clangd call");
                    auto r2 = clangd_result("textDocument/completion",
                        {{"textDocument", {{"uri", lu}}},
                            {"position",
                                to_lsp(gen, {g->line - 1, g->col - 1 + static_cast<int>(prefix.size())}, clangd_utf8_)}},
                        static_cast<int>(left));
                    if (r2)
                    {
                        json merged = r->is_array() ? *r : r->value("items", json::array());
                        for (const json& it : r2->is_array() ? *r2 : r2->value("items", json::array()))
                            merged.push_back(it);
                        r = merged;
                    }
                }
            }
            // No patched text translates (an error that the analysis cannot cut out). The last good
            // generated text is in clangd. Ask at the nearest place of the old text that has a map:
            // the hole, then the earlier positions of the line, then the earlier lines. A member
            // completion asks only at the hole. No patched text goes to clangd here.
            if (!mapped && d.map && !d.gen.s.empty())
            {
                std::optional<Pos> g = to_gen(d, hole);
                // The start of a statement line has a map. The lines before the hole come first.
                auto line_start = [&](int l) -> std::optional<Pos>
                {
                    const std::string_view ln = d.text.line(l);
                    const size_t c = ln.find_first_not_of(" \t");
                    return c == std::string_view::npos ? std::nullopt : to_gen(d, {l, static_cast<int>(c)});
                };
                const int last_line = std::min(static_cast<int>(d.text.starts.size()) - 1, cur.line + 30);
                for (int l = cur.line - (g ? 99 : 0); !member && !g && l >= std::max(0, cur.line - 30); --l)
                    g = line_start(l);
                // The edit can cover the lines before the hole: then the next mapped lines.
                for (int l = cur.line + 1; !member && !g && l <= last_line; ++l)
                    g = line_start(l);
                if (g)
                {
                    const auto stale_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt_.completion_budget_ms);
                    if (wait_for_native_ready_until(d, stale_end))
                    {
                        const auto stale_left = [&]
                        {
                            return std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                stale_end - std::chrono::steady_clock::now()).count()));
                        };
                        const json comp_params = {{"textDocument", {{"uri", lite_uri(d, d.gen.s, g->line, stale_left())}}},
                            {"position", gen_pos(d, *g)}};
                        r = clangd_result("textDocument/completion", comp_params, stale_left());
                        if (r)
                            lite_check("textDocument/completion", comp_params, d, *r, "completion");
                    }
                    mapped = true; // no answer in time: the reply is incomplete
                }
            }
            restore.f();
            sent = false;
            cc.rest = rest;
            cc.prefix = prefix;
            cc.valid = true;
            cc.list = r ? (r->is_array() ? *r : r->value("items", json::array())) : json::array();
            // A translation that fails stays failed while the prefix grows. An empty answer is not
            // complete: clangd gives the names of the headers from its index, and it fills the index
            // after the preamble. An early answer can be empty, so the next request asks again.
            cc.complete = !mapped || (r && !cc.list.empty() && !(r->is_object() && r->value("isIncomplete", false)));
        }
        incomplete = !cc.complete;
        for (const json& it : cc.list)
        {
            std::string name = it.value("filterText", std::string());
            if (name.empty())
                name = it.value("insertText", it.value("label", std::string()));
            const int kind = it.value("kind", 1);
            // The names of the generated code and of the shell are not script names.
            if (name.empty() || name.find(kHole) != std::string::npos || is_generated_name(name) || is_generated_class_name(d, name)
                || name.compare(0, 8, "operator") == 0 || name[0] == '~' || kind == 4
                || name.find("::") != std::string::npos)
                continue;
            if (name == "cls" || name == "state")
                continue; // hot::ShellBase (runtime/shell.h)
            // The members of the generated base structs. A script member with such a name is in
            // "names" already, so it stays.
            static const std::set<std::string> base_internal = {"creator", "live_next", "live_prev", "shared_from_this",
                "enable_shared_from_this", "weak_from_this"};
            if (base_internal.count(name) && !names.count(name))
                continue;
            if (!member)
            {
                // A name in scope: a variable, a function, a type, an enumerator, a namespace.
                // Also the members of the class and of its native bases: methods, fields, properties.
                static const std::set<int> kinds = {2, 3, 5, 6, 7, 9, 10, 13, 20, 21, 22};
                static const std::set<int> type_kinds = {7, 8, 9, 13, 22, 25};
                if (type_only && !type_kinds.count(kind))
                    continue;
                if (!kinds.count(kind) || name.compare(0, 2, "__") == 0 || is_libc_name(name))
                    continue;
            }
            std::string label = it.value("label", name);
            while (!label.empty() && (label[0] == ' '))
                label.erase(0, 1);
            if (label.compare(0, 3, "\xE2\x80\xA2") == 0)
                label.erase(0, 3); // the include bullet
            if (!has_prefix(name, prefix))
                continue;
            // A name from a namespace comes as "std::string": the script form is "std.string".
            std::string insert = it.value("insertText", name);
            insert = insert.find("::") != std::string::npos ? hotc::rewrite_generated_names(insert) : name;
            // The namespace hot is the runtime of the generated code. Its names do not hide a member.
            if (!member && insert.compare(0, 4, "hot.") == 0)
                continue;
            if (!names.insert(name).second)
                continue;
            json item = {{"label", hotc::rewrite_generated_names(label)}, {"kind", kind}, {"insertText", insert},
                {"filterText", name}, {"sortText", "2" + it.value("sortText", name)}};
            if (it.contains("detail"))
                item["detail"] = hotc::rewrite_generated_names(it.value("detail", std::string()));
            items.push_back(std::move(item));
        }
    }

    // ---- the keywords and the builtin types
    if (!member && !base_only)
    {
        // "self" is a stub attribute. It is also the name of a generated helper, so it is not offered.
        // A type place has no keyword. A declaration start has the declaration keywords, not the
        // statement keywords: no statement starts in a file, a namespace, a class or a parameter list.
        static const std::set<std::string> statement_keywords = {"if", "else", "while", "do", "for", "foreach",
            "return", "break", "continue", "switch", "case", "default", "try", "catch", "finally", "throw", "await",
            "delete", "static_assert"};
        const bool decl_start = type_only && ctx.declStart;
        for (const std::string& k : script_keywords())
            if (k != "self" && (!type_only || (decl_start && !statement_keywords.count(k))))
                add(k, 14, "keyword", "3");
        // The global helpers of a script (include/hot/runtime/enumerate.h).
        for (const std::string& h : script_helpers())
            if (!type_only)
                add(h, 3, "global function", "3");
        for (const std::string& k : builtin_type_names())
            add(k, 25, "builtin type", "3");
    }
    return {{"isIncomplete", incomplete}, {"items", items}};
}


std::optional<Server::StaticHit> Server::static_member_at(const Doc& d, Pos cur) const
{
    if (cur.line < 0 || cur.line >= static_cast<int>(d.text.starts.size()))
        return std::nullopt;
    const auto [wb, we] = word_at(d, cur);
    if (we <= wb)
        return std::nullopt;
    const std::string_view l = d.text.line(cur.line);
    const std::string name(l.substr(static_cast<size_t>(wb), static_cast<size_t>(we - wb)));
    auto find_class = [&](const std::string& n) -> const hotc::AnalyzedClass*
    {
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            if (c.name == n)
                return &c;
        return nullptr;
    };
    // The class and its script bases, nearest first.
    auto chain = [&](const hotc::AnalyzedClass* c)
    {
        std::vector<const hotc::AnalyzedClass*> out;
        for (; c && out.size() < 32; c = find_class(c->base))
            out.push_back(c);
        return out;
    };
    // The code of the whole script: a comment, a string and a character literal are spaces. The
    // line "l" starts at "off[l]".
    std::string code;
    std::vector<size_t> off;
    {
        bool block = false;
        for (int i = 0; i < static_cast<int>(d.text.starts.size()); ++i)
        {
            off.push_back(code.size());
            code += blank_non_code(d.text.line(i), block);
            code += '\n';
        }
    }
    const size_t co = off[static_cast<size_t>(cur.line)] + static_cast<size_t>(wb);
    if (co >= code.size() || code[co] == ' ')
        return std::nullopt; // a word in a comment or in a string
    const hotc::AnalyzedClass* start = nullptr;
    bool bare = false;
    size_t boundary = 0, scope_end = 0;
    bool in_function = false;
    int b = wb;
    while (b > 0 && (l[static_cast<size_t>(b) - 1] == ' ' || l[static_cast<size_t>(b) - 1] == '\t'))
        --b;
    if (b > 0 && l[static_cast<size_t>(b) - 1] == '.')
    {
        int e = b - 1;
        int q = e;
        while (q > 0 && is_id_char(l[static_cast<size_t>(q) - 1]))
            --q;
        start = find_class(std::string(l.substr(static_cast<size_t>(q), static_cast<size_t>(e - q))));
        if (!start)
            return std::nullopt;
    }
    else
    {
        // A bare name: the class that holds the word. The range of a class ends at its closing brace.
        // The facts can be older than the text: a line outside the text gives no answer.
        const hotc::AnalyzedClass* near_class = nullptr;
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            if (c.begin.line - 1 <= cur.line)
                near_class = &c;
        if (!near_class || near_class->begin.line < 1 || near_class->begin.line - 1 >= static_cast<int>(off.size())
            || near_class->begin.col < 1)
            return std::nullopt;
        const size_t cb = off[static_cast<size_t>(near_class->begin.line - 1)] + static_cast<size_t>(near_class->begin.col - 1);
        const size_t open = cb < code.size() ? code.find('{', cb) : std::string::npos;
        if (open == std::string::npos || open > co)
            return std::nullopt;
        // Scan from the class brace to the word. "boundary" is the start of the member at depth 1.
        int depth = 1, parens = 0;
        boundary = open + 1;
        for (size_t i = open + 1; i < co; ++i)
        {
            const char ch = code[i];
            if (ch == '{')
            {
                ++depth;
            }
            else if (ch == '}')
            {
                if (--depth == 0)
                    return std::nullopt; // after the class
                if (depth == 1)
                    boundary = i + 1;
            }
            else if (depth == 1 && ch == '(')
                ++parens;
            else if (depth == 1 && ch == ')')
                --parens;
            else if (depth == 1 && ch == ';')
                boundary = i + 1;
        }
        start = near_class;
        bare = true;
        in_function = depth >= 2 || parens > 0;
        if (in_function)
        {
            scope_end = co;
            int dd = depth;
            for (; scope_end < code.size(); ++scope_end)
            {
                if (code[scope_end] == '{')
                    ++dd;
                else if (code[scope_end] == '}' && --dd <= 1)
                    break;
                else if (code[scope_end] == ';' && dd == 1)
                    break;
            }
        }
    }
    const hotc::AnalyzedMember* found = nullptr;
    for (const hotc::AnalyzedClass* c : chain(start))
    {
        for (const hotc::AnalyzedMember& m : c->statics)
            if (!found && m.name == name)
                found = &m;
        if (found)
            break;
    }
    if (!found)
        return std::nullopt;
    if (bare && in_function)
    {
        // A same-name token that may declare a local or a parameter: the word is perhaps not the
        // static. The test is conservative on purpose: it never guesses.
        static const std::set<std::string> not_a_type = {"return", "case", "new", "delete", "await", "throw",
            "else", "do", "in", "is", "as", "typeof", "default"};
        auto prev_word = [&](size_t q)
        {
            while (q > boundary && (code[q - 1] == ' ' || code[q - 1] == '\t' || code[q - 1] == '\n'))
                --q;
            size_t w = q;
            while (w > boundary && is_id_char(code[w - 1]))
                --w;
            return code.substr(w, q - w);
        };
        for (size_t at = code.find(name, boundary); at != std::string::npos && at < scope_end; at = code.find(name, at + 1))
        {
            if ((at > 0 && is_id_char(code[at - 1])) || is_id_char(code[at + name.size()]))
                continue;
            size_t q = at;
            while (q > boundary && (code[q - 1] == ' ' || code[q - 1] == '\t' || code[q - 1] == '\n'))
                --q;
            if (q <= boundary)
                continue;
            const char pc = code[q - 1];
            bool may_declare = false;
            if (is_id_char(pc))
                may_declare = !not_a_type.count(prev_word(q));
            else if (pc == '>')
            {
                // "->" and "=>" are not a type end. A ">" next to a name ends a template list.
                size_t r = q - 1;
                while (r > boundary && code[r - 1] == '>')
                    --r;
                may_declare = r > boundary && code[r - 1] != '-' && code[r - 1] != '=' && (is_id_char(code[r - 1]) || code[r - 1] == '>');
            }
            else if (pc == '&' || pc == '*')
            {
                // "Type * name" is a declaration only at a statement start and with a declarator end.
                // "a * name + 1" and "x & name" are uses of the word.
                size_t e = at + name.size();
                while (e < code.size() && (code[e] == ' ' || code[e] == '\t' || code[e] == '\n'))
                    ++e;
                const bool eq_end = e < code.size() && code[e] == '=' && (e + 1 >= code.size() || code[e + 1] != '=');
                const bool declarator_end = eq_end
                    || (e < code.size() && (code[e] == ';' || code[e] == ',' || code[e] == ')' || code[e] == '{' || code[e] == '(' || code[e] == '['));
                size_t r = q - 1;
                while (r > boundary && (code[r - 1] == ' ' || code[r - 1] == '\t' || code[r - 1] == '*' || code[r - 1] == '&'))
                    --r;
                if (declarator_end && r > boundary && (is_id_char(code[r - 1]) || code[r - 1] == '>'))
                {
                    size_t t = r;
                    while (t > boundary && (is_id_char(code[t - 1]) || code[t - 1] == '<' || code[t - 1] == '>'
                        || (code[t - 1] == ':' && ((t >= 2 && code[t - 2] == ':') || code[t] == ':')) || code[t - 1] == ' ' || code[t - 1] == '\t' || code[t - 1] == '\n'
                        || code[t - 1] == '*' || code[t - 1] == '&'))
                        --t;
                    size_t f = t;
                    while (f < r && std::isspace(static_cast<unsigned char>(code[f])))
                        ++f;
                    size_t fe = f;
                    while (fe < r && is_id_char(code[fe]))
                        ++fe;
                    const std::string first = code.substr(f, fe - f);
                    if (!first.empty() && !not_a_type.count(first))
                    {
                        // A statement start: a boundary, ";", "{", "}" or a list start "(" and ",".
                        size_t s = t;
                        while (s > boundary && std::isspace(static_cast<unsigned char>(code[s - 1])))
                            --s;
                        if (s <= boundary)
                            may_declare = true;
                        else if (code[s - 1] == ';' || code[s - 1] == '{' || code[s - 1] == '}' || code[s - 1] == ','
                            || (code[s - 1] == ':' && !(s >= 2 && code[s - 2] == ':')))
                            may_declare = true; // a label colon ("case 1:", "default:") starts a statement
                        else if (code[s - 1] == '(')
                        {
                            const std::string w = prev_word(s - 1);
                            if (w == "if" || w == "while" || w == "switch")
                                may_declare = eq_end; // an init-declaration: "if (Foo* name = get())"
                            else
                                may_declare = w != "return" && w != "case";
                        }
                    }
                }
            }
            else if (pc == ',' || pc == '[')
            {
                // A declaration list ("int a = 0, count = 1;") or a binding ("auto [i, count]").
                // A statement start: after the last ";", "{" or "}" before the word.
                size_t st = at;
                while (st > boundary && code[st - 1] != ';' && code[st - 1] != '{' && code[st - 1] != '}')
                    --st;
                int par = 0, sq = 0;
                size_t open_sq = std::string::npos;
                for (size_t i = st; i < at; ++i)
                {
                    if (code[i] == '(')
                        ++par;
                    else if (code[i] == ')')
                        --par;
                    else if (code[i] == '[')
                    {
                        ++sq;
                        open_sq = i;
                    }
                    else if (code[i] == ']')
                        --sq;
                }
                if (sq > 0 && open_sq != std::string::npos)
                {
                    const std::string w = prev_word(open_sq);
                    may_declare = w == "auto" || w == "var" || w == "let";
                }
                else if (par == 0)
                {
                    // Two names in a row at the statement start: a type and a declarator.
                    size_t i = st;
                    while (i < at && std::isspace(static_cast<unsigned char>(code[i])))
                        ++i;
                    size_t j = i;
                    while (j < at && (is_id_char(code[j]) || code[j] == '<' || code[j] == '>' || code[j] == ':' || code[j] == '*' || code[j] == '&'))
                        ++j;
                    size_t k = j;
                    while (k < at && std::isspace(static_cast<unsigned char>(code[k])))
                        ++k;
                    may_declare = j > i && k > j && k < at && (std::isalpha(static_cast<unsigned char>(code[k])) || code[k] == '_');
                }
            }
            if (!may_declare)
            {
                // A "for (" header can declare a name in any form.
                size_t st = at;
                int par = 0;
                for (; st > boundary; --st)
                {
                    if (code[st - 1] == ')')
                        ++par;
                    else if (code[st - 1] == '(' && par-- == 0)
                        break;
                    else if (code[st - 1] == ';' && par == 0)
                    {
                        // "for (a; b; c)": a ";" inside the header: go on to the "(".
                    }
                    else if ((code[st - 1] == '{' || code[st - 1] == '}') && par == 0)
                    {
                        st = boundary;
                        break;
                    }
                }
                if (st > boundary)
                {
                    const std::string w = prev_word(st - 1);
                    may_declare = w == "for" || w == "foreach";
                }
            }
            if (may_declare)
                return std::nullopt;
        }
    }
    return StaticHit{{found->begin.line - 1, found->begin.col - 1}, found};
}

json Server::on_hover(const Doc& d, const json& p)
{
    const Pos here = from_lsp(d.text, p["position"], utf8_);
    // A static member of a script class has no map segment: the script gives the answer.
    // The table gives the declaration line. The text scan serves only the degraded mode.
    auto fenced = [&](std::string t)
    {
        const size_t f = t.find_first_not_of(" \t");
        t = f == std::string::npos ? std::string() : t.substr(f);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\r'))
            t.pop_back();
        const auto [wb, we] = word_at(d, here);
        return json{{"contents", {{"kind", "markdown"}, {"value", "```hot\n" + t + "\n```"}}},
            {"range", script_range(d, {here.line, wb}, {here.line, we})}};
    };
    if (has_no_table(d))
    {
        if (const auto hit = static_member_at(d, here))
        {
            std::string t(d.text.line(hit->decl.line));
            const size_t cut = t.find(hit->member->kind == "static method" ? '{' : ';');
            if (cut != std::string::npos)
                t.resize(cut);
            return fenced(std::move(t));
        }
    }
    else if (const auto r = resolve_at(d, here))
    {
        const hotc::RefKind k = r->ref.kind;
        const bool is_static = k == hotc::RefKind::StaticMethod || k == hotc::RefKind::StaticField
            || (k == hotc::RefKind::Const && r->ref.decl.find('.') != std::string::npos);
        if (is_static)
        {
            Text decl_text;
            const auto table = file_refs(r->ref.decl_file, nullptr, &decl_text);
            const hotc::ResolvedRef* decl = table ? decl_of(*table, r->ref.id) : nullptr;
            if (decl && decl->offset <= decl_text.s.size())
            {
                const auto [line, col] = decl_text.pos(decl->offset);
                (void)col;
                std::string t(decl_text.line(line));
                const size_t cut = t.find(k == hotc::RefKind::StaticMethod ? '{' : ';');
                if (cut != std::string::npos)
                    t.resize(cut);
                return fenced(std::move(t));
            }
        }
    }
    // A word in a comment or in a string is not a symbol: no hover.
    {
        const auto [wb, we] = word_at(d, here);
        bool block = false;
        const std::string code = blank_non_code(d.text.line(here.line), block);
        if (we > wb && static_cast<size_t>(wb) < code.size() && code[static_cast<size_t>(wb)] == ' ')
            return nullptr;
    }
    bool by_word = false;
    const std::vector<Pos> asks = gen_asks(d, here, by_word);
    if (!asks.empty())
        wait_for_native_ready(d);
    // The asks of a word share a short time budget (see on_locations).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNativeBudgetMs);
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
        // The open of the lite document uses part of the budget. A time-out gives the full document.
        const std::string target = lite_uri(d, d.gen.s, g.line, timeout_ms);
        if (by_word)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0)
                break;
            timeout_ms = static_cast<int>(left);
        }
        const json hover_params = {{"textDocument", {{"uri", target}}}, {"position", gen_pos(d, g)}};
        auto r = clangd_result("textDocument/hover", hover_params, timeout_ms);
        if (r)
            lite_check("textDocument/hover", hover_params, d, *r, "hover");
        if (!r || !r->contains("contents"))
            continue;
        json c = (*r)["contents"];
        if (c.is_object() && c.contains("value"))
            c["value"] = hotc::rewrite_generated_names(c.value("value", std::string()));
        else if (c.is_string())
            c = hotc::rewrite_generated_names(c.get<std::string>());
        json out = {{"contents", c}};
        const auto [wb, we] = word_at(d, here);
        if (by_word)
        {
            out["range"] = script_range(d, {here.line, wb}, {here.line, we});
        }
        else if (r->contains("range"))
        {
            const auto b = to_script(d, (*r)["range"]["start"]);
            if (b)
            {
                const auto [rb, re] = word_at(d, *b);
                out["range"] = script_range(d, {b->line, rb}, {b->line, re});
            }
        }
        return out;
    }
    return nullptr;
}

// The call that holds the cursor. "open" is the "(" of the call, "name_begin" the first byte of
// the callee name. "closers" are the closing brackets that complete the text up to the cursor.
struct SigCall
{
    size_t open = 0, name_begin = 0;
    int commas = 0;
    std::string closers;
};

// The "<" at "lt" is a template list when it follows a name in a plausible place: the name has
// a capital letter, or the name follows a ".", or a ">" and a "(" close the list on the same
// statement with only type characters between.
static bool plausible_template(const std::string& code, size_t lt, size_t limit)
{
    size_t e = lt;
    while (e > 0 && (code[e - 1] == ' ' || code[e - 1] == '\t'))
        --e;
    size_t b = e;
    while (b > 0 && is_id_char(code[b - 1]))
        --b;
    if (b == e || std::isdigit(static_cast<unsigned char>(code[b])))
        return false;
    if (e != lt)
        return false;
    if (std::isupper(static_cast<unsigned char>(code[b])))
        return true;
    size_t k = b;
    while (k > 0 && (code[k - 1] == ' ' || code[k - 1] == '\t'))
        --k;
    if (k > 0 && code[k - 1] == '.')
        return true;
    int depth = 0;
    for (size_t i = lt; i < code.size(); ++i)
    {
        const char c = code[i];
        if (c == '<')
            ++depth;
        else if (c == '>' && --depth == 0)
        {
            while (++i < code.size() && (code[i] == ' ' || code[i] == '\t'))
            {
            }
            // A ">" or a "," after the list: this list is in an outer list.
            return i < code.size() && (code[i] == '(' || code[i] == '>' || code[i] == ',');
        }
        else if (!is_id_char(c) && std::string_view(" \t\n:,*&").find(c) == std::string_view::npos)
            return false;
    }
    (void)limit;
    return false;
}

// A scan of the script text from the start to the cursor. Strings and comments are blank. The
// scan tracks "()", "[]", "{}" and the template lists "<>" in a plausible place. A comma counts
// for the innermost bracket only. Braces do not hide the call around them. No value when the
// cursor is not in the arguments of a call.
static std::optional<SigCall> find_call_at(const std::string& text, size_t cursor)
{
    std::string code;
    {
        bool block = false;
        size_t i = 0;
        while (i < cursor)
        {
            size_t e = text.find('\n', i);
            const size_t end = e == std::string::npos ? text.size() : e;
            code += blank_non_code(std::string_view(text).substr(i, end - i), block);
            if (e == std::string::npos)
                break;
            code += '\n';
            i = e + 1;
        }
        code.resize(std::min(code.size(), cursor));
    }
    struct Frame
    {
        char kind;
        size_t open, name_begin;
        int commas;
        bool semi;
    };
    std::vector<Frame> st;
    // The name that starts the template list that ended with the ">" at "gt".
    size_t last_gt = std::string::npos, last_gt_name = 0;
    auto name_start = [&](size_t end)
    {
        size_t b = end;
        while (b > 0 && is_id_char(code[b - 1]))
            --b;
        return b;
    };
    for (size_t i = 0; i < code.size(); ++i)
    {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{')
        {
            size_t e = i;
            while (e > 0 && (code[e - 1] == ' ' || code[e - 1] == '\t'))
                --e;
            size_t nb = e;
            if (c == '(' && e > 0)
            {
                if (code[e - 1] == '>' && e - 1 == last_gt)
                    nb = last_gt_name;
                else
                    nb = name_start(e);
            }
            st.push_back({c, i, nb == e ? std::string::npos : nb, 0, false});
        }
        else if (c == ')' || c == ']' || c == '}')
        {
            const char want = c == ')' ? '(' : c == ']' ? '[' : '{';
            while (!st.empty() && st.back().kind == '<')
                st.pop_back();
            if (!st.empty() && st.back().kind == want)
                st.pop_back();
        }
        else if (c == '<')
        {
            if (plausible_template(code, i, code.size()))
            {
                size_t e = i;
                st.push_back({'<', i, name_start(e), 0, false});
            }
        }
        else if (c == '>' && !(i > 0 && code[i - 1] == '-'))
        {
            if (!st.empty() && st.back().kind == '<')
            {
                last_gt = i;
                last_gt_name = st.back().name_begin;
                st.pop_back();
            }
        }
        else if (c == ',')
        {
            if (!st.empty())
                ++st.back().commas;
        }
        else if (c == ';')
        {
            while (!st.empty() && st.back().kind == '<')
                st.pop_back();
            if (!st.empty())
                st.back().semi = true;
        }
    }
    // A "<" that no ">" closes is a comparison: its commas belong to the bracket around it.
    while (!st.empty() && st.back().kind == '<')
    {
        const int commas = st.back().commas;
        st.pop_back();
        if (!st.empty())
            st.back().commas += commas;
    }
    // The innermost "(" with a name: braces are transparent unless they hold a statement.
    size_t k = st.size();
    while (k > 0)
    {
        const Frame& f = st[k - 1];
        if (f.kind == '{' && !f.semi)
            --k;
        else
            break;
    }
    if (k == 0 || st[k - 1].kind != '(' || st[k - 1].name_begin == std::string::npos)
        return std::nullopt;
    SigCall r;
    r.open = st[k - 1].open;
    r.name_begin = st[k - 1].name_begin;
    r.commas = st[k - 1].commas;
    for (size_t j = st.size(); j > k - 1; --j)
    {
        const char kind = st[j - 1].kind;
        if (kind == '(')
            r.closers += ')';
        else if (kind == '[')
            r.closers += ']';
        else if (kind == '{')
            r.closers += '}';
    }
    return r;
}

// The signature of the function that the cursor is in. The call is in the script. clangd gets the
// place after the "(" of the generated call. The active parameter is the number of commas
// between the "(" and the cursor, so it does not depend on the generated arguments. When the
// text does not translate (an unfinished call), the text gets a hole name and the closing
// brackets, as in a completion, and clangd gets the generated text of that patched text.
json Server::on_signature_help(const Doc& d, const json& p)
{
    const std::string& t = d.text.s;
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    const size_t cursor = d.text.offset(cur.line, cur.col);
    const auto call = find_call_at(t, cursor);
    if (!call)
        return nullptr;
    // The "(" of the generated call is the first one after the name.
    auto ask = [&](const Text& gen, Pos g) -> std::optional<json>
    {
        const std::string_view gl = gen.line(g.line);
        const size_t paren = gl.find('(', static_cast<size_t>(g.col));
        if (paren == std::string_view::npos)
            return std::nullopt;
        auto r = clangd_result("textDocument/signatureHelp",
            {{"textDocument", {{"uri", d.gen_uri}}}, {"position", to_lsp(gen, {g.line, static_cast<int>(paren) + 1}, clangd_utf8_)}});
        if (!r || !r->contains("signatures") || (*r)["signatures"].empty())
            return std::nullopt;
        return *r;
    };
    std::optional<json> r;
    const auto [nl, nc] = d.text.pos(call->name_begin);
    if (const auto g = to_gen(d, {nl, nc}))
        r = ask(d.gen, *g);
    if (!r && clangd_ && clangd_->running())
    {
        // The hole goes at the cursor. A try with the text after the cursor, then a try with the
        // closing brackets and the end of the statement.
        const std::string before = t.substr(0, cursor), after = t.substr(cursor);
        const std::vector<std::string> texts = {
            before + "hot_hole_" + call->closers + ";" + after, before + "hot_hole_" + after};
        wait_for_native_ready(d);
        Doc& md = const_cast<Doc&>(d);
        bool sent = false;
        struct Restore
        {
            std::function<void()> f;
            ~Restore() { f(); }
        } restore{[&]
            {
                if (!sent)
                    return;
                std::lock_guard<std::mutex> lock(docs_mutex_);
                md.gen_version = md.sent_version + 1;
                send_generated(md, d.gen.s);
            }};
        for (const std::string& patched : texts)
        {
            hotc::TranslateOptions to = translate_options(d);
            to.null_warnings = true;
            hotc::AnalyzeResult facts = timed_translate([&] { return hotc::analyze(patched, to, session_); });
            if (facts.cpp.empty())
                continue;
            const auto g = facts.source_map.map_to_generated(nl + 1, nc + 1);
            if (!g)
                continue;
            const Text gen(facts.cpp);
            sent = true;
            send_generated(md, facts.cpp);
            r = ask(gen, {g->line - 1, g->col - 1});
            if (r)
                break;
        }
    }
    if (!r)
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
    out["activeParameter"] = call->commas;
    return out;
}

// The options of a format request. The formatter has its own default for the indent width.
static FormatOptions format_options(const json& p)
{
    const json o = p.value("options", json::object());
    FormatOptions f;
    f.indent_width = std::max(o.value("tabSize", 4), 1);
    f.use_tabs = !o.value("insertSpaces", true);
    return f;
}

// The format of the whole script (see format.h). The reply is one edit.
json Server::on_formatting(const Doc& d, const json& p)
{
    const std::string formatted = format_hot(d.text.s, format_options(p));
    if (formatted == d.text.s)
        return json::array();
    const int last = static_cast<int>(d.text.starts.size()) - 1;
    const Pos end = {last, static_cast<int>(d.text.line(last).size())};
    return json::array({{{"range", script_range(d, {0, 0}, end)}, {"newText", formatted}}});
}

// The format of the lines of a range. The formatter runs on the whole text. The edit holds the
// new text of the lines in the range only.
json Server::on_range_formatting(const Doc& d, const json& p)
{
    const Text& t = d.text;
    const int lines = static_cast<int>(t.starts.size());
    const json r = p.value("range", json::object());
    int first = r.value("/start/line"_json_pointer, 0);
    int last = r.value("/end/line"_json_pointer, first);
    if (r.value("/end/character"_json_pointer, 1) == 0 && last > first)
        --last;
    first = std::clamp(first, 0, lines - 1);
    last = std::clamp(last, first, lines - 1);
    const FormatResult f = format_hot_map(t.s, format_options(p));
    if (f.text == t.s)
        return json::array();
    // The lines of the output.
    std::vector<std::string> out;
    for (size_t pos = 0; pos < f.text.size();)
    {
        const size_t e = f.text.find('\n', pos);
        out.push_back(f.text.substr(pos, e == std::string::npos ? std::string::npos : e + 1 - pos));
        pos = e == std::string::npos ? f.text.size() : e + 1;
    }
    int lo = -1, hi = -1;
    for (int k = first; k <= last && k < static_cast<int>(f.line_map.size()); ++k)
    {
        const int m = f.line_map[static_cast<size_t>(k)];
        if (m < 0)
            continue;
        lo = lo < 0 ? m : std::min(lo, m);
        hi = std::max(hi, m);
    }
    std::string text;
    for (int k = lo; lo >= 0 && k <= hi && k < static_cast<int>(out.size()); ++k)
        text += out[static_cast<size_t>(k)];
    Pos end = {last + 1, 0};
    if (last + 1 >= lines)
    {
        end = {last, static_cast<int>(t.line(last).size())};
        // The last line has no new line when it has text. The edit adds none.
        if (end.col > 0)
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
    }
    const size_t from = t.starts[static_cast<size_t>(first)];
    const size_t to = end.line < lines ? t.starts[static_cast<size_t>(end.line)] + static_cast<size_t>(end.col) : t.s.size();
    if (t.s.compare(from, to - from, text) == 0)
        return json::array();
    return json::array({{{"range", script_range(d, {first, 0}, end)}, {"newText", text}}});
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
    auto whole_words = [&w](std::string_view l) { return whole_word_cols(l, w); };
    bool script_block = false;
    const std::vector<int> in_script = whole_words(blank_non_code(d.gen_script.line(old->line), script_block));
    const auto nth = std::find(in_script.begin(), in_script.end(), old->col);
    // The cursor is not on a code word: it is in a comment or in a string.
    if (nth == in_script.end())
        return out;
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

// The script place of a use of "word" at the generated place (line "gl", byte column "gcol") when
// the use has no map segment. "script_lines" is gen_script_lines(d). The k-th use of the word on the
// generated line is the k-th use on the script line when both lines have the same count. A script
// line with one use gives that use. Else the place is not clear and there is no place. A
// parameter or a local with the same name is not a use of the symbol.
std::optional<Pos> Server::script_use_of_gen_word(
    const Doc& d, const std::vector<int>& script_lines, int gl, int gcol, const std::string& word) const
{
    const int sl = gl >= 0 && gl < static_cast<int>(script_lines.size()) ? script_lines[static_cast<size_t>(gl)] : -1;
    const auto t = sl < 0 ? std::nullopt : d.gen_script_to_text({sl, 0});
    if (!t || t->line >= static_cast<int>(d.text.starts.size()))
        return std::nullopt;
    bool block = false, gen_block = false;
    const std::vector<int> in_text = whole_word_cols(blank_non_code(d.text.line(t->line), block), word);
    const std::vector<int> in_gen = whole_word_cols(blank_non_code(d.gen.line(gl), gen_block), word);
    const auto k = std::find(in_gen.begin(), in_gen.end(), gcol) - in_gen.begin();
    if (in_text.size() == 1)
        return Pos{t->line, in_text[0]};
    if (in_text.size() == in_gen.size() && k < static_cast<long>(in_gen.size()))
        return Pos{t->line, in_text[static_cast<size_t>(k)]};
    return std::nullopt;
}

std::vector<Pos> Server::gen_asks(const Doc& d, Pos script_pos, bool& by_word) const
{
    by_word = false;
    if (const auto g = to_gen(d, script_pos))
    {
        // The map can put the start of a script word on a wrapper that the translator adds
        // ("hot::deref(" before "Pnt()"). The generated word is then not the script word: the
        // candidates by word give the right place.
        const std::string w = word(d, script_pos);
        const std::string_view t = d.gen.line(g->line);
        const bool same = !is_symbol_word(w)
            || (static_cast<size_t>(g->col) <= t.size() && t.compare(static_cast<size_t>(g->col), w.size(), w) == 0);
        if (same)
            return {*g};
        std::vector<Pos> found = gen_word_candidates(d, script_pos);
        if (found.empty())
            return {*g};
        by_word = true;
        return found;
    }
    else
    {
        by_word = true;
        return gen_word_candidates(d, script_pos);
    }
    by_word = true;
    return gen_word_candidates(d, script_pos);
}

std::vector<int> Server::gen_script_lines(const Doc& d) const
{
    std::vector<int> out(d.gen.starts.size(), -1);
    int script_line = -1, last_directive = -1;
    bool in_block = false;
    for (int l = 0; l < static_cast<int>(out.size()); ++l)
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
            continue;
        }
        blank_non_code(t, in_block);
        if (script_line >= 0)
            out[static_cast<size_t>(l)] = script_line + (l - last_directive - 1);
    }
    return out;
}

json Server::on_locations(const Doc& d, const json& p, const std::string& method)
{
    json out = json::array();
    const Pos here = from_lsp(d.text, p["position"], utf8_);
    const bool is_ref = method == "textDocument/references";
    const std::string here_word = word(d, here);
    // A script symbol: the table of the resolved names gives the places (resolve_at). No value:
    // the name is not a script symbol, and clangd gives the places (below).
    auto by_table = [&]() -> std::optional<json>
    {
        const auto r = resolve_at(d, here);
        if (!r)
            return std::nullopt;
        const int len = static_cast<int>(r->name.size());
        auto loc = [&](const std::string& uri, const Text& t, Pos s)
        {
            return json{{"uri", uri},
                {"range", {{"start", to_lsp(t, s, utf8_)}, {"end", to_lsp(t, {s.line, s.col + len}, utf8_)}}}};
        };
        // The declaration: a position in the file of the declaration. At the declaration itself
        // it is the cursor.
        Text decl_text;
        const auto decl_table = file_refs(r->ref.decl_file, nullptr, &decl_text);
        const hotc::ResolvedRef* decl = decl_table ? decl_of(*decl_table, r->ref.id) : nullptr;
        const std::shared_ptr<Doc> decl_doc = open_doc_at(r->ref.decl_file);
        const std::string decl_uri = decl_doc ? decl_doc->uri : path_to_uri(r->ref.decl_file);
        std::optional<Pos> decl_pos;
        if (decl && decl->offset <= decl_text.s.size())
        {
            const auto [line, col] = decl_text.pos(decl->offset);
            decl_pos = Pos{line, col};
        }
        if (!is_ref)
        {
            // A method with a closer declaration in the class chain of the receiver (an override):
            // the definition is that declaration, not the root declaration.
            if (!r->ref.near_decl.empty())
            {
                Text near_text;
                const auto near_table = file_refs(r->ref.near_file, nullptr, &near_text);
                for (const hotc::ResolvedRef& n : near_table ? near_table->refs : std::vector<hotc::ResolvedRef>())
                    if (n.is_decl && n.id == r->ref.id && n.offset <= near_text.s.size()
                        && (n.near_decl == r->ref.near_decl || (n.near_decl.empty() && n.decl == r->ref.near_decl)))
                    {
                        const std::shared_ptr<Doc> near_doc = open_doc_at(r->ref.near_file);
                        const auto [line, col] = near_text.pos(n.offset);
                        return json::array(
                            {loc(near_doc ? near_doc->uri : path_to_uri(r->ref.near_file), near_text, Pos{line, col})});
                    }
            }
            // Overloads share one id in the table. Then clangd selects the overload at the
            // generated position, by the arguments of the call. The call goes to a forwarder of
            // the written name that has no map segment: its body calls the overload.
            size_t decl_count = 0;
            for (const hotc::ResolvedRef& n : decl_table ? decl_table->refs : std::vector<hotc::ResolvedRef>())
                if (n.is_decl && n.id == r->ref.id)
                    ++decl_count;
            if (decl_count > 1 && !r->ref.is_decl && clangd_)
                if (const auto g = to_gen(d, here))
                {
                    wait_for_native_ready(d);
                    auto locations = [&](const json& at, const char* kind) -> json
                    {
                        const auto defs = clangd_result(kind, at, kNativeBudgetMs);
                        if (defs)
                            lite_check(kind, at, d, *defs, "definition");
                        return defs ? (defs->is_array() ? *defs : json::array({*defs})) : json::array();
                    };
                    const json first = {{"textDocument", {{"uri", lite_uri(d, d.gen.s, g->line)}}}, {"position", gen_pos(d, *g)}};
                    for (const json& l : locations(first, "textDocument/definition"))
                    {
                        const std::string uri = l.contains("targetUri") ? l.value("targetUri", std::string())
                                                                        : l.value("uri", std::string());
                        const Doc* gd = nullptr;
                        if (uri == d.gen_uri)
                            gd = &d;
                        else if (const auto it = gen_docs_.find(uri); it != gen_docs_.end())
                            gd = it->second.get();
                        if (!gd)
                            continue;
                        json m = map_location(*gd, l);
                        if (m.is_null())
                        {
                            // The forwarder: "return <overload>(" is two lines after its name.
                            const json range = l.contains("targetSelectionRange") ? l["targetSelectionRange"] : l.value("range", json());
                            const Pos fp = from_lsp(gd->gen, range["start"], clangd_utf8_);
                            const std::string_view body = gd->gen.line(fp.line + 2);
                            const size_t ret = body.find("return ");
                            if (ret == std::string_view::npos)
                                continue;
                            const json second = {{"textDocument", {{"uri", uri}}},
                                {"position", to_lsp(gd->gen, {fp.line + 2, static_cast<int>(ret) + 7}, clangd_utf8_)}};
                            // The declaration is the prototype of the overload, with a map segment.
                            for (const json& l2 : locations(second, "textDocument/declaration"))
                            {
                                m = map_location(*gd, l2);
                                if (!m.is_null())
                                    break;
                            }
                        }
                        if (!m.is_null() && m.contains("range") && m.value("uri", std::string()) == decl_uri)
                            return json::array({m});
                    }
                }
            if (!decl_pos)
                return std::nullopt;
            return json::array({loc(decl_uri, decl_text, *decl_pos)});
        }
        // The words that only clangd resolves need a ready clangd, as on the clangd path below.
        wait_for_native_ready(d);
        std::map<std::string, Text> texts;
        auto found = places_of(*r, false, d.path, &texts);
        json list = json::array();
        if (auto* m = std::get_if<PlaceMap>(&found))
        {
            const bool with_decl = p.value("context", json::object()).value("includeDeclaration", true);
            if (!with_decl && decl_pos)
                if (const auto it = m->find(decl_uri); it != m->end())
                    it->second.erase(*decl_pos);
            for (const auto& [uri, at] : *m)
                for (const Pos& s : at)
                    list.push_back(loc(uri, texts[uri], s));
        }
        return list;
    };
    // Degraded mode: the translator gives no table, or a table with gaps, for a script with a
    // syntax error or with an import that has an error. Then the text scans of the lenient facts
    // give the script places, and the table is not used. The fallback stays: the table can have
    // gaps for a broken file. Hover reads the table when it exists (see on_hover).
    const bool no_table = has_no_table(d);
    if (is_ref && !no_table)
        if (auto t = by_table())
            return *t;
    if (no_table && is_ref && !here_word.empty() && is_script_class(d, here_word))
    {
        // Degraded mode: a script class. The places come from the tokens (see class_places).
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
    if (no_table && !is_ref)
        if (const auto hit = static_member_at(d, here))
        {
            // Degraded mode: a static member of a script class.
            out.push_back({{"uri", d.uri},
                {"range", script_range(d, hit->decl, {hit->decl.line, hit->decl.col + static_cast<int>(hit->member->name.size())})}});
            return out;
        }
    // A class name of a script that the table does not give: the facts give the place.
    if (!is_ref)
    {
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            if (!c.is_interface && c.name == here_word)
            {
                const Pos b = class_name_pos(d, c);
                out.push_back({{"uri", d.uri},
                    {"range", script_range(d, b, {b.line, b.col + static_cast<int>(c.name.size())})}});
                return out;
            }
    }
    // The name of an "override" method names the method of the native base. The workspace index
    // of clangd gives the place of "Base::Name".
    if (!is_ref && !here_word.empty() && clangd_)
    {
        const std::string_view l = d.text.line(here.line);
        const auto [wb, we] = word_at(d, here);
        const size_t ov = l.rfind("override", static_cast<size_t>(wb));
        const hotc::AnalyzedClass* owner = nullptr;
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            if (c.begin.line - 1 <= here.line)
                owner = &c;
        if (owner && !owner->base.empty() && we > wb && ov != std::string_view::npos && !is_id_char(ov > 0 ? l[ov - 1] : ' ')
            && l.substr(ov + 8, static_cast<size_t>(wb) - ov - 8).find_first_of("(;{=") == std::string_view::npos)
        {
            wait_for_native_ready(d);
            const auto r = clangd_result("workspace/symbol", {{"query", owner->base + "::" + here_word}}, kNativeBudgetMs);
            for (const json& sym : r && r->is_array() ? *r : json::array())
            {
                const json loc = sym.value("location", json());
                if (sym.value("name", std::string()) == here_word && sym.value("containerName", std::string()) == owner->base
                    && loc.is_object() && !gen_docs_.count(loc.value("uri", std::string())))
                    out.push_back(loc);
            }
            if (!out.empty())
                return out;
        }
    }
    if (!is_ref && !no_table)
        if (auto t = by_table())
            return *t;
    // A script interface has no place in the generated text that maps back. The facts give the
    // declaration, for example from the base list of a class.
    if (!is_ref && method == "textDocument/definition" && !here_word.empty())
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            if (c.is_interface && c.name == here_word)
            {
                const Pos b = class_name_pos(d, c);
                out.push_back({{"uri", d.uri},
                    {"range", script_range(d, b, {b.line, b.col + static_cast<int>(c.name.size())})}});
                return out;
            }
    // The places in the generated text to ask clangd. A word without a map segment (a type name,
    // a base class, a name after "new") has the places of the same word in the generated lines
    // of its script line. The first place that gives an answer wins.
    bool by_word = false;
    const std::vector<Pos> asks = gen_asks(d, here, by_word);
    if (!asks.empty())
        wait_for_native_ready(d);
    // The asks of a word share a short time budget: a place that clangd does not answer in time
    // does not hold the editor.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNativeBudgetMs);
    std::set<std::string> seen;
    std::optional<Pos> hit_ask;
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
        std::vector<int> script_lines;
        for (const json& loc : list)
        {
            json m = map_location(d, loc);
            // A map segment of an expression ("this as Boss") maps to the start of the expression:
            // the word there is not the word of the use.
            const bool wrong_word = !m.is_null() && m.value("uri", std::string()) == d.uri
                && word(d, from_lsp(d.text, m["range"]["start"], utf8_)) != here_word;
            if ((m.is_null() || wrong_word) && is_ref && loc.value("uri", std::string()) == d.gen_uri
                && !here_word.empty())
            {
                // A use with no map segment (a type name, a base class): the same word in the
                // script line of the generated line is the place.
                if (script_lines.empty())
                    script_lines = gen_script_lines(d);
                const int gl = loc["range"]["start"]["line"].get<int>();
                const int gcol = from_lsp(d.gen, loc["range"]["start"], clangd_utf8_).col;
                if (const auto at = script_use_of_gen_word(d, script_lines, gl, gcol, here_word))
                {
                    json w = {{"uri", d.uri},
                        {"range", script_range(d, *at, {at->line, at->col + static_cast<int>(here_word.size())})}};
                    if (seen.insert(w.dump()).second)
                        out.push_back(std::move(w));
                }
                continue;
            }
            if (m.is_null() || !seen.insert(m.dump()).second)
                continue;
            out.push_back(std::move(m));
        }
        if (!out.empty())
        {
            // The places in the C++ files: clangd has the declaration in a header only in its
            // index, so ask for the declaration and the definition too. Skip it when a C++
            // place is already there. The two calls share a short budget.
            const bool have_native = std::any_of(out.begin(), out.end(),
                [&](const json& l) { return l.value("uri", std::string()) != d.uri; });
            const auto extra_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNativeBudgetMs);
            if (is_ref && !have_native)
                for (const char* kind : {"textDocument/declaration", "textDocument/definition"})
                {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        extra_deadline - std::chrono::steady_clock::now()).count();
                    if (left <= 0)
                        break;
                    const auto decl = clangd_result(kind,
                        {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, g)}}, static_cast<int>(left));
                    for (const json& loc : decl ? (decl->is_array() ? *decl : json::array({*decl})) : json::array())
                    {
                        json m = map_location(d, loc);
                        if (!m.is_null() && m.value("uri", std::string()) != d.uri && seen.insert(m.dump()).second)
                            out.push_back(std::move(m));
                    }
                }
            hit_ask = g;
            break;
        }
    }
    if (is_ref && hit_ask)
        native_uses_in_scripts(d, *hit_ask, here_word, out);
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
    if (no_table && !asks.empty() && is_ref && roots_.size())
    {
        // Degraded mode: the same symbol in the other scripts of the workspace (see for_other_scripts).
        if (const auto key = here_word.empty() ? std::nullopt : key_at(d, here))
            for_other_scripts(d, *key, here_word, [&](const Doc& doc, const std::set<Pos>& places)
            {
                for (const Pos& s : places)
                    out.push_back({{"uri", doc.uri},
                        {"range", script_range(doc, s, {s.line, s.col + static_cast<int>(here_word.size())})}});
            });
    }
    return out;
}

// The references of a native symbol: clangd gives the places in the generated file of "d" and
// in the generated files of other scripts. The places in a generated file of another script have
// no map here. Remove them. Then find the uses in the other scripts of the workspace: the script
// words with the name that have the same declaration as the symbol at "ask".
void Server::native_uses_in_scripts(const Doc& d, Pos ask, const std::string& word_name, json& out)
{
    auto is_gen = [&](const json& l)
    {
        const std::string uri = l.value("uri", std::string());
        return uri != d.uri && is_generated_uri(uri);
    };
    out.erase(std::remove_if(out.begin(), out.end(), is_gen), out.end());
    if (word_name.empty() || !clangd_)
        return;
    using Target = std::tuple<std::string, int, int>;
    // True when an ask had no answer (time-out or clangd gone). A null result is an answer.
    bool ask_failed = false;
    auto targets_at = [&](const Doc& o, Pos g)
    {
        std::vector<Target> list;
        const json tdp = {{"textDocument", {{"uri", o.gen_uri}}}, {"position", gen_pos(o, g)}};
        for (const char* kind : {"textDocument/declaration", "textDocument/definition"})
        {
            const json raw = clangd_ && clangd_->running() ? clangd_request(kind, tdp, kNativeBudgetMs) : json();
            ask_failed = ask_failed || !raw.is_object();
            const std::optional<json> r = raw.is_object() && raw.contains("result") && !raw["result"].is_null()
                ? std::optional<json>(raw["result"]) : std::nullopt;
            for (const json& loc : r ? (r->is_array() ? *r : json::array({*r})) : json::array())
            {
                const std::string uri = loc.contains("targetUri") ? json_str(loc, "targetUri") : json_str(loc, "uri");
                const json range = loc.contains("targetSelectionRange") ? loc["targetSelectionRange"] : loc.value("range", json());
                if (uri.empty() || !range.is_object() || !range.contains("start") || is_generated_uri(uri))
                    continue;
                list.emplace_back(uri, range["start"].value("line", 0), range["start"].value("character", 0));
            }
        }
        return list;
    };
    const std::vector<Target> target = targets_at(d, ask);
    if (target.empty())
        return; // not a native symbol
    // The references request is single-threaded: a long budget blocks all other requests (HOT-36).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt_.reference_budget_ms);
    auto norm = [](const std::string& p) { return fs::path(p).lexically_normal().string(); };
    std::vector<std::string> todo;
    std::set<std::string> known = {norm(d.path)};
    auto add = [&](const std::string& path)
    {
        if (known.insert(norm(path)).second)
            todo.push_back(path);
    };
    for (const auto& [uri, doc] : docs_)
        if (doc->path.size() > 4 && doc->path.compare(doc->path.size() - 4, 4, ".hot") == 0)
            add(doc->path);
    for (const std::string& path : workspace_scripts(true))
        add(path);
    // The deadline is now: a wait for an index job would use the scan budget. A missing path uses the fallback.
    const IndexSnapshot snap = index_entries(todo, std::chrono::steady_clock::now());
    std::set<std::string> seen;
    for (const json& l : out)
        seen.insert(l.dump());
    size_t not_scanned = 0; // scripts that the scan could not ask, for the message
    for (size_t index = 0; index < todo.size(); ++index)
    {
        check_cancel();
        const std::string& path = todo[index];
        if (std::chrono::steady_clock::now() >= deadline)
        {
            not_scanned += todo.size() - index;
            break;
        }
        const std::shared_ptr<Doc> open = open_doc_at(path);
        std::optional<std::string> closed_text;
        if (!open)
        {
            std::shared_ptr<const Entry> e = entry_in(snap, path);
            bool read_done = false;
            if (e)
            {
                // A disk text that differs from the entry text drops the entry.
                closed_text = read_file(path);
                read_done = true;
                if (!closed_text || *closed_text != e->text)
                    e = nullptr;
            }
            if (e)
            {
                ++entry_hits_;
                if (!has_word(e->text, word_name) || !e->unclaimed_words.count(word_name))
                    continue; // no word, or every code word is a script symbol
            }
            else
            {
                ++entry_misses_;
                if (!read_done)
                    closed_text = read_file(path);
                if (!closed_text || !has_word(*closed_text, word_name))
                    continue;
                if (table_lacks_word(path, *closed_text, word_name))
                    continue; // every code word is a script symbol
            }
        }
        const Text text = open ? open->text : Text(*closed_text);
        std::vector<Pos> words;
        bool block = false;
        for (int line = 0; line < static_cast<int>(text.starts.size()); ++line)
            for (const int col : whole_word_cols(blank_non_code(text.line(line), block), word_name))
                words.push_back({line, col});
        if (words.empty())
            continue;
        auto work = [&](Doc& o)
        {
            ask_failed = false;
            if (!wait_for_native_ready_until(o, deadline))
            {
                ++not_scanned;
                return;
            }
            for (const Pos& p : words)
            {
                check_cancel();
                bool by_word = false;
                for (const Pos& g : gen_asks(o, p, by_word))
                {
                    const std::vector<Target> here = targets_at(o, g);
                    if (here.empty())
                        continue;
                    const bool same = std::any_of(here.begin(), here.end(),
                        [&](const Target& t) { return std::find(target.begin(), target.end(), t) != target.end(); });
                    if (same)
                    {
                        const json w = {{"uri", o.uri},
                            {"range", script_range(o, p, {p.line, p.col + static_cast<int>(word_name.size())})}};
                        if (seen.insert(w.dump()).second)
                            out.push_back(w);
                    }
                    break;
                }
            }
            if (ask_failed)
                ++not_scanned; // an ask had no answer: the script may have more uses
        };
        if (open)
        {
            if (open->facts.cpp.empty() || open->gen_script.s != open->text.s)
                continue; // the script does not translate
            work(*open);
        }
        else
        {
            Doc tmp = closed_doc(path, *closed_text);
            tmp.facts = timed_translate([&] { return hotc::analyze(tmp.text.s, translate_options(tmp), session_); });
            // Keep the table: the next scan skips this script when it has no native use.
            index_.store(fs::path(path).lexically_normal().string(), shard_digest(fs::path(path).lexically_normal().string(), *closed_text),
                shard_of_doc(tmp));
            with_temp_clangd(tmp, [&] { work(tmp); });
        }
    }
    if (not_scanned > 0)
    {
        const std::string note = "native references: scan stopped by the time budget or by a failed ask, "
            + std::to_string(todo.size() - not_scanned) + " of " + std::to_string(todo.size()) + " scripts";
        std::fprintf(stderr, "hot-ls: %s\n", note.c_str());
        send_client({{"jsonrpc", "2.0"}, {"method", "window/showMessage"},
            {"params", {{"type", 2}, {"message", "hot-ls: " + note + ". The list of references is partial."}}}});
    }
}

// The fallback filter for a path that the workspace index does not have (HOT-34).
bool Server::table_lacks_word(const std::string& path, const std::string& text, const std::string& word)
{
    const std::string norm = fs::path(path).lexically_normal().string();
    const std::string digest = shard_digest(norm, text);
    TableEntry& entry = table_cache_[norm];
    if (!entry.table || entry.digest != digest)
    {
        auto shard = index_.load(norm, digest);
        if (!shard)
            return false;
        entry = {digest, std::make_shared<const FileRefs>(std::move(shard->table))};
    }
    return table_lacks_word_in(*entry.table, text, word);
}

std::set<Pos> Server::class_places(const Doc& d, const std::string& name) const
{
    return class_places_in(d.text, d.facts, name);
}

bool Server::is_script_class(const Doc& d, const std::string& name) const
{
    return std::any_of(
        d.facts.classes.begin(), d.facts.classes.end(), [&](const hotc::AnalyzedClass& c) { return c.name == name; });
}

std::variant<std::set<Pos>, std::string> Server::symbol_places(
    const Doc& d, Pos cur, const std::string& old_name, Pos* decl, bool* generated)
{
    if (generated)
        *generated = false;
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
        {
            if (generated)
                *generated = true;
            return std::string("cannot rename a generated name: " + old_name);
        }
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
        // An interface method has the same name as its implementations. It does not count.
        std::optional<SymbolKey> found;
        for (const hotc::AnalyzedClass& c : d.facts.classes)
            for (const hotc::AnalyzedMember& m : c.members)
                if (m.name == name && !c.is_interface)
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

Pos Server::class_name_pos(const Doc& d, const hotc::AnalyzedClass& c) const { return class_name_pos_in(d.text, c); }

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

std::vector<std::string> Server::workspace_scripts(bool with_import_dirs) const
{
    std::vector<std::string> out;
    std::set<std::string> dirs(roots_.begin(), roots_.end());
    if (with_import_dirs)
    {
        for (const HostSet& hs : hosts_)
            for (const auto& h : hs.all())
            {
                dirs.insert(h->script_dirs.begin(), h->script_dirs.end());
                dirs.insert(h->include_dirs.begin(), h->include_dirs.end());
            }
        dirs.insert(opt_.include_dirs.begin(), opt_.include_dirs.end());
    }
    for (const std::string& root : dirs)
    {
        std::error_code ec;
        fs::recursive_directory_iterator it(utf8_path(root), fs::directory_options::skip_permission_denied, ec), end;
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
        check_cancel();
        if (path == origin.path)
            continue;
        const std::shared_ptr<Doc> open = open_doc_at(path);
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
        Doc tmp = closed_doc(path, *text);
        bool analyzed = false;
        auto analyze_tmp = [&]
        {
            tmp.facts = timed_translate([&] { return hotc::analyze(tmp.text.s, translate_options(tmp), session_); });
            analyzed = true;
            ++closed_analyses_;
        };
        // The saved shard of the file holds the symbols and the references of the last analyze.
        const std::string digest = shard_digest(path, *text);
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
                if (!at)
                    continue;
                with_temp_clangd(tmp,
                    [&]
                    {
                        const auto r = symbol_places(tmp, *at, name, nullptr);
                        if (const auto* found = std::get_if<std::set<Pos>>(&r))
                            places = *found;
                    });
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

std::vector<std::string> Server::script_imports(const Doc& d, const std::string& text)
{
    if (!open_docs_fs_)
    {
        open_docs_fs_ = std::make_shared<OpenDocsFs>();
        open_docs_fs_->server = this;
    }
    hotc::TranslateOptions o = translate_options(d);
    o.fs = open_docs_fs_;
    return script_imports_in(o, d.path, text);
}

FileRefs Server::table_of_doc(const Doc& d)
{
    return table_of_facts(d.path, d.facts, script_imports(d, d.text.s));
}

IndexShard Server::shard_of_doc(const Doc& d)
{
    return shard_of_facts(d.path, d.text, d.facts, script_imports(d, d.text.s));
}

std::shared_ptr<Doc> Server::open_doc_at(const std::string& path) const
{
    const fs::path want = fs::path(path).lexically_normal();
    for (const auto& [uri, doc] : docs_)
        if (doc->path == path || fs::path(doc->path).lexically_normal() == want)
            return doc;
    return nullptr;
}

Doc Server::closed_doc(const std::string& path, const std::string& text) const
{
    Doc tmp;
    tmp.uri = path_to_uri(path);
    tmp.path = path;
    tmp.gen_path = path + ".cpp";
    tmp.gen_uri = path_to_uri(tmp.gen_path);
    tmp.text = Text(text);
    return tmp;
}

std::shared_ptr<const FileRefs> Server::file_refs(const std::string& path, const std::string* given, Text* used)
{
    if (const std::shared_ptr<Doc> open = open_doc_at(path))
    {
        if (used)
            *used = open->text;
        if (open->table && open->table_version == open->facts_version)
        {
            ++table_hits_;
            return open->table;
        }
        open->table = std::make_shared<const FileRefs>(table_of_doc(*open));
        open->table_version = open->facts_version;
        ++table_analyses_;
        return open->table;
    }
    const std::string norm = fs::path(path).lexically_normal().string();
    std::optional<std::string> read;
    if (!given)
    {
        read = read_file(norm);
        if (!read)
            return nullptr;
    }
    const std::string* text = given ? given : &*read;
    if (used)
        *used = Text(*text);
    const std::string digest = shard_digest(norm, *text);
    TableEntry& entry = table_cache_[norm];
    if (entry.table && entry.digest == digest)
    {
        ++table_hits_;
        return entry.table;
    }
    if (auto shard = index_.load(norm, digest))
    {
        entry = {digest, std::make_shared<const FileRefs>(std::move(shard->table))};
        ++table_hits_;
        return entry.table;
    }
    Doc tmp = closed_doc(norm, *text);
    tmp.facts = timed_translate([&] { return hotc::analyze(tmp.text.s, translate_options(tmp), session_); });
    ++table_analyses_;
    ++closed_analyses_;
    IndexShard shard = shard_of_doc(tmp);
    index_.store(norm, digest, shard);
    entry = {digest, std::make_shared<const FileRefs>(std::move(shard.table))};
    return entry.table;
}

void Server::drop_import_graph(const std::string& path)
{
    graph_walk_valid_ = false;
    if (path.empty())
        graph_files_.clear();
    else
        graph_files_.erase(fs::path(path).lexically_normal().string());
}

ImportGraph& Server::import_graph(const std::string& also, bool fresh)
{
    // The scripts on disk in the roots, each open script and "also". An open script outside the
    // roots or not saved can import the declaration file too.
    // A walk of the roots that is less than 500 ms old is used again.
    const auto now = std::chrono::steady_clock::now();
    if (fresh || !graph_walk_valid_ || now - graph_walk_at_ >= std::chrono::milliseconds(500))
    {
        graph_walk_ = workspace_scripts(fresh);
        graph_walk_at_ = now;
        graph_walk_valid_ = true;
    }
    std::vector<std::string> paths = graph_walk_;
    for (const auto& [uri, doc] : docs_)
        if (!doc->path.empty())
            paths.push_back(fs::path(doc->path).lexically_normal().string());
    if (!also.empty())
        paths.push_back(fs::path(also).lexically_normal().string());
    std::set<std::string> seen;
    for (const std::string& path : paths)
    {
        if (!seen.insert(path).second)
            continue;
        check_cancel(); // each file is complete when set: a cancel leaves a valid cache
        const auto it = graph_files_.find(path);
        GraphEntry next;
        const std::shared_ptr<Doc> open = open_doc_at(path);
        std::vector<std::string> imports;
        if (open)
        {
            next.open = true;
            next.version = open->facts_version;
            if (it != graph_files_.end() && it->second.open && it->second.version == next.version)
                continue;
            imports = script_imports(*open, open->text.s);
        }
        else
        {
            std::error_code ec;
            next.size = fs::file_size(path, ec);
            next.mtime = fs::last_write_time(path, ec);
            if (it != graph_files_.end() && !it->second.open && it->second.size == next.size
                && it->second.mtime == next.mtime)
                continue;
            if (const auto text = read_file(path))
                imports = script_imports(closed_doc(path, *text), *text);
        }
        graph_files_[path] = next;
        graph_.set(hotc::ref_file_id(path), imports);
    }
    for (auto it = graph_files_.begin(); it != graph_files_.end();)
    {
        if (seen.count(it->first))
        {
            ++it;
            continue;
        }
        graph_.erase(hotc::ref_file_id(it->first));
        it = graph_files_.erase(it);
    }
    return graph_;
}

bool Server::with_temp_clangd(Doc& tmp, const std::function<void()>& fn)
{
    if (tmp.facts.cpp.empty())
        return false;
    tmp.gen = Text(tmp.facts.cpp);
    tmp.map = std::make_shared<const hotc::SourceMap>(tmp.facts.source_map);
    tmp.gen_script = tmp.text;
    tmp.gen_version = 1;
    // The guard closes the document in clangd also when "fn" throws.
    struct Close
    {
        Server& s;
        Doc& d;
        ~Close()
        {
            try
            {
                if (s.clangd_ && d.in_clangd)
                    s.clangd_->notify("textDocument/didClose", {{"textDocument", {{"uri", d.gen_uri}}}});
            }
            catch (...)
            {
            }
            d.in_clangd = false;
        }
    } close{*this, tmp};
    send_generated(tmp, tmp.facts.cpp);
    fn();
    return true;
}

std::shared_ptr<const FileRefs> Server::refs_of(const Doc& d)
{
    if (open_doc_at(d.path).get() == &d)
        return file_refs(d.path);
    return std::make_shared<const FileRefs>(table_of_doc(d));
}

Server::ClangdRef Server::clangd_ref(const Doc& d, Pos at)
{
    ClangdRef out;
    const auto g = to_gen(d, at);
    if (!g || !clangd_)
        return out;
    const auto defs = clangd_result(
        "textDocument/definition", {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, *g)}});
    const json def_list = defs ? (defs->is_array() ? *defs : json::array({*defs})) : json::array();
    if (def_list.empty())
        return out;
    const json& loc = def_list[0];
    const std::string uri = loc.contains("targetUri") ? loc.value("targetUri", std::string()) : loc.value("uri", std::string());
    json mapped;
    if (uri == d.gen_uri)
        mapped = map_location(d, loc);
    else if (const auto it = gen_docs_.find(uri); it != gen_docs_.end())
        mapped = map_location(*it->second, loc);
    else
    {
        out.kind = ClangdRef::Other; // a native declaration
        return out;
    }
    if (mapped.is_null() || !mapped.contains("range"))
        return out; // a name that the translator makes
    const std::string path = uri_to_path(mapped.value("uri", std::string()));
    std::shared_ptr<const FileRefs> table;
    Text text;
    if (path == d.path)
    {
        table = refs_of(d);
        text = d.text;
    }
    else
        table = file_refs(path, nullptr, &text); // one text for the offsets and the positions
    if (!table)
        return out;
    const Pos p = from_lsp(text, mapped["range"]["start"], utf8_);
    const hotc::ResolvedRef* r = ref_at(*table, static_cast<uint32_t>(text.offset(p.line, p.col)));
    if (!r)
        return out;
    out.kind = ClangdRef::Found;
    out.ref = *r;
    return out;
}

std::optional<Server::Resolved> Server::resolve_at(const Doc& d, Pos cur)
{
    const auto table = refs_of(d);
    if (!table)
        return std::nullopt;
    const uint32_t off = static_cast<uint32_t>(d.text.offset(cur.line, cur.col));
    if (const hotc::ResolvedRef* r = ref_at(*table, off))
        return Resolved{*r, d.text.s.substr(r->offset, r->length)};
    bool ask = false;
    for (const hotc::UnresolvedName& u : table->unresolved)
        ask = ask || (u.offset <= off && off <= u.offset + u.length);
    for (const auto& [b, e] : table->raw_ranges)
        ask = ask || (b <= off && off < e);
    if (!ask)
        return std::nullopt;
    const auto [b, e] = word_at(d, cur);
    if (b == e)
        return std::nullopt;
    const ClangdRef c = clangd_ref(d, {cur.line, b});
    if (c.kind != ClangdRef::Found)
        return std::nullopt;
    return Resolved{c.ref, std::string(d.text.line(cur.line).substr(static_cast<size_t>(b), static_cast<size_t>(e - b)))};
}

std::variant<Server::PlaceMap, std::string> Server::places_of(
    const Resolved& r, bool strict, const std::string& also, std::map<std::string, Text>* texts,
    const std::vector<std::string>* only, const IndexSnapshot* given)
{
    PlaceMap out;
    const std::string& name = r.name;
    std::vector<std::string> files = {r.ref.decl_file};
    if (only)
        files = *only;
    else if (!file_local(r.ref.kind))
        for (const std::string& f : import_graph(also).importers_of(r.ref.decl_file))
            files.push_back(f);
    // The index entries of the closed scripts. No deadline: the poll is check_cancel.
    // Strict without "given": no entry is used, every closed script uses the inline code.
    // A closed script without the name on disk gets no entry: it has no place, and a cold index
    // does not wait for it.
    IndexSnapshot own;
    if (!given && !strict)
    {
        std::vector<std::string> wanted;
        for (const std::string& path : files)
            if (!open_doc_at(path))
                if (const auto disk = read_file(path); disk && disk->find(name) != std::string::npos)
                    wanted.push_back(path);
        own = index_entries(wanted, std::chrono::steady_clock::time_point::max());
    }
    const IndexSnapshot& snap = given ? *given : own;
    // Strict with "given": the caller counted the lookups.
    const bool count = !(strict && given);
    for (const std::string& path : files)
    {
        check_cancel();
        const std::shared_ptr<Doc> open = open_doc_at(path);
        std::shared_ptr<const Entry> e;
        std::optional<std::string> closed_text;
        if (!open)
        {
            const std::optional<std::string> disk = read_file(path);
            if (!disk || disk->find(name) == std::string::npos)
                continue; // the word filter
            if (given || !strict)
                e = entry_in(snap, path);
            if (e && *disk != e->text)
            {
                // Strict with "given": the entry text must be the text on disk, else the places are wrong.
                if (strict)
                    return path + " changed on disk during the rename";
                e = nullptr; // not strict: the disk text wins
            }
            if (count)
                ++(e ? entry_hits_ : entry_misses_);
            if (!e)
                closed_text = disk;
        }
        const std::string uri = open ? open->uri : path_to_uri(path);
        // The table and "text" come from the same text: the table offsets are valid in "text".
        std::shared_ptr<const FileRefs> table;
        Text text;
        std::set<Pos> found;
        std::vector<uint32_t> asks;
        // Fills "found" and "asks" from "table" and "text".
        auto scan = [&]
        {
            found.clear();
            asks.clear();
            for (const hotc::ResolvedRef* p : places(*table, r.ref.id))
            {
                if (p->offset > text.s.size())
                    continue;
                const auto [line, col] = text.pos(p->offset);
                found.insert({line, col});
            }
            // The words that only clangd can resolve.
            for (const hotc::UnresolvedName& u : table->unresolved)
                if (u.length == name.size() && u.offset <= text.s.size() && text.s.compare(u.offset, u.length, name) == 0)
                    asks.push_back(u.offset);
            for (const auto& [b, e2] : table->raw_ranges)
                for (size_t at = text.s.find(name, b); at != std::string::npos && at + name.size() <= e2;
                    at = text.s.find(name, at + 1))
                {
                    const bool left = at == 0 || !is_id_char(text.s[at - 1]);
                    const bool right = at + name.size() >= text.s.size() || !is_id_char(text.s[at + name.size()]);
                    if (left && right)
                        asks.push_back(static_cast<uint32_t>(at));
                }
        };
        if (e)
        {
            table = e->table;
            text = Text(e->text);
        }
        else
            table = file_refs(path, open ? nullptr : &*closed_text, &text);
        if (!table)
            continue;
        scan();
        if (texts)
            (*texts)[uri] = text;
        for (const Pos& pos : found)
            out[uri].insert(pos);
        // Strict: a code word "name" that is not a ref, not an unresolved name and not in a raw body
        // has no resolution in the table. Clangd must resolve it, else the rename skips a place.
        if (strict)
        {
            std::set<uint32_t> known(asks.begin(), asks.end());
            for (const hotc::ResolvedRef& p : table->refs)
                known.insert(p.offset);
            for (const hotc::UnresolvedName& u : table->unresolved)
                known.insert(u.offset);
            bool block = false;
            for (int l = 0; l < static_cast<int>(text.starts.size()); ++l)
            {
                const std::string code = blank_non_code(text.line(l), block);
                for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1))
                {
                    const bool left = at == 0 || !is_id_char(code[at - 1]);
                    const bool right = at + name.size() >= code.size() || !is_id_char(code[at + name.size()]);
                    if (left && right && !known.count(static_cast<uint32_t>(text.starts[l] + at)))
                        asks.push_back(static_cast<uint32_t>(text.starts[l] + at));
                }
            }
        }
        // The import is not transitive: a free function or a module variable of the declaring script
        // is not visible in a script that does not import it directly. A word with its name is not
        // the symbol.
        if ((r.ref.kind == hotc::RefKind::Function || r.ref.kind == hotc::RefKind::ModuleVar)
            && table->file_id != r.ref.decl_file
            && std::find(table->imports.begin(), table->imports.end(), r.ref.decl_file) == table->imports.end())
            asks.clear();
        if (asks.empty())
            continue;
        std::string error;
        auto check = [&](const Doc& doc)
        {
            for (const uint32_t off : asks)
            {
                const auto [line, col] = doc.text.pos(off);
                const ClangdRef c = clangd_ref(doc, {line, col});
                if (c.kind == ClangdRef::Found && c.ref.id == r.ref.id)
                    out[uri].insert({line, col});
                else if (c.kind == ClangdRef::Unknown && strict && error.empty())
                    error = "cannot resolve " + name + " at " + path + ":" + std::to_string(line + 1);
            }
        };
        if (open)
            check(*open);
        else
        {
            Doc tmp = closed_doc(path, text.s);
            tmp.facts = timed_translate([&] { return hotc::analyze(tmp.text.s, translate_options(tmp), session_); });
            if (!with_temp_clangd(tmp, [&] { check(tmp); }) && strict)
                error = "cannot resolve " + name + " at " + path + ":" + std::to_string(text.pos(asks[0]).first + 1);
        }
        if (!error.empty())
            return error;
    }
    return out;
}

bool Server::importers_known() const
{
    if (!roots_.empty())
        return true;
    for (const HostSet& hs : hosts_)
        for (const auto& h : hs.all())
            if (!h->script_dirs.empty())
                return true;
    return false;
}

// True when "text" has the whole word "name".
static bool has_word(std::string_view text, const std::string& name)
{
    for (size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1))
        if ((at == 0 || !is_id_char(text[at - 1])) && (at + name.size() >= text.size() || !is_id_char(text[at + name.size()])))
            return true;
    return false;
}

std::optional<std::variant<json, std::string>> Server::table_rename(
    const Doc& d, Pos cur, const std::string& new_name, bool prepare)
{
    const auto r = resolve_at(d, cur);
    if (!r)
        return std::nullopt;
    const std::string& old_name = r->name;
    const std::string what = "cannot rename " + old_name + ": ";
    const bool local = file_local(r->ref.kind);
    if (!local && !importers_known())
        return what + "the importers of this script are unknown";
    // The candidate files: the file of the declaration and the scripts that import it, directly or
    // through other scripts. A fresh walk of the roots: a rename must not miss a new script.
    ImportGraph& graph = import_graph(d.path, true);
    std::vector<std::string> files = {r->ref.decl_file};
    if (!local)
        for (const std::string& f : graph.importers_of(r->ref.decl_file))
            files.push_back(f);
    // The index entries of the closed scripts: the candidate files and their import closure. One
    // call, no deadline: the poll is check_cancel. A path without a valid entry uses the disk.
    std::vector<std::string> closure;
    if (!prepare)
    {
        std::set<std::string> seen(files.begin(), files.end());
        std::vector<std::string> todo;
        for (const std::string& f : files)
            for (const std::string& i : graph.imports_of(f))
                todo.push_back(i);
        while (!todo.empty())
        {
            const std::string id = todo.back();
            todo.pop_back();
            if (!seen.insert(id).second)
                continue;
            closure.push_back(id);
            for (const std::string& next : graph.imports_of(id))
                todo.push_back(next);
        }
    }
    // The disk texts of the closed files, read once. Only a closed file with a word of the later
    // filters gets an entry: a cold index does not wait for the other files.
    std::map<std::string, std::optional<std::string>> disks;
    auto disk_of = [&](const std::string& path) -> const std::optional<std::string>&
    {
        auto it = disks.find(path);
        if (it == disks.end())
            it = disks.emplace(path, read_file(path)).first;
        return it->second;
    };
    std::vector<std::string> wanted;
    for (const std::string& path : files)
        if (!open_doc_at(path))
            if (const auto& disk = disk_of(path);
                disk && (has_word(*disk, old_name) || (!prepare && (has_word(*disk, new_name) || has_word(*disk, "class") || has_word(*disk, "struct")))))
                wanted.push_back(path);
    for (const std::string& id : closure)
        if (!open_doc_at(id))
            if (const auto& disk = disk_of(id);
                disk && (has_word(*disk, new_name) || has_word(*disk, "class") || has_word(*disk, "struct")))
                wanted.push_back(id);
    IndexSnapshot snap = index_entries(wanted, std::chrono::steady_clock::time_point::max());
    // The open documents with a text that is not the text on disk. The table of a closed file
    // comes from the disk texts of its imports.
    std::map<std::string, bool> dirty_cache;
    auto dirty = [&](const std::string& id)
    {
        const auto it = dirty_cache.find(id);
        if (it != dirty_cache.end())
            return it->second;
        const std::shared_ptr<Doc> open = open_doc_at(id);
        const auto disk = open ? read_file(id) : std::nullopt;
        return dirty_cache[id] = open && (!disk || *disk != open->text.s);
    };
    struct Kept
    {
        std::shared_ptr<const FileRefs> table;
        Text text;
    };
    std::vector<Kept> kept;
    kept.reserve(files.size());
    for (const std::string& path : files)
    {
        check_cancel();
        const std::shared_ptr<Doc> open = open_doc_at(path);
        std::optional<std::string> closed_text;
        std::shared_ptr<const Entry> entry;
        if (!open)
        {
            closed_text = disk_of(path);
            if (!closed_text)
                return what + "cannot read " + path;
            entry = checked_entry(snap, path, *closed_text);
        }
        const std::string_view text = open ? std::string_view(open->text.s) : std::string_view(*closed_text);
        const bool affected = has_word(text, old_name);
        // A file without the old name gets no edit. It counts for the conflict check only: for a
        // word with the new name, or for the bases of its classes (the class family).
        if (!affected && (prepare || (!has_word(text, new_name) && !has_word(text, "class") && !has_word(text, "struct"))))
            continue;
        // One lookup for each closed file of the request: a hit when the entry is used.
        if (!open)
            ++(entry ? entry_hits_ : entry_misses_);
        Kept k;
        if (entry)
        {
            k.table = entry->table;
            k.text = Text(entry->text);
        }
        else
            k.table = file_refs(path, open ? nullptr : &*closed_text, &k.text);
        if (!k.table)
            return what + path + " has no table of the resolved names";
        if (affected)
        {
            if (open.get() != &d && !in_workspace(path))
                return what + path + " is outside the workspace folders";
            // The table of an open document comes from its facts, thus it has the errors too.
            if (!k.table->errors.empty())
            {
                const auto& [line, message] = k.table->errors[0];
                if (line > 0)
                    return what + path + " has a syntax error at line " + std::to_string(line);
                return what + path + " does not translate: " + message;
            }
            if (!open)
            {
                std::set<std::string> seen = {k.table->file_id};
                std::vector<std::string> todo = graph.imports_of(k.table->file_id);
                while (!todo.empty())
                {
                    const std::string id = todo.back();
                    todo.pop_back();
                    if (!seen.insert(id).second)
                        continue;
                    if (dirty(id))
                        return what + path + " imports " + id + ", and the open document of " + id
                            + " has changes that are not saved";
                    for (const std::string& next : graph.imports_of(id))
                        todo.push_back(next);
                }
            }
        }
        kept.push_back(std::move(k));
    }
    // The conflict check also reads the imports of the files, directly or not: they have the
    // bases of the class family and the module symbols that the files see.
    if (!prepare)
    {
        for (const std::string& id : closure)
        {
            check_cancel();
            const std::shared_ptr<Doc> open = open_doc_at(id);
            std::optional<std::string> closed_text;
            std::shared_ptr<const Entry> entry;
            if (!open)
            {
                closed_text = disk_of(id);
                if (!closed_text)
                    return what + "cannot read " + id;
                entry = checked_entry(snap, id, *closed_text);
            }
            // A file without the new name and without a class cannot conflict.
            const std::string_view itext = open ? std::string_view(open->text.s) : std::string_view(*closed_text);
            if (!has_word(itext, new_name) && !has_word(itext, "class") && !has_word(itext, "struct"))
                continue;
            if (!open)
                ++(entry ? entry_hits_ : entry_misses_);
            Kept k;
            if (entry)
            {
                k.table = entry->table;
                k.text = Text(entry->text);
            }
            else
                k.table = file_refs(id, open ? nullptr : &*closed_text, &k.text);
            if (!k.table)
                return what + id + " has no table of the resolved names";
            kept.push_back(std::move(k));
        }
    }
    if (opt_.rename_hook)
        opt_.rename_hook("before_places");
    std::map<std::string, Text> texts;
    auto found = places_of(*r, true, d.path, &texts, &files, &snap);
    if (const auto* e = std::get_if<std::string>(&found))
        return what + *e;
    const PlaceMap& at = std::get<PlaceMap>(found);
    const int len = static_cast<int>(old_name.size());
    for (const auto& [uri, starts] : at)
    {
        const Text& t = texts[uri];
        for (const Pos& s : starts)
            if (s.line < 0 || s.line >= static_cast<int>(t.starts.size())
                || t.line(s.line).substr(static_cast<size_t>(s.col), old_name.size()) != old_name)
                return what + "the text at " + uri_to_path(uri) + ":" + std::to_string(s.line + 1) + " is not " + old_name;
    }
    if (prepare)
    {
        const auto [b, e] = word_at(d, cur);
        return json{{"range", script_range(d, {cur.line, b}, {cur.line, e})}, {"placeholder", old_name}};
    }
    std::vector<RenameFile> conflict_files;
    for (const Kept& k : kept)
        conflict_files.push_back({k.table.get(), k.text.s});
    if (std::string c = rename_conflict(conflict_files, r->ref, new_name); !c.empty())
        return c;
    if (!local)
        if (const std::vector<std::string> native = native_bases_of_family(conflict_files, r->ref); !native.empty())
            if (std::string c = native_member_conflict(d, native, new_name); !c.empty())
                return c;
    if (opt_.rename_hook)
        opt_.rename_hook("before_final");
    // The safety rule: the text of each closed file with an edit must be the text on disk, whether it
    // came from an entry or from a read. Else an edit could use wrong bytes.
    for (const auto& [uri, starts] : at)
    {
        const std::string path = uri_to_path(uri);
        if (starts.empty() || open_doc_at(path))
            continue;
        const std::optional<std::string> disk = read_file(path);
        if (!disk || *disk != texts[uri].s)
            return what + path + " changed on disk during the rename";
    }
    json changes = json::object();
    for (const auto& [uri, starts] : at)
    {
        const Text& t = texts[uri];
        json edits = json::array();
        for (const Pos& s : starts)
            edits.push_back({{"range", {{"start", to_lsp(t, s, utf8_)}, {"end", to_lsp(t, {s.line, s.col + len}, utf8_)}}},
                {"newText", new_name}});
        if (merge_edits(changes, uri, edits) > 0)
            return what + "two edits of " + uri_to_path(uri) + " change the same range";
    }
    warn_attribute_rename(*r);
    return json{{"changes", changes}};
}

std::string Server::native_member_conflict(
    const Doc& d, const std::vector<std::string>& bases, const std::string& new_name)
{
    for (const std::string& base : bases)
    {
        const std::string unknown = "cannot rename to " + new_name + ": the members of the native base " + base + " are unknown";
        if (!clangd_ || !clangd_->running())
            return unknown;
        wait_for_native_ready(d);
        const auto r = clangd_result("workspace/symbol", {{"query", base + "::" + new_name}}, kNativeBudgetMs);
        if (!r || !r->is_array())
            return unknown;
        for (const json& sym : *r)
        {
            const json loc = sym.value("location", json());
            std::string container = sym.value("containerName", std::string());
            if (const size_t colons = container.rfind("::"); colons != std::string::npos)
                container = container.substr(colons + 2);
            if (sym.value("name", std::string()) == new_name && container == base && loc.is_object()
                && !gen_docs_.count(loc.value("uri", std::string())) && loc.value("uri", std::string()) != d.gen_uri)
                return "cannot rename to " + new_name + ": the native class " + base + " has a member " + new_name;
        }
    }
    return std::string();
}

// A rename note (HOT-10, never a refusal): a class with an attribute, or a field or a property with a
// tag other than "alias", can be a string in a host. One window/showMessage tells the user.
void Server::warn_attribute_rename(const Resolved& r)
{
    const bool is_class = r.ref.kind == hotc::RefKind::Class;
    if (!is_class && r.ref.kind != hotc::RefKind::Field && r.ref.kind != hotc::RefKind::Property)
        return;
    Text text;
    const hotc::AnalyzeResult* facts = script_facts(path_to_uri(r.ref.decl_file), text);
    if (!facts)
        return;
    const std::vector<std::string>* attrs = nullptr;
    const size_t dot = r.ref.decl.find('.');
    for (const hotc::AnalyzedClass& c : facts->classes)
    {
        if (is_class)
        {
            if (c.name == r.ref.decl)
                attrs = &c.attributes;
        }
        else if (dot != std::string::npos && c.name == r.ref.decl.substr(0, dot))
            for (const hotc::AnalyzedMember& m : c.members)
                if (m.name == r.ref.decl.substr(dot + 1) && (m.kind == "field" || m.kind == "property"))
                    attrs = &m.attributes;
    }
    if (!attrs || attrs->empty())
        return;
    send_client({{"jsonrpc", "2.0"}, {"method", "window/showMessage"},
        {"params",
            {{"type", 2},
                {"message",
                    "hot-ls: '" + r.name + "' has the attribute [" + attrs->front()
                        + "]. A host can use this name as a string. Rename does not change strings in assets or code."}}}});
}

// The refusal of a rename of a script symbol that the table does not have.
static std::string untabled_refusal(const Doc& d, const std::string& old_name)
{
    for (const hotc::AnalyzeError& e : d.facts.errors)
        if (!e.note)
            return "cannot rename " + old_name + ": " + d.path + " has a syntax error at line " + std::to_string(e.pos.line);
    return "cannot rename " + old_name + ": the table of the resolved names does not have the symbol";
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
    wait_for_native_ready(d);
    if (auto t = table_rename(d, cur, new_name, false))
        return std::move(*t);
    // Not in the table: a native symbol (clangd renames it) or a name that the translator makes.
    Pos decl;
    bool generated = false;
    const auto r = symbol_places(d, cur, old_name, &decl, &generated);
    if (const auto* e = std::get_if<std::string>(&r))
        return generated ? *e : native_rename(d, cur, old_name, new_name, *e, false);
    return untabled_refusal(d, old_name);
}

bool Server::in_workspace(const std::string& path) const
{
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(path, ec);
    for (const std::string& r : roots_)
    {
        const fs::path rel = p.lexically_relative(fs::weakly_canonical(r, ec));
        if (!rel.empty() && *rel.begin() != "..")
            return true;
    }
    return false;
}

// The host of a script for the scope rule: the matching host, else the nearest host when the
// script is in a workspace folder. A script outside the workspace folders has no host then.
std::shared_ptr<const Host> Server::scope_host(const Doc& d) const
{
    if (auto host = host_of(d.path))
        return host;
    return in_workspace(d.path) ? nearest_host(d.path) : nullptr;
}

std::string Server::native_edit_refusal(const Doc& d, const std::string& path) const
{
    const auto host = scope_host(d);
    if (in_workspace(path))
    {
        // A generated file below the build dir of the host gets no edit, also inside the workspace.
        if (host && host_in_build_dir(*host, path))
            return "in the build dir of the host";
        return {};
    }
    if (!host)
        return "outside the workspace folders and the include dirs of the host";
    if (std::string why = host_header_refusal(*host, path); !why.empty())
        return why;
    return opt_.rename_host_headers ? std::string()
                                    : "outside the workspace folders (the setting renameHostHeaders is off)";
}

// The refusal text of a native rename starts with "what". The helpers of native_rename share this state.
std::string Server::map_script_edits(NativeRenameJob& job, const Doc& o, const json& edits)
{
    const std::string& old_name = job.old_name;
    const std::string& what = job.what;
    if (&o != job.start && !in_workspace(o.path))
        return what + ": the edit changes " + o.path + ", outside the workspace folders";
    // The script places of the edits of this script.
    std::set<Pos> places;
    // The generated line, and the script place of each edit.
    std::set<std::tuple<int, int, int>> seen;
    auto code_of = [&](const Doc& doc, int line) -> const std::string&
    {
        auto& lines = job.code_lines[&doc];
        if (lines.empty())
        {
            bool block = false;
            for (int i = 0; i < static_cast<int>(doc.text.starts.size()); ++i)
                lines.push_back(blank_non_code(doc.text.line(i), block));
        }
        return lines[static_cast<size_t>(line)];
    };
    const std::vector<int> script_lines = gen_script_lines(o);
    for (const json& e : edits)
    {
        if (!e.contains("range"))
            continue;
        const Pos gb = from_lsp(o.gen, e["range"]["start"], clangd_utf8_);
        const Pos ge = from_lsp(o.gen, e["range"]["end"], clangd_utf8_);
        if (gb.line < 0 || gb.line >= static_cast<int>(o.gen.starts.size()))
            return what + ": a changed range in the generated code is not the name";
        const std::string_view gl = o.gen.line(gb.line);
        if (gb.line != ge.line || gb.col < 0 || gb.col > ge.col || ge.col > static_cast<int>(gl.size())
            || gl.substr(static_cast<size_t>(gb.col), static_cast<size_t>(ge.col - gb.col)) != old_name)
            return what + ": a changed range in the generated code is not the name";
        const int sl = gb.line < static_cast<int>(script_lines.size()) ? script_lines[static_cast<size_t>(gb.line)] : -1;
        std::optional<Pos> at;
        if (const auto s = to_script(o, e["range"]["start"]))
        {
            // The mapped start can be the start of the expression ("this" of "this.heal").
            // Take the first whole word from there.
            const std::string_view l = o.text.line(s->line);
            for (size_t w = l.find(old_name, static_cast<size_t>(s->col)); w != std::string_view::npos && !at;
                w = l.find(old_name, w + 1))
                if ((w == 0 || !is_id_char(l[w - 1]))
                    && (w + old_name.size() >= l.size() || !is_id_char(l[w + old_name.size()])))
                    at = Pos{s->line, static_cast<int>(w)};
        }
        else if (sl < 0)
            continue; // text of the translator: no script place
        if (at && whole_word_cols(code_of(o, at->line), old_name).size() != 1)
        {
            // The line has another word of the same name (a local, a member). The first word
            // from the segment start is the use only if the k-th use rule agrees.
            const auto kth = script_use_of_gen_word(o, script_lines, gb.line, gb.col, old_name);
            if (!kth || kth->line != at->line || kth->col != at->col)
                return what + ": line " + std::to_string(at->line + 1) + " of " + o.path + " has another word "
                    + old_name + " that is not clearly this symbol";
        }
        if (!at)
            at = script_use_of_gen_word(o, script_lines, gb.line, gb.col, old_name);
        if (!at)
        {
            // Code of the translator in the region of a script line (the copy of a
            // parameter, for example) does not use the word in the script line.
            const auto t = o.gen_script_to_text({sl, 0});
            bool block = false;
            if (t && t->line < static_cast<int>(o.text.starts.size())
                && whole_word_cols(blank_non_code(o.text.line(t->line), block), old_name).empty())
                continue;
        }
        if (!at)
            return what + ": a use at line " + std::to_string(sl + 1) + " of " + o.path + " has no clear place in the script";
        places.insert(*at);
        // Two edits of one generated line for one place, in a script line with more than one
        // word of the name: the map cannot tell the uses apart. No partial rename. The
        // translator can write a use twice (a method declaration and definition, the type in
        // "Counter local = Counter()"), so these duplicates are fine in a line with one word.
        if (!seen.insert({gb.line, at->line, at->col}).second && whole_word_cols(code_of(o, at->line), old_name).size() != 1)
            return what + ": two uses at line " + std::to_string(at->line + 1) + " of " + o.path
                + " map to the same place in the script";
    }
    if (places.empty())
        return {};
    json out = json::array();
    for (const Pos& s : places)
        out.push_back({{"range", script_range(o, s, {s.line, s.col + static_cast<int>(old_name.size())})},
            {"newText", job.new_name}});
    if (merge_edits(job.changes, o.uri, out) > 0)
        return what + ": two edits of " + o.path + " change the same range";
    return {};
}

std::string Server::add_cpp_edits(NativeRenameJob& job, const Doc& d, const std::string& uri, const json& edits)
{
    // A real C++ file: the edit passes through. It must be in a workspace folder (or an include dir
    // of the host), and it must change the old name.
    const std::string path = uri_to_path(uri);
    // A file that "hotc --compile-commands" writes ("<stem>.g<N>.cpp") is generated code. The next
    // translation writes it again, so an edit of it is lost and the script does not change.
    if (is_hotc_output(path))
        return job.what + ": the edit changes the generated file " + path + ", the script of this file does not change";
    if (const std::string why = native_edit_refusal(d, path); !why.empty())
        return job.what + ": the edit changes " + path + ", " + why;
    auto& text = job.cpp_texts[uri];
    if (!text)
        if (const auto content = read_file(path))
            text = std::make_shared<Text>(*content);
    for (const json& e : edits)
        if (!e.contains("range") || !is_old_name(job, uri, e["range"]))
            return job.what + ": an edit of " + path + " does not change the name " + job.old_name;
    if (merge_edits(job.changes, uri, edits) > 0)
        return job.what + ": two edits of " + path + " change the same range";
    return {};
}

// True when the range of a real C++ file holds exactly the old name.
bool Server::is_old_name(NativeRenameJob& job, const std::string& uri, const json& range)
{
    auto& t = job.cpp_texts[uri];
    if (!t)
        if (const auto content = read_file(uri_to_path(uri)))
            t = std::make_shared<Text>(*content);
    if (!t || !range.is_object() || !range.contains("start") || !range.contains("end"))
        return false;
    const int line = range["start"].value("line", -1);
    if (line < 0 || line >= static_cast<int>(t->starts.size()) || range["end"].value("line", -2) != line)
        return false;
    const int b = from_lsp(*t, range["start"], clangd_utf8_).col, e = from_lsp(*t, range["end"], clangd_utf8_).col;
    return b >= 0 && e > b && e <= static_cast<int>(t->line(line).size())
        && t->line(line).substr(static_cast<size_t>(b), static_cast<size_t>(e - b)) == job.old_name;
}

// Skips a comment, a string or a character literal that starts at "i". Gives the new offset, or "i"
// when there is none.
static size_t skip_non_code(const std::string& s, size_t i)
{
    if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/')
    {
        const size_t e = s.find('\n', i);
        return e == std::string::npos ? s.size() : e;
    }
    if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*')
    {
        const size_t e = s.find("*/", i + 2);
        return e == std::string::npos ? s.size() : e + 2;
    }
    if (s[i] == '"' || s[i] == '\'')
    {
        size_t k = i + 1;
        while (k < s.size() && s[k] != s[i] && s[k] != '\n')
            k += s[k] == '\\' ? 2 : 1;
        return std::min(k + 1, s.size());
    }
    return i;
}

void Server::add_ctor_edits(NativeRenameJob& job)
{
    const std::string& old = job.old_name;
    auto is_id = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::vector<std::string> uris;
    for (const auto& [uri, edits] : job.changes.items())
        if (!(uri.size() > 4 && uri.compare(uri.size() - 4, 4, ".hot") == 0))
            uris.push_back(uri);
    for (const std::string& uri : uris)
    {
        const auto it = job.cpp_texts.find(uri);
        if (it == job.cpp_texts.end() || !it->second)
            continue;
        const Text& t = *it->second;
        const std::string& s = t.s;
        std::set<size_t> have; // the start offsets of the edits
        for (const json& e : job.changes[uri])
            if (e.contains("range") && is_old_name(job, uri, e["range"]))
                have.insert(t.offset(e["range"]["start"].value("line", 0),
                    from_lsp(t, e["range"]["start"], clangd_utf8_).col));
        std::set<size_t> add;
        auto skip_ws = [&](size_t i)
        {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            return i;
        };
        auto name_at = [&](size_t i) { return s.compare(i, old.size(), old) == 0 && (i + old.size() >= s.size() || !is_id(s[i + old.size()])) && (i == 0 || !is_id(s[i - 1])); };
        for (const size_t b : have)
        {
            // An out-of-class definition: Old::Old( and Old::~Old(. The name can have template
            // arguments: Old<T>::Old(.
            size_t i = skip_ws(b + old.size());
            if (i < s.size() && s[i] == '<')
            {
                int angles = 0;
                for (; i < s.size(); ++i)
                {
                    if (s[i] == '<')
                        ++angles;
                    else if (s[i] == '>' && --angles == 0)
                        break;
                    else if (s[i] == ';' || s[i] == '{' || s[i] == '}')
                        break;
                }
                i = i < s.size() && s[i] == '>' ? skip_ws(i + 1) : s.size();
            }
            if (s.compare(i, 2, "::") == 0)
            {
                i = skip_ws(i + 2);
                if (i < s.size() && s[i] == '~')
                    i = skip_ws(i + 1);
                if (name_at(i) && s[skip_ws(i + old.size())] == '(')
                    add.insert(i);
            }
            // A class definition: "struct" or "class" is before the name. Other tokens (a macro
            // like URHO3D_API, an attribute) can be between. The tokens have no ";{}(" in them.
            // "enum class Old" is not a class.
            size_t from = b;
            while (from > 0 && b - from < 300 && std::strchr(";{}()", s[from - 1]) == nullptr)
                --from;
            size_t kw_at = std::string::npos;
            for (size_t k = from; k < b;)
            {
                if (!is_id(s[k]))
                {
                    ++k;
                    continue;
                }
                size_t e = k;
                while (e < b && is_id(s[e]))
                    ++e;
                const std::string word = s.substr(k, e - k);
                if (word == "struct" || word == "class")
                    kw_at = k;
                k = e;
            }
            if (kw_at == std::string::npos)
                continue;
            // A single ":" between the keyword and the name is a base list: "class Foo : public Old".
            bool base_list = false;
            for (size_t k = kw_at; k < b; ++k)
                if (s[k] == ':' && s.compare(k, 2, "::") != 0 && (k == 0 || s[k - 1] != ':'))
                    base_list = true;
            if (base_list)
                continue;
            size_t before = kw_at;
            while (before > from && std::isspace(static_cast<unsigned char>(s[before - 1])))
                --before;
            if (before >= from + 4 && s.compare(before - 4, 4, "enum") == 0 && (before == from + 4 || !is_id(s[before - 5])))
                continue;
            i = skip_ws(b + old.size());
            if (s.compare(i, 5, "final") == 0)
                i = skip_ws(i + 5);
            if (i >= s.size() || (s[i] != '{' && !(s[i] == ':' && s.compare(i, 2, "::") != 0)))
                continue;
            while (i < s.size() && s[i] != '{' && s[i] != ';')
                ++i;
            if (i >= s.size() || s[i] != '{')
                continue;
            // clangd can miss the out-of-class definitions of this file: its index has only the
            // files of the translation units. Find "Old::Old(" and "Old::~Old(" in the whole file.
            for (size_t k = 0; k < s.size(); ++k)
            {
                if (const size_t n = skip_non_code(s, k); n != k)
                {
                    k = n - 1;
                    continue;
                }
                if (s[k] != old[0] || !name_at(k))
                    continue;
                // A qualified name (B::Old::Old) can be another class: only the places that
                // clangd renamed count.
                size_t pre = k;
                while (pre > 0 && std::isspace(static_cast<unsigned char>(s[pre - 1])))
                    --pre;
                if (pre >= 2 && s.compare(pre - 2, 2, "::") == 0 && !have.count(k))
                    continue;
                size_t q = skip_ws(k + old.size());
                if (q < s.size() && s[q] == '<')
                {
                    int angles = 0;
                    for (; q < s.size() && s[q] != ';' && s[q] != '{'; ++q)
                        if (s[q] == '<')
                            ++angles;
                        else if (s[q] == '>' && --angles == 0)
                            break;
                    q = q < s.size() && s[q] == '>' ? skip_ws(q + 1) : s.size();
                }
                if (s.compare(q, 2, "::") != 0)
                    continue;
                q = skip_ws(q + 2);
                if (q < s.size() && s[q] == '~')
                    q = skip_ws(q + 1);
                if (name_at(q) && s[skip_ws(q + old.size())] == '(')
                {
                    add.insert(k);
                    add.insert(q);
                }
            }
            int braces = 0, parens = 0;
            for (; i < s.size(); ++i)
            {
                if (const size_t n = skip_non_code(s, i); n != i)
                {
                    i = n - 1;
                    continue;
                }
                const char c = s[i];
                if (c == '{')
                    ++braces;
                else if (c == '}' && --braces == 0)
                    break;
                else if (c == '(' || c == '[')
                    ++parens;
                else if (c == ')' || c == ']')
                    --parens;
                else if (braces == 1 && parens == 0 && c == old[0] && name_at(i)
                    && s[skip_ws(i + old.size())] == '(')
                    add.insert(i);
            }
        }
        for (const size_t off : add)
        {
            if (have.count(off))
                continue;
            const auto [l, c] = t.pos(off);
            const json range = {{"start", to_lsp(t, {l, c}, clangd_utf8_)},
                {"end", to_lsp(t, {l, c + static_cast<int>(old.size())}, clangd_utf8_)}};
            job.changes[uri].push_back({{"range", range}, {"newText", job.new_name}});
        }
    }
}

std::string Server::add_answer(NativeRenameJob& job, const Doc& d, const Doc& asked, const json& result)
{
    // The edits by file. A client with "documentChanges" gets the other form; clangd gives
    // "changes" to a client without it, and this server does not ask for it.
    std::map<std::string, json> by_uri;
    if (result.contains("changes") && result["changes"].is_object())
        for (const auto& [uri, edits] : result["changes"].items())
            by_uri[uri] = edits;
    if (result.contains("documentChanges") && result["documentChanges"].is_array())
        for (const json& dc : result["documentChanges"])
            if (dc.contains("textDocument") && dc.contains("edits"))
                by_uri[json_str(dc["textDocument"], "uri")] = dc["edits"];
    for (const auto& [uri, edits] : by_uri)
    {
        if (!edits.is_array())
            continue;
        // A file of this server (a lite document, a header of a unit) is never a place of the user.
        if (uri.size() >= 9 && (uri.compare(uri.size() - 9, 9, ".lite.cpp") == 0
                || (uri.size() >= 8 && uri.compare(uri.size() - 8, 8, ".shell.h") == 0)))
            continue;
        const auto gen_it = gen_docs_.find(uri);
        std::string why;
        if (uri == asked.gen_uri)
            why = map_script_edits(job, asked, edits);
        else if (gen_it != gen_docs_.end())
            why = map_script_edits(job, *gen_it->second, edits);
        else if (is_generated_uri(uri))
        {
            // The generated file of a closed script: no edit here. Phase 2 asks the script itself.
            const std::string gen_path = uri_to_path(uri);
            job.extra_script_paths.push_back(gen_path.substr(0, gen_path.size() - 4));
        }
        else
            why = add_cpp_edits(job, d, uri, edits);
        if (!why.empty())
            return why;
    }
    return {};
}

std::variant<json, std::string> Server::native_rename(const Doc& d, Pos cur, const std::string& old_name,
    const std::string& new_name, const std::string& no_symbol, bool prepare)
{
    if (!clangd_ || !is_symbol_word(old_name))
        return no_symbol;
    bool by_word = false;
    const std::vector<Pos> asks = gen_asks(d, cur, by_word);
    if (asks.empty())
        return no_symbol;
    NativeRenameJob job;
    job.old_name = old_name;
    job.new_name = new_name;
    job.start = &d;
    job.what = "cannot rename the native symbol " + old_name;
    const std::string& what = job.what;
    // All the clangd calls of one rename share a time budget.
    job.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt_.rename_budget_ms);
    auto left_ms = [&]
    {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            job.deadline - std::chrono::steady_clock::now()).count());
    };
    auto locations = [](const std::optional<json>& r)
    { return r ? (r->is_array() ? *r : json::array({*r})) : json::array(); };
    std::string last_error = "clangd gives no answer";
    bool found = false;
    for (const Pos& g : asks)
    {
        if (left_ms() <= 0)
            break;
        const json tdp = {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, g)}};
        // A declaration or a definition in a file outside the workspace folders (a system header,
        // a library): the rename would change files of the user that are not in the project.
        // The places in the workspace are edits too: clangd can miss a header that no indexed
        // translation unit includes.
        std::vector<std::pair<std::string, json>> declared; // the uri and the range
        job.target.clear();
        for (const char* kind : {"textDocument/declaration", "textDocument/definition"})
            for (const json& loc : locations(clangd_result(kind, tdp, std::max(left_ms(), 1))))
            {
                const std::string uri = loc.contains("targetUri") ? json_str(loc, "targetUri") : json_str(loc, "uri");
                if (uri.empty() || uri == d.gen_uri || gen_docs_.count(uri))
                    continue;
                const std::string path = uri_to_path(uri);
                if (const std::string why = native_edit_refusal(d, path); !why.empty())
                    return what + ": it is declared in " + path + ", " + why;
                const json range = loc.contains("targetSelectionRange") ? loc["targetSelectionRange"] : loc.value("range", json());
                if (range.is_object() && range.contains("start") && range.contains("end"))
                {
                    job.target.emplace_back(uri, range["start"].value("line", 0), range["start"].value("character", 0));
                    declared.emplace_back(uri, range);
                }
            }
        json params = tdp;
        params["newName"] = new_name;
        const json resp = clangd_request("textDocument/rename", params, std::max(left_ms(), 1));
        if (!resp.is_object())
            continue;
        if (resp.contains("error"))
        {
            last_error = json_str(resp["error"], "message");
            continue;
        }
        if (!resp.contains("result") || !resp["result"].is_object())
            continue;
        // The state of one answer: the next answer starts with a new state.
        job.changes = json::object();
        job.cpp_texts.clear();
        job.code_lines.clear();
        if (const std::string why = add_answer(job, d, d, resp["result"]); !why.empty())
            return why;
        for (const auto& [uri, range] : declared)
        {
            // The name at the place must be the old name, and the answer of clangd must not have it.
            if (!is_old_name(job, uri, range))
                continue;
            // The range must not overlap a range of the answer of clangd.
            auto before = [](const json& a, const json& b)
            { // the position a is before the position b
                const int al = a.value("line", 0), bl = b.value("line", 0);
                return al < bl || (al == bl && a.value("character", 0) < b.value("character", 0));
            };
            bool have = false;
            if (job.changes.contains(uri))
                for (const json& e : job.changes[uri])
                    have = have
                        || (e.contains("range") && before(e["range"]["start"], range["end"])
                            && before(range["start"], e["range"]["end"]));
            if (have)
                continue;
            if (!job.changes.contains(uri))
                job.changes[uri] = json::array();
            job.changes[uri].push_back({{"range", range}, {"newText", new_name}});
        }
        if (job.changes.empty())
        {
            last_error = "the answer of clangd has no edit";
            continue;
        }
        found = true;
        break;
    }
    if (!found)
        return what + ": " + last_error;
    // prepareRename checks the start script and the declaration only.
    if (prepare)
    {
        add_ctor_edits(job);
        return json{{"changes", job.changes}};
    }
    if (const std::string why = rename_other_scripts(job, d); !why.empty())
        return why;
    add_ctor_edits(job);
    return json{{"changes", job.changes}};
}

// Phase 2 of a native rename: one clangd rename for each other script that uses the symbol. The
// answers merge into job.changes. A failure gives a refusal text and the caller drops job.changes.
std::string Server::rename_other_scripts(NativeRenameJob& job, const Doc& d)
{
    const std::string& old_name = job.old_name;
    const std::string& what = job.what;
    auto left_ms = [&]
    {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            job.deadline - std::chrono::steady_clock::now()).count());
    };
    auto norm = [](const std::string& p) { return fs::path(p).lexically_normal().string(); };
    struct Candidate
    {
        std::string path;
        bool extra;
    };
    std::vector<Candidate> todo;
    std::set<std::string> known = {norm(d.path)};
    auto add_candidate = [&](const std::string& path, bool extra)
    {
        if (known.insert(norm(path)).second)
            todo.push_back({path, extra});
    };
    for (const auto& [uri, doc] : docs_)
        if (doc->path.size() > 4 && doc->path.compare(doc->path.size() - 4, 4, ".hot") == 0)
            add_candidate(doc->path, false);
    for (const std::string& path : workspace_scripts(true))
        add_candidate(path, false);
    size_t extras_taken = 0;
    for (size_t i = 0; i < todo.size() || extras_taken < job.extra_script_paths.size(); ++i)
    {
        check_cancel();
        if (i >= todo.size())
        {
            add_candidate(job.extra_script_paths[extras_taken++], true);
            --i;
            continue;
        }
        const Candidate c = todo[i];
        const std::shared_ptr<Doc> open = open_doc_at(c.path);
        std::optional<std::string> closed_text;
        if (!open)
        {
            closed_text = read_file(c.path);
            if (!closed_text)
                return c.extra ? what + ": an edit changes the generated file " + c.path
                        + ".cpp, and its script is not found"
                               : what + ": cannot read " + c.path;
        }
        // The code words of the script: no comment, no string.
        const Text text = open ? open->text : Text(*closed_text);
        std::vector<Pos> words;
        bool block = false;
        for (int line = 0; line < static_cast<int>(text.starts.size()); ++line)
            for (const int col : whole_word_cols(blank_non_code(text.line(line), block), old_name))
                words.push_back({line, col});
        if (words.empty())
            continue;
        if (job.target.empty())
            return what + ": clangd gives no declaration";
        // The budget is in milliseconds when it is not a whole number of seconds.
        const std::string budget = opt_.rename_budget_ms % 1000 == 0
            ? std::to_string(opt_.rename_budget_ms / 1000) + " s"
            : std::to_string(opt_.rename_budget_ms) + " ms";
        const std::string over = what + ": the time budget of " + budget + " is over at " + c.path;
        auto ask = [&](const std::string& method, const json& params, json& result) -> std::string
        {
            if (left_ms() <= 0)
                return over;
            const json resp = clangd_request(method, params, left_ms());
            if (!resp.is_object() || resp.contains("error"))
                return what + ": clangd gives no answer for " + c.path;
            result = resp.contains("result") ? resp["result"] : json();
            return {};
        };
        auto work = [&](Doc& o) -> std::string
        {
            job.code_lines.erase(&o);
            if (!wait_for_native_ready_until(o, job.deadline))
                return over;
            for (const Pos& p : words)
            {
                check_cancel();
                bool by_word = false;
                for (const Pos& g : gen_asks(o, p, by_word))
                {
                    const json tdp = {{"textDocument", {{"uri", o.gen_uri}}}, {"position", gen_pos(o, g)}};
                    bool any = false, same = false;
                    std::string related;
                    for (const char* kind : {"textDocument/declaration", "textDocument/definition"})
                    {
                        json result;
                        if (const std::string why = ask(kind, tdp, result); !why.empty())
                            return why;
                        if (result.is_null())
                            continue;
                        for (const json& loc : result.is_array() ? result : json::array({result}))
                        {
                            const std::string uri = loc.contains("targetUri") ? json_str(loc, "targetUri") : json_str(loc, "uri");
                            const json range = loc.contains("targetSelectionRange") ? loc["targetSelectionRange"] : loc.value("range", json());
                            if (uri.empty() || !range.is_object() || !range.contains("start"))
                                continue;
                            any = true;
                            const int line = range["start"].value("line", 0), col = range["start"].value("character", 0);
                            const std::tuple<std::string, int, int> at = {uri, line, col};
                            // The symbol of the first answer, or a place that the first answer
                            // renames too (a constructor of a class, an override of a method).
                            const bool is_generated = uri == o.gen_uri || is_generated_uri(uri);
                            bool here = std::find(job.target.begin(), job.target.end(), at) != job.target.end();
                            if (!here && !is_generated && job.changes.contains(uri))
                                for (const json& e : job.changes[uri])
                                    here = here
                                        || (e["range"]["start"].value("line", -1) == line
                                            && e["range"]["start"].value("character", -1) == col);
                            same = same || here;
                            if (!here && !is_generated && is_old_name(job, uri, range))
                                related = related.empty() ? uri_to_path(uri) : related;
                        }
                    }
                    if (!same)
                    {
                        // A place of a real C++ file with the old name that the first answer does
                        // not change can be a related symbol (an override, a template
                        // specialization). A partial rename is wrong, so the server refuses.
                        if (!related.empty())
                            return what + ": " + c.path + " uses a symbol with the name " + old_name
                                + " in " + related + " that the rename of clangd does not change";
                        if (any)
                            break; // an other symbol: the next word
                        continue;
                    }
                    json params = tdp;
                    params["newName"] = job.new_name;
                    json result;
                    if (const std::string why = ask("textDocument/rename", params, result); !why.empty())
                        return why;
                    if (!result.is_object())
                        return what + ": clangd gives no rename answer for " + c.path;
                    if (const std::string why = add_answer(job, d, o, result); !why.empty())
                        return why;
                    if (!job.changes.contains(o.uri) || job.changes[o.uri].empty())
                        return what + ": a use in " + c.path + " has no edit";
                    return {};
                }
            }
            return {};
        };
        std::string why;
        if (open)
        {
            // The generated code of an open script must be the code of its editor text. Else the
            // script has no answer of clangd for its uses of the name.
            if (open->facts.cpp.empty() || open->gen_script.s != open->text.s)
                return what + ": " + c.path + " does not translate";
            why = work(*open);
        }
        else
        {
            Doc tmp = closed_doc(c.path, *closed_text);
            tmp.facts = timed_translate([&] { return hotc::analyze(tmp.text.s, translate_options(tmp), session_); });
            if (!with_temp_clangd(tmp, [&] { why = work(tmp); }))
                return what + ": " + c.path + " does not translate";
            job.code_lines.erase(&tmp);
        }
        if (!why.empty())
            return why;
    }
    return {};
}

std::variant<json, std::string> Server::on_prepare_rename(const Doc& d, const json& p)
{
    const Pos cur = from_lsp(d.text, p["position"], utf8_);
    const std::string old_name = word(d, cur);
    if (old_name.empty())
        return std::string("no symbol at the position");
    wait_for_native_ready(d);
    if (auto t = table_rename(d, cur, std::string(), true))
        return std::move(*t);
    Pos decl;
    bool generated = false;
    const auto r = symbol_places(d, cur, old_name, &decl, &generated);
    if (const auto* e = std::get_if<std::string>(&r))
    {
        // The probe name makes clangd do the full check (the declaration, the files, the map) of a
        // native rename, so that prepareRename and rename give the same answer.
        if (generated)
            return *e;
        const auto n = native_rename(d, cur, old_name, "hot_rename_probe", *e, true);
        if (const auto* ne = std::get_if<std::string>(&n))
            return *ne;
        const auto [b, e2] = word_at(d, cur);
        return json{{"range", script_range(d, {cur.line, b}, {cur.line, e2})}, {"placeholder", old_name}};
    }
    return untabled_refusal(d, old_name);
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
        const std::string detail = c.is_interface ? "interface" : c.base.empty() ? "class" : "class : " + c.base;
        out.push_back({{"name", c.name}, {"kind", c.is_interface ? 11 : 5}, {"detail", detail},
            {"range", script_range(d, b, e)},
            {"selectionRange", script_range(d, b, {b.line, b.col + static_cast<int>(c.name.size())})},
            {"children", children}});
    }
    return out;
}

void Server::start_token_worker(const std::string& gen_uri, int version, uint64_t nonce)
{
    // The worker has its own late answer, so it does not wait for the preamble.
    constexpr int kBudgetMs = kNativeBudgetMs;
    token_workers_.erase(std::remove_if(token_workers_.begin(), token_workers_.end(),
                             [](TokenWorker& w)
                             {
                                 if (!w.done->load())
                                     return false;
                                 w.thread.join();
                                 return true;
                             }),
        token_workers_.end());
    auto done = std::make_shared<std::atomic<bool>>(false);
    ClangdProcess* clangd = clangd_.get();
    ++native_token_requests_;
    token_workers_.push_back({std::thread(
                                  [this, clangd, gen_uri, version, nonce, done]
                                  {
                                      json r = clangd->request("textDocument/semanticTokens/full",
                                          {{"textDocument", {{"uri", gen_uri}}}}, kBudgetMs);
                                      {
                                          std::lock_guard<std::mutex> lock(token_mutex_);
                                          const auto it = token_cache_.find(gen_uri);
                                          // A new entry (the document was closed and opened again) is not the entry of this request.
                                          if (it != token_cache_.end() && it->second.nonce == nonce)
                                          {
                                              NativeTokenCache& c = it->second;
                                              c.pending = false;
                                              // An answer with no token is not kept: clangd can answer before it
                                              // has the syntax tree. The next request asks again.
                                              if (r.is_object() && r.contains("result") && r["result"].is_object()
                                                  && r["result"].contains("data") && r["result"]["data"].is_array()
                                                  && !r["result"]["data"].empty() && version >= c.version)
                                              {
                                                  c.version = version;
                                                  c.data = std::move(r["result"]["data"]);
                                              }
                                          }
                                      }
                                      token_cv_.notify_all();
                                      done->store(true);
                                  }),
        done});
}

// The semantic tokens of clangd for the generated text, as tokens of the script. The answer of
// clangd is in a cache for each version of the generated text. A request waits a short time for
// a missing answer. Then it uses the tokens of the last answer that still fit, or no token. A late
// answer goes to the cache for the next request. The server thread never waits for the full time.
std::vector<SemanticToken> Server::native_tokens(const Doc& d, const std::vector<SemanticToken>& known)
{
    if (!opt_.native_semantic_tokens || !clangd_ || !clangd_->running() || clangd_token_types_.empty() || !d.map
        || d.gen.s.empty())
        return {};
    std::unique_lock<std::mutex> lock(token_mutex_);
    NativeTokenCache& c = token_cache_[d.gen_uri];
    if (c.nonce == 0)
        c.nonce = ++token_nonce_;
    if (c.version != d.gen_version && !c.pending)
    {
        c.pending = true;
        start_token_worker(d.gen_uri, d.gen_version, c.nonce);
    }
    if (opt_.native_token_wait_ms > 0)
        token_cv_.wait_for(lock, std::chrono::milliseconds(opt_.native_token_wait_ms),
            [&] { return c.version == d.gen_version || !c.pending; });
    if (c.version == d.gen_version && c.last_version != d.gen_version)
    {
        c.last = derive_native_tokens(d, c.data);
        c.last_version = d.gen_version;
        c.data = json();
    }
    std::set<std::pair<int, int>> taken;
    for (const SemanticToken& t : known)
        taken.insert({t.line, t.col});
    std::vector<SemanticToken> out;
    for (const auto& [t, name] : c.last)
    {
        if (taken.count({t.line, t.col}))
            continue;
        if (c.last_version != d.gen_version)
        {
            // The tokens of an older text: keep a token only if the same name is still at the place.
            if (t.line >= static_cast<int>(d.text.starts.size()) || t.col < 0)
                continue;
            const std::string_view l = d.text.line(t.line);
            if (static_cast<size_t>(t.col) + static_cast<size_t>(t.len) > l.size()
                || l.substr(static_cast<size_t>(t.col), static_cast<size_t>(t.len)) != name)
                continue;
        }
        out.push_back(t);
    }
    return out;
}

// A native name has no segment in the source map, so the match is by the name and the script
// line: a token of the generated lines of a script line is a token for the same name in that
// script line. A name that clangd classifies in two ways in those lines (a parameter with the
// name of a type, for example) gets no token. There is a cap on the number of tokens.
std::vector<std::pair<SemanticToken, std::string>> Server::derive_native_tokens(const Doc& d, const json& answer)
{
    constexpr size_t kMaxClangdTokens = 50000;
    constexpr int kSkip = -1, kOther = -2; // no token for the type, and a name that is not a native symbol
    if (!answer.is_array())
        return {};
    // The type of the legend of hot-ls for each type of the legend of clangd.
    std::vector<int> ours;
    for (const std::string& t : clangd_token_types_)
    {
        int v = kSkip;
        if (t == "class" || t == "struct" || t == "interface" || t == "enum")
            v = 2;
        else if (t == "type")
            v = 1;
        else if (t == "function" || t == "method")
            v = 3;
        else if (t == "property" || t == "enumMember")
            v = 4;
        else if (t == "variable" || t == "parameter" || t == "macro" || t == "typeParameter")
            v = kOther;
        ours.push_back(v);
    }
    const std::vector<int> script_lines = gen_script_lines(d);
    std::map<int, std::map<std::string, int>> by_line; // the text line, the name, the type
    const json& data = answer;
    int gl = 0, gc = 0;
    for (size_t i = 0; i + 4 < data.size() && i / 5 < kMaxClangdTokens; i += 5)
    {
        if (!data[i].is_number_integer() || !data[i + 1].is_number_integer() || !data[i + 2].is_number_integer()
            || !data[i + 3].is_number_integer())
            return {};
        gl += data[i].get<int>();
        gc = data[i].get<int>() == 0 ? gc + data[i + 1].get<int>() : data[i + 1].get<int>();
        const int type = data[i + 3].get<int>();
        if (type < 0 || type >= static_cast<int>(ours.size()) || ours[static_cast<size_t>(type)] == kSkip || gl < 0
            || gl >= static_cast<int>(script_lines.size()) || script_lines[static_cast<size_t>(gl)] < 0)
            continue;
        const auto t = d.gen_script_to_text({script_lines[static_cast<size_t>(gl)], 0});
        if (!t)
            continue;
        const std::string_view l = d.gen.line(gl);
        const int b = from_lsp(d.gen, {{"line", gl}, {"character", gc}}, clangd_utf8_).col;
        const int e = from_lsp(d.gen, {{"line", gl}, {"character", gc + data[i + 2].get<int>()}}, clangd_utf8_).col;
        if (b < 0 || e <= b || e > static_cast<int>(l.size()))
            continue;
        const std::string name(l.substr(static_cast<size_t>(b), static_cast<size_t>(e - b)));
        const auto [it, fresh] = by_line[t->line].emplace(name, ours[static_cast<size_t>(type)]);
        if (!fresh && it->second != ours[static_cast<size_t>(type)])
            it->second = kOther;
    }
    std::vector<std::pair<SemanticToken, std::string>> out;
    bool in_block = false;
    for (int line = 0; line < static_cast<int>(d.text.starts.size()) && !by_line.empty(); ++line)
    {
        const std::string code = blank_non_code(d.text.line(line), in_block);
        const auto found = by_line.find(line);
        if (found == by_line.end())
            continue;
        for (size_t i = 0; i < code.size();)
        {
            if (!is_id_char(code[i]))
            {
                ++i;
                continue;
            }
            size_t e = i;
            while (e < code.size() && is_id_char(code[e]))
                ++e;
            const std::string name = code.substr(i, e - i);
            const auto t = found->second.find(name);
            if (!(code[i] >= '0' && code[i] <= '9') && t != found->second.end() && t->second >= 0 && is_symbol_word(name))
                out.push_back({{line, static_cast<int>(i), static_cast<int>(e - i), t->second}, name});
            i = e;
        }
    }
    return out;
}

json Server::on_semantic_tokens(const Doc& d)
{
    json data = json::array();
    int pl = 0, pc = 0;
    std::vector<SemanticToken> tokens = script_tokens(d.text.s, d.facts);
    std::vector<SemanticToken> native = native_tokens(d, tokens);
    tokens.insert(tokens.end(), native.begin(), native.end());
    std::stable_sort(tokens.begin(), tokens.end(),
        [](const SemanticToken& a, const SemanticToken& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
    for (const SemanticToken& t : tokens)
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

// The parameter names at the call sites of the script methods and constructors. The hints inside
// the "range" of the request are the answer. The facts of the last analysis give the callees.
// Hints for the type of a "var" are not available: the server has no type of a local.
json Server::on_inlay_hint(const Doc& d, const json& p)
{
    Pos lo = {0, 0}, hi = {std::numeric_limits<int>::max(), 0};
    if (p.contains("range"))
    {
        lo = from_lsp(d.text, p["range"]["start"], utf8_);
        hi = from_lsp(d.text, p["range"]["end"], utf8_);
    }
    json out = json::array();
    for (const ParamHint& h : script_param_hints(d.text.s, d.facts))
    {
        const auto [line, col] = d.text.pos(h.off);
        const Pos at = {line, col};
        if (at < lo || hi < at)
            continue;
        out.push_back({{"position", to_lsp(d.text, at, utf8_)}, {"label", h.label}, {"kind", 2}, {"paddingRight", true}});
    }
    // The calls of native functions, methods and constructors: clangd gives the parameter names.
    // The calls share a short time budget, so a slow clangd does not hold the editor.
    // The budget starts at the first native call, after the wait for the preamble.
    constexpr int kMaxNativeCalls = 64;
    auto deadline = std::chrono::steady_clock::time_point::max();
    int asked = 0;
    for (const CallSite& site : script_unknown_calls(d.text.s, d.facts))
    {
        const auto [nl, nc] = d.text.pos(site.name_off);
        if (Pos{nl, nc} < lo || hi < Pos{nl, nc})
            continue;
        if (site.args.empty() || !is_symbol_word(site.name) || asked >= kMaxNativeCalls)
            continue;
        if (asked == 0)
        {
            wait_for_native_ready(d);
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNativeBudgetMs);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            break;
        ++asked;
        const std::vector<std::string> names = native_param_names(d, site, site.args.size(), static_cast<int>(left));
        if (names.size() < site.args.size())
            continue;
        for (size_t k = 0; k < site.args.size(); ++k)
        {
            const CallArg& a = site.args[k];
            if (a.empty || names[k].empty() || a.plain == names[k])
                continue;
            const auto [al, ac] = d.text.pos(a.begin);
            out.push_back({{"position", to_lsp(d.text, {al, ac}, utf8_)}, {"label", names[k] + ":"}, {"kind", 2},
                {"paddingRight", true}});
        }
    }
    return out;
}

// The name of a parameter in the label of a signature ("int by", "const char* text = nullptr").
// A label with a type only gives no name.
static std::string param_name_of_label(std::string label)
{
    const size_t eq = label.find('=');
    if (eq != std::string::npos)
        label.resize(eq);
    auto trim = [](std::string& t)
    {
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
    };
    trim(label);
    if (!label.empty() && label.back() == ']')
    {
        const size_t open = label.find('[');
        if (open != std::string::npos)
            label.resize(open);
        trim(label);
    }
    size_t b = label.size();
    while (b > 0 && is_id_char(label[b - 1]))
        --b;
    const std::string name = label.substr(b);
    std::string prefix = label.substr(0, b);
    trim(prefix);
    if (name.empty() || prefix.empty() || prefix.back() == ':' || name[0] == '_' || std::isdigit(static_cast<unsigned char>(name[0])))
        return "";
    static const std::set<std::string> type_words = {"int", "char", "short", "long", "unsigned", "signed", "float",
        "double", "bool", "void", "auto", "wchar_t", "char8_t", "char16_t", "char32_t"};
    static const std::set<std::string> qualifiers = {"const", "volatile", "struct", "class", "enum", "typename"};
    if (type_words.count(name))
        return "";
    // "const Counter" is a type only: every word before the last is a qualifier.
    std::istringstream words(prefix);
    std::string w;
    bool only_qualifiers = true;
    while (words >> w)
        only_qualifiers = only_qualifiers && qualifiers.count(w);
    return only_qualifiers ? std::string() : name;
}

// The parameter names of the signature that clangd picks for the call. The place for clangd is
// the end of the generated arguments: then the arguments choose the overload. An empty answer
// means that the callee is not known, or has fewer parameters than the call has arguments.
std::vector<std::string> Server::native_param_names(const Doc& d, const CallSite& site, size_t arg_count, int budget_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    const auto [nl, nc] = d.text.pos(site.name_off);
    bool by_word = false;
    for (const Pos& g : gen_asks(d, {nl, nc}, by_word))
    {
        const std::string_view gl = d.gen.line(g.line);
        size_t i = static_cast<size_t>(g.col) + site.name.size();
        while (i < gl.size() && (gl[i] == ' ' || gl[i] == '>'))
            ++i;
        if (i >= gl.size() || gl[i] != '(' || gl.compare(static_cast<size_t>(g.col), site.name.size(), site.name) != 0)
            continue;
        // The matching ")" on the same line, else the place after the "(". With the place after
        // the "(" the arguments do not choose the overload: then the signatures must agree.
        bool block = false;
        const std::string code = blank_non_code(gl, block);
        size_t cursor = i + 1;
        bool closed = false;
        int depth = 0;
        for (size_t k = i; k < code.size(); ++k)
        {
            if (code[k] == '(')
                ++depth;
            else if (code[k] == ')' && --depth == 0)
            {
                cursor = k;
                closed = true;
                break;
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            break;
        const auto r = clangd_result("textDocument/signatureHelp",
            {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, {g.line, static_cast<int>(cursor)})}},
            static_cast<int>(left));
        if (!r || !r->is_object() || !r->contains("signatures") || !(*r)["signatures"].is_array()
            || (*r)["signatures"].empty())
            continue;
        const json& sigs = (*r)["signatures"];
        std::vector<std::vector<std::string>> all_names;
        for (const json& sig : sigs)
        {
            const std::string text = json_str(sig, "label");
            std::vector<std::string> names;
            const auto plist = sig.is_object() ? sig.find("parameters") : sig.end();
            if (sig.is_object() && plist != sig.end() && plist->is_array())
                for (const json& prm : *plist)
                {
                    std::string label;
                    const auto lf = prm.is_object() ? prm.find("label") : prm.end();
                    if (prm.is_object() && lf != prm.end())
                    {
                        if (lf->is_string())
                            label = lf->get<std::string>();
                        else if (lf->is_array() && lf->size() == 2 && (*lf)[0].is_number_integer()
                            && (*lf)[1].is_number_integer())
                        {
                            const size_t b = static_cast<size_t>(std::max((*lf)[0].get<int>(), 0));
                            const size_t e = static_cast<size_t>(std::max((*lf)[1].get<int>(), 0));
                            if (b <= e && e <= text.size())
                                label = text.substr(b, e - b);
                        }
                    }
                    names.push_back(param_name_of_label(label));
                }
            all_names.push_back(std::move(names));
        }
        if (closed)
        {
            const auto af = r->find("activeSignature");
            const int active = af != r->end() && af->is_number_integer() ? af->get<int>() : 0;
            const auto& names = all_names[static_cast<size_t>(active >= 0 && active < static_cast<int>(all_names.size()) ? active : 0)];
            if (names.size() >= arg_count)
                return names;
            continue;
        }
        // Every signature with enough parameters must have the same names for the arguments.
        const std::vector<std::string>* first = nullptr;
        bool agree = true;
        for (const auto& names : all_names)
        {
            if (names.size() < arg_count)
                continue;
            if (!first)
                first = &names;
            else if (!std::equal(first->begin(), first->begin() + static_cast<long>(arg_count), names.begin()))
                agree = false;
        }
        if (first && agree)
            return *first;
    }
    return {};
}

// One lens for each script class and each method or constructor. The title has the number of
// references. The count is a name scan of the document (see script_name_uses): it uses neither
// clangd nor the other scripts, so the request is quick. The lens has no command: the title is all.
// A link for the path of each `using "..."` line. A script path uses the import search of the
// translator. A header path uses the directory of the script and the include directories. A path
// that no file has gets no link.
json Server::on_document_link(const Doc& d)
{
    json out = json::array();
    if (!open_docs_fs_)
    {
        open_docs_fs_ = std::make_shared<OpenDocsFs>();
        open_docs_fs_->server = this;
    }
    hotc::TranslateOptions o = translate_options(d);
    o.fs = open_docs_fs_;
    const std::string dir = fs::path(d.path).parent_path().string();
    bool block = false;
    for (int l = 0; l < static_cast<int>(d.text.starts.size()); ++l)
    {
        const std::string line(d.text.line(l));
        const std::string code = blank_non_code(line, block);
        const size_t first = code.find_first_not_of(" \t");
        if (first == std::string::npos || code.compare(first, 5, "using") != 0)
            continue;
        size_t q1 = line.find_first_not_of(" \t", first + 5);
        if (q1 == std::string::npos || line[q1] != '"')
            continue;
        const size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos)
            continue;
        const std::string path = line.substr(q1 + 1, q2 - q1 - 1);
        std::string target;
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".hot") == 0)
        {
            std::string text, file, unit;
            if (hotc::find_import(o, path, dir, std::string(), text, file, unit))
                target = file;
        }
        else
        {
            std::vector<std::string> dirs = {dir};
            dirs.insert(dirs.end(), o.include_dirs.begin(), o.include_dirs.end());
            for (const std::string& base : dirs)
            {
                std::error_code ec;
                const fs::path cand = fs::path(base) / path;
                if (fs::is_regular_file(cand, ec))
                {
                    target = cand.string();
                    break;
                }
            }
        }
        if (target.empty())
            continue;
        std::error_code ec;
        const fs::path abs = fs::absolute(fs::path(target), ec).lexically_normal();
        out.push_back({{"range", script_range(d, {l, static_cast<int>(q1) + 1}, {l, static_cast<int>(q2)})},
            {"target", path_to_uri(ec ? target : abs.string())}});
    }
    return out;
}

json Server::on_code_lens(const Doc& d)
{
    json out = json::array();
    const std::vector<NameUse> uses = script_name_uses(d.text.s, d.facts);
    auto count = [&](const std::string& name)
    {
        return static_cast<int>(
            std::count_if(uses.begin(), uses.end(), [&](const NameUse& u) { return u.name == name; }));
    };
    auto lens = [&](Pos b, size_t len, int n)
    {
        const std::string title = std::to_string(n) + (n == 1 ? " reference" : " references");
        out.push_back({{"range", script_range(d, b, {b.line, b.col + static_cast<int>(len)})},
            {"command", {{"title", title}, {"command", ""}}}});
    };
    for (const hotc::AnalyzedClass& c : d.facts.classes)
    {
        lens(class_name_pos(d, c), c.name.size(), count(c.name));
        for (const hotc::AnalyzedMember& m : c.members)
            if (m.kind == "method" || m.kind == "constructor")
                lens({m.begin.line - 1, m.begin.col - 1}, m.name.size(), count(m.name));
    }
    return out;
}

// The item of a call hierarchy for a method or a constructor. "data" lets the next requests find
// the member again.
json Server::call_item(const Doc& d, const Callable& c) const
{
    const Pos b = {c.m->begin.line - 1, c.m->begin.col - 1}, e = {c.m->end.line - 1, c.m->end.col - 1};
    return {{"name", c.m->name}, {"kind", c.m->kind == "constructor" ? 9 : 6}, {"detail", c.cls->name},
        {"uri", d.uri}, {"range", script_range(d, b, e)},
        {"selectionRange", script_range(d, b, {b.line, b.col + static_cast<int>(c.m->name.size())})},
        {"data", {{"cls", c.cls->name}, {"name", c.m->name}, {"line", b.line}, {"col", b.col}}}};
}

// The methods and the constructors at the position: the one that is declared there, or else all
// the ones that have the name of the word. The match is by name.
json Server::on_prepare_call_hierarchy(const Doc& d, const json& p)
{
    const json position = p.is_object() ? p.value("position", json::object()) : json::object();
    if (!position.is_object())
        return nullptr;
    const Pos here = from_lsp(d.text, position, utf8_);
    const size_t at = d.text.offset(here.line, here.col);
    const std::vector<Callable> all = callables_of(d);
    json out = json::array();
    for (const Callable& c : all)
        if (c.begin <= at && at <= c.begin + c.m->name.size())
            return json::array({call_item(d, c)});
    // The name of a destructor is not a call.
    for (const hotc::AnalyzedClass& c : d.facts.classes)
        for (const hotc::AnalyzedMember& m : c.members)
        {
            const size_t b = d.text.offset(m.begin.line - 1, m.begin.col - 1);
            if (m.kind == "destructor" && b <= at && at <= b + m.name.size())
                return nullptr;
        }
    const std::string w = word(d, here);
    if (w.empty())
        return nullptr;
    for (const Callable& c : all)
        if (c.m->name == w)
            out.push_back(call_item(d, c));
    if (!out.empty())
        return out;
    // A native function, method or constructor: clangd gives the item.
    wait_for_native_ready(d);
    out = native_call_items(d, here, kNativeBudgetMs);
    return out.empty() ? json(nullptr) : out;
}

bool Server::is_generated_uri(const std::string& uri) const
{
    return gen_docs_.count(uri) || (uri.size() > 8 && uri.compare(uri.size() - 8, 8, ".hot.cpp") == 0);
}

// An item of clangd as an item of the server. "data" holds the item of clangd, so that the next
// request can give it back, and the script that asked.
json Server::native_item(const Doc& d, json item) const
{
    item["data"] = {{"native", item}, {"script", d.uri}};
    return item;
}

// The items of clangd for the word at a script position. The place is the word in the generated
// text. An item in a generated file is not native, and an item with an other name is not the word.
json Server::native_call_items(const Doc& d, Pos here, int budget_ms)
{
    json out = json::array();
    const std::string w = word(d, here);
    if (!is_symbol_word(w))
        return out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    bool by_word = false;
    for (const Pos& g : gen_asks(d, here, by_word))
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            break;
        const auto r = clangd_result("textDocument/prepareCallHierarchy",
            {{"textDocument", {{"uri", d.gen_uri}}}, {"position", gen_pos(d, g)}}, static_cast<int>(left));
        if (!r || !r->is_array())
            continue;
        for (const json& item : *r)
            if (item.is_object() && json_str(item, "name") == w && !is_generated_uri(json_str(item, "uri")))
                out.push_back(native_item(d, item));
        if (!out.empty())
            break;
    }
    return out;
}

// The incoming calls of an item of clangd are the script calls with its name (the open
// documents) and the C++ calls that clangd knows. The outgoing calls are the calls that clangd knows.
json Server::on_native_call_hierarchy(bool incoming, const json& data)
{
    json out = json::array();
    const json& native = data["native"];
    if (incoming)
    {
        // A script that has a method or a constructor with the name calls that one, not the
        // native symbol. The match is by name only (see the documentation).
        const std::string name = json_str(native, "name");
        for (const auto& [uri, doc] : docs_)
        {
            if (doc->cpp)
                continue;
            const std::vector<Callable> own = callables_of(*doc);
            if (std::any_of(own.begin(), own.end(), [&](const Callable& c) { return c.m->name == name; }))
                continue;
            for (json& e : incoming_by_name(*doc, name))
                out.push_back(std::move(e));
        }
    }
    const auto r = clangd_result(incoming ? "callHierarchy/incomingCalls" : "callHierarchy/outgoingCalls",
        {{"item", native}}, kNativeBudgetMs);
    if (!r || !r->is_array())
        return out;
    const char* key = incoming ? "from" : "to";
    const std::string script = json_str(data, "script");
    for (json e : *r)
    {
        if (!e.is_object() || !e.contains(key) || !e[key].is_object()
            || is_generated_uri(json_str(e[key], "uri")))
            continue;
        e[key]["data"] = {{"native", e[key]}, {"script", script}};
        out.push_back(std::move(e));
    }
    return out;
}

// The incoming calls of a method are the calls with its name in the bodies of the methods and
// constructors of the document, grouped by the one that holds them. The outgoing calls are the calls in the body of
// the method whose name is the name of a method or a constructor of the document. A call
// inside a field initializer or in another document is not found.
json Server::on_call_hierarchy(const std::string& method, const json& p)
{
    json out = json::array();
    // A request with a malformed item gets an empty answer. A member of a wrong type is absent.
    auto field = [](const json& j, const char* key, json::value_t type) -> const json*
    {
        if (!j.is_object())
            return nullptr;
        const auto f = j.find(key);
        return f != j.end() && f->type() == type ? &*f : nullptr;
    };
    const json* item = field(p, "item", json::value_t::object);
    const json* uri = item ? field(*item, "uri", json::value_t::string) : nullptr;
    const json* data = item ? field(*item, "data", json::value_t::object) : nullptr;
    if (!uri || !data)
        return out;
    const bool incoming = method == "callHierarchy/incomingCalls";
    if (const json* native = field(*data, "native", json::value_t::object); native && !native->empty())
        return on_native_call_hierarchy(incoming, *data);
    const auto it = docs_.find(uri->get<std::string>());
    if (it == docs_.end() || it->second->cpp)
        return out;
    const Doc& d = *it->second;
    const std::vector<Callable> all = callables_of(d);
    const json* cls_j = field(*data, "cls", json::value_t::string);
    const json* name_j = field(*data, "name", json::value_t::string);
    if (!cls_j || !name_j)
        return out;
    const std::string cls = cls_j->get<std::string>(), name = name_j->get<std::string>();
    auto number = [&](const char* key)
    {
        const auto f = data->find(key);
        return f != data->end() && f->is_number_integer() ? f->get<int>() : -1;
    };
    const int want_line = number("line"), want_col = number("col");
    const Callable* self = nullptr;
    for (const Callable& c : all)
        if (c.cls->name == cls && c.m->name == name
            && (!self || (c.m->begin.line - 1 == want_line && c.m->begin.col - 1 == want_col)))
            self = &c;
    if (!self)
        return out;
    if (method == "callHierarchy/incomingCalls")
        return incoming_by_name(d, name);
    const std::vector<NameUse> uses = script_name_uses(d.text.s, d.facts);
    std::map<const Callable*, json> groups; // the items that the calls reach
    std::vector<const Callable*> order;
    // The native callees: the uses of each name that no script method has, in source order.
    std::vector<std::string> native_order;
    std::map<std::string, std::vector<const NameUse*>> native_uses;
    for (const NameUse& u : uses)
    {
        if (!u.call || !(self->begin <= u.off && u.off < self->end))
            continue;
        bool script_callee = false;
        for (const Callable& c : all)
            if (c.m->name == u.name)
            {
                script_callee = true;
                if (!groups.count(&c))
                {
                    groups[&c] = json::array();
                    order.push_back(&c);
                }
                groups[&c].push_back(use_range(d, u));
            }
        if (!script_callee && is_symbol_word(u.name))
        {
            if (!native_uses.count(u.name))
                native_order.push_back(u.name);
            native_uses[u.name].push_back(&u);
        }
    }
    std::sort(order.begin(), order.end(), [](const Callable* a, const Callable* b) { return a->begin < b->begin; });
    for (const Callable* c : order)
        out.push_back({{"to", call_item(d, *c)}, {"fromRanges", groups[c]}});
    // clangd gives the item of each native name. The names share a short time budget.
    constexpr size_t kMaxNativeNames = 16;
    if (!native_order.empty())
        wait_for_native_ready(d);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNativeBudgetMs);
    for (size_t i = 0; i < native_order.size() && i < kMaxNativeNames; ++i)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            break;
        const auto& list = native_uses[native_order[i]];
        const auto [l, c] = d.text.pos(list[0]->off);
        const json items = native_call_items(d, {l, c}, static_cast<int>(left));
        if (items.empty())
            continue;
        json ranges = json::array();
        for (const NameUse* u : list)
            ranges.push_back(use_range(d, *u));
        out.push_back({{"to", items[0]}, {"fromRanges", ranges}});
    }
    return out;
}

// The script range of the name of a use.
json Server::use_range(const Doc& d, const NameUse& u) const
{
    const auto [l, c] = d.text.pos(u.off);
    return script_range(d, {l, c}, {l, c + static_cast<int>(u.name.size())});
}

json Server::incoming_by_name(const Doc& d, const std::string& name) const
{
    json out = json::array();
    const std::vector<Callable> all = callables_of(d);
    std::map<const Callable*, json> groups;
    std::vector<const Callable*> order;
    for (const NameUse& u : script_name_uses(d.text.s, d.facts))
    {
        if (!u.call || u.name != name)
            continue;
        for (const Callable& c : all)
            if (c.begin <= u.off && u.off < c.end)
            {
                if (!groups.count(&c))
                {
                    groups[&c] = json::array();
                    order.push_back(&c);
                }
                groups[&c].push_back(use_range(d, u));
                break;
            }
    }
    std::sort(order.begin(), order.end(), [](const Callable* a, const Callable* b) { return a->begin < b->begin; });
    for (const Callable* c : order)
        out.push_back({{"from", call_item(d, *c)}, {"fromRanges", groups[c]}});
    return out;
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

const hotc::AnalyzeResult* Server::script_facts(const std::string& uri, Text& text)
{
    if (const auto it = docs_.find(uri); it != docs_.end())
    {
        text = it->second->text;
        return &it->second->facts;
    }
    const std::string path = uri_to_path(uri);
    const auto t = read_file(path);
    if (!t)
        return nullptr;
    auto& cached = disk_facts_[path];
    if (cached.first != *t)
    {
        Doc tmp;
        tmp.path = path;
        cached = {*t, timed_translate([&] { return hotc::analyze(*t, translate_options(tmp), session_); })};
    }
    text = Text(*t);
    return &cached.second;
}

json Server::script_member_location(const std::string& uri, const std::string& script_name)
{
    // The script name is "Class.member". A helper of the translator (a thunk, a slot function,
    // a field init) has a name that is not a member of the class: it gets no location.
    const size_t dot = script_name.find('.');
    if (uri.empty() || dot == std::string::npos)
        return nullptr;
    const std::string cls = script_name.substr(0, dot), member = script_name.substr(dot + 1);
    Text text;
    const hotc::AnalyzeResult* facts = script_facts(uri, text);
    if (!facts)
        return nullptr;
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
    const std::string query = p.value("query", std::string());
    auto r = clangd_result("workspace/symbol", {{"query", query}});
    if (r && r->is_array())
        map_clangd_symbols(*r, out);
    add_script_symbols(query, out);
    // The lite document gives a symbol with another range for the same script place: after the map two entries are
    // equal. The first entry stays.
    json unique = json::array();
    std::set<std::string> seen;
    for (json& e : out)
        if (seen.insert(e.dump()).second)
            unique.push_back(std::move(e));
    return unique;
}

void Server::map_clangd_symbols(const json& symbols, json& out)
{
    for (const json& s : symbols)
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
}

IndexSnapshot Server::index_entries(const std::vector<std::string>& paths, std::chrono::steady_clock::time_point deadline)
{
    // The server never gives an open path to wait_for. The index always describes the disk.
    // An inverted map is exact only for a path set that wait_for validated with an empty "missing" list.
    // With no running index, or for a path outside the workspace, the path is missing. No job runs.
    const bool active = windex_.started() && index_active();
    IndexSnapshot out;
    std::vector<std::string> wanted;
    for (const std::string& path : paths)
    {
        if (open_doc_at(path))
            continue;
        const std::string key = index_key(path);
        if (!active || !in_workspace(key))
        {
            out.missing.push_back(key);
            continue;
        }
        windex_.ensure(key, index_job_config(key), IndexLevel::Request); // keeps a running job
        wanted.push_back(key);
    }
    if (wanted.empty())
        return out;
    IndexSnapshot snap;
    {
        Yield yield(*this);
        snap = windex_.wait_for(wanted, [this] { check_cancel(); }, deadline);
    }
    out.entries = std::move(snap.entries);
    out.missing.insert(out.missing.end(), snap.missing.begin(), snap.missing.end());
    return out;
}

JobConfig Server::index_job_config(const std::string& path) const
{
    JobConfig c;
    c.options = translate_options(closed_doc(path, std::string()), false);
    const Effective eff = effective_options(path);
    for (const std::string& p : prelude_files(eff, path))
        c.prelude_paths.push_back(index_key(p));
    for (const std::string& f : eff.extra_flags)
        c.flags_text += f + '\n';
    c.build_id = opt_.build_id;
    return c;
}

std::string Server::host_signature() const
{
    // A host refresh can report a change for the same data. The index needs a new job only
    // when the data of a job changes: the flags, the include dirs, the standard, the preludes.
    std::string sig;
    for (const HostSet& hs : hosts_)
        for (const auto& h : hs.all())
        {
            sig += h->name + '\n' + h->std_version + '\n';
            for (const std::string& f : h->cxx_flags())
                sig += f + '\n';
            for (const std::string& d : h->include_dirs)
                sig += d + '\n';
            for (const std::string& d : h->script_dirs)
                sig += d + '\n';
            for (const std::string& d : h->preludes)
                sig += d + '\n';
            sig += '\x01';
        }
    return sig;
}

void Server::reschedule_index()
{
    if (!index_active())
        return;
    std::string sig = host_signature();
    if (sig == index_signature_)
        return;
    index_signature_ = std::move(sig);
    // Every path that has a slot gets the new config. This includes the paths that ensure()
    // made under hidden directories. A path that left the workspace is forgotten.
    std::set<std::string> paths;
    for (const std::string& path : workspace_scripts())
        paths.insert(path);
    for (const std::string& path : windex_.known_paths())
        paths.insert(path);
    for (const std::string& path : paths)
    {
        if (in_workspace(path))
            windex_.schedule(path, index_job_config(path), IndexLevel::Rest);
        else
            windex_.forget(path);
    }
}

void Server::start_background_index()
{
    if (!opt_.background_index || !index_.enabled() || roots_.empty() || windex_.started() || shutdown_)
        return;
    index_.prune();
    const std::vector<std::string> scripts = workspace_scripts();
    if (progress_wanted_)
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_create_id_ = "hot-index-progress";
        send_client({{"jsonrpc", "2.0"}, {"id", progress_create_id_}, {"method", "window/workDoneProgress/create"},
            {"params", {{"token", "hot-index"}}}});
    }
    // Queue all scripts first. The workers start after that, so no worker ends the queue early.
    for (const std::string& path : scripts)
        windex_.schedule(path, index_job_config(path), IndexLevel::Rest);
    index_signature_ = host_signature();
    windex_.finish_scheduling();
    windex_.start(index_, session_, static_cast<unsigned>(std::max(0, opt_.index_threads)), [this] { index_progress(); });
    index_progress();
}

// Sends the begin, the report and the end of the progress. At most one report in 200 ms.
void Server::index_progress()
{
    if (!progress_wanted_)
        return;
    std::lock_guard<std::mutex> lock(progress_mutex_);
    if (!progress_created_ || progress_ended_)
        return;
    const IndexStats st = windex_.stats();
    const auto now = std::chrono::steady_clock::now();
    const std::string text = std::to_string(st.done) + " of " + std::to_string(st.total) + " scripts";
    const int percent = st.total > 0 ? st.done * 100 / st.total : 100;
    auto progress = [&](json value)
    {
        send_client({{"jsonrpc", "2.0"}, {"method", "$/progress"}, {"params", {{"token", "hot-index"}, {"value", std::move(value)}}}});
    };
    if (!progress_begun_)
    {
        progress_begun_ = true;
        progress_last_ = now;
        progress({{"kind", "begin"}, {"title", "Indexing scripts"}, {"message", text}, {"percentage", percent}});
    }
    if (st.ready)
    {
        progress_ended_ = true;
        progress({{"kind", "end"}, {"message", text}});
    }
    else if (now - progress_last_ >= std::chrono::milliseconds(200))
    {
        progress_last_ = now;
        progress({{"kind", "report"}, {"message", text}, {"percentage", percent}});
    }
}

std::string Server::shard_digest(const std::string& path, const std::string& text)
{
    // The digest holds the import files. The flags of the host also go in.
    const Effective eff = effective_options(path);
    std::string flags_text;
    for (const std::string& f : eff.extra_flags)
        flags_text += f + '\n';
    // The preludes go in with the key form of index_job_config, so the worker gives the same digest.
    std::vector<std::pair<std::string, std::string>> preludes;
    for (const std::string& p : prelude_files(eff, path))
        if (const auto prelude_text = read_file(p))
            preludes.emplace_back(index_key(p), *prelude_text);
    return SymbolIndex::shard_digest(
        index_.cached_digest(path, text, eff.include_dirs), preludes, flags_text, opt_.build_id);
}

void Server::add_script_symbols(const std::string& query, json& out)
{
    if (query.empty())
        return; // every symbol of every file: too much
    auto lower = [](std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = lower(query);
    std::set<std::pair<std::string, std::string>> seen; // (uri, name) of the symbols that are in "out"
    for (const json& s : out)
        seen.insert({s.value("/location/uri"_json_pointer, std::string()), s.value("name", std::string())});
    struct Hit
    {
        std::string name;
        int kind;
        Pos at;
        size_t len;
    };
    // The deadline is now: the index returns at once and queues the stale paths.
    const auto scripts = workspace_scripts();
    const IndexSnapshot snap = index_entries(scripts, std::chrono::steady_clock::now());
    for (const std::string& path : scripts)
    {
        check_cancel();
        const std::string uri = path_to_uri(path);
        std::vector<Hit> hits;
        auto add = [&](const std::string& name, int kind, Pos at, size_t len)
        {
            if (lower(name).find(q) != std::string::npos && seen.insert({uri, name}).second)
                hits.push_back({name, kind, at, len});
        };
        std::shared_ptr<Doc> open;
        for (const auto& [u, doc] : docs_)
            if (doc->path == path)
                open = doc;
        const Text* text = nullptr;
        Text closed_text;
        if (open)
        {
            // An open document has its own facts.
            text = &open->text;
            for (const hotc::AnalyzedClass& c : open->facts.classes)
            {
                add(c.name, 5, class_name_pos(*open, c), c.name.size());
                for (const hotc::AnalyzedMember& m : c.members)
                    add(c.name + "." + m.name, m.kind == "field" ? 8 : 6, {m.begin.line - 1, m.begin.col - 1},
                        m.name.size());
            }
        }
        else if (auto e = entry_in(snap, path); e || (index_active() && (e = windex_.entry(index_key(path)))))
        {
            // An entry can be stale here. That is allowed for a workspace symbol.
            ++entry_hits_;
            for (const IndexClass& c : e->classes)
            {
                add(c.name, 5, {c.begin.line, c.begin.col}, c.name.size());
                for (const IndexMember& m : c.members)
                    add(c.name + "." + m.name, m.kind == "field" ? 8 : 6, {m.begin.line, m.begin.col}, m.name.size());
            }
            if (!hits.empty())
            {
                closed_text = Text(e->text);
                text = &closed_text;
            }
        }
        else
        {
            // A closed file without an entry is never analyzed here: only a saved shard counts.
            ++entry_misses_;
            auto content = read_file(path);
            if (!content)
                continue;
            auto shard = index_.load(path, shard_digest(path, *content));
            if (!shard)
                continue;
            for (const IndexClass& c : shard->classes)
            {
                add(c.name, 5, {c.begin.line, c.begin.col}, c.name.size());
                for (const IndexMember& m : c.members)
                    add(c.name + "." + m.name, m.kind == "field" ? 8 : 6, {m.begin.line, m.begin.col}, m.name.size());
            }
            if (!hits.empty())
            {
                closed_text = Text(std::move(*content));
                text = &closed_text;
            }
        }
        for (const Hit& h : hits)
        {
            const Pos e = {h.at.line, h.at.col + static_cast<int>(h.len)};
            out.push_back({{"name", h.name}, {"kind", h.kind},
                {"location",
                    {{"uri", uri},
                        {"range", {{"start", to_lsp(*text, h.at, utf8_)}, {"end", to_lsp(*text, e, utf8_)}}}}}});
        }
    }
}

void Server::store_closed_shard(const Doc& d)
{
    // A saved document that closes gives its shard to the index. A document with changes that are
    // not saved, or with no class (a parse error), gives none.
    if (!index_.enabled() || d.cpp || d.facts.classes.empty())
        return;
    const auto disk = read_file(d.path);
    if (!disk || *disk != d.text.s)
        return;
    index_.store(d.path, shard_digest(d.path, *disk), shard_of_doc(d));
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
    {
        // A response to a request of the server. Only the progress create request has one.
        // The server ignores the other responses and sends no error reply.
        bool created = false;
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            if (msg.contains("id") && msg["id"] == progress_create_id_ && !progress_create_id_.is_null()
                && !msg.contains("error"))
            {
                progress_created_ = true;
                created = true;
            }
        }
        if (created)
            index_progress();
        return;
    }

    if (method == "initialize")
        return reply(id, on_initialize(p));
    if (method == "shutdown")
    {
        shutdown_ = true;
        windex_.stop();
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
        update(*d);
        if (!d->cpp)
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            for (const hotc::AnalyzedClass& c : d->facts.classes)
                for (const hotc::AnalyzedMember& m : c.members)
                    if (m.kind == "field")
                        d->open_fields[c.name].push_back(m.name);
        }
        return;
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
        close_lite(*d);
        remove_shell(*d);
        preamble_ready_.erase(d->gen_uri);
        completion_synced_.erase(d->gen_uri);
        store_closed_shard(*d);
        if (!d->cpp && index_active())
        {
            // A change that was not saved to the disk shows in the stamp: the index analyzes again.
            const std::string key = index_key(d->path);
            FileStamp disk;
            const auto e = windex_.entry(key);
            if (!e || !stat_file(key, disk) || e->stamp != disk)
                windex_.invalidate(key);
        }
        {
            std::lock_guard<std::mutex> lock(docs_mutex_);
            docs_.erase(d->uri);
            if (!d->cpp)
                gen_docs_.erase(d->gen_uri);
        }
        {
            std::lock_guard<std::mutex> lock(token_mutex_);
            token_cache_.erase(d->gen_uri);
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
    if (method == "workspace/didChangeWorkspaceFolders")
        return on_workspace_folders(p);
    if (method == "initialized")
        return start_background_index();
    if (!is_request)
        return; // $/cancelRequest and other notifications
    if (method == "workspace/symbol")
        return reply(id, on_workspace_symbol(p));
    if (method == "hot/indexStats")
    {
        // The test hook: "tables" (an array of URIs) gets the number of refs of each table;
        // "importersOf" (a URI) gets the importers of that script.
        json out;
        if (p.is_object() && p.contains("tables") && p["tables"].is_array())
        {
            out["tableRefs"] = json::array();
            for (const json& u : p["tables"])
            {
                const auto t = u.is_string() ? file_refs(uri_to_path(u.get<std::string>())) : nullptr;
                out["tableRefs"].push_back(t ? static_cast<int>(t->refs.size()) : -1);
            }
        }
        if (const std::string of = json_str(p, "entryOf"); !of.empty())
        {
            // The test probe: "open", "valid" or "missing".
            const std::string path = uri_to_path(of);
            if (open_doc_at(path))
                out["entryOf"] = "open";
            else
            {
                const std::string key = index_key(path);
                const IndexSnapshot snap = index_entries({path}, std::chrono::steady_clock::now() + std::chrono::seconds(2));
                out["entryOf"] = snap.entries.count(key) ? "valid" : "missing";
            }
        }
        if (const std::string of = json_str(p, "importersOf"); !of.empty())
            out["importers"] = import_graph().importers_of(hotc::ref_file_id(uri_to_path(of)));
        {
            const IndexStats st = windex_.stats();
            out["index"] = {{"ready", st.ready}, {"total", st.total}, {"done", st.done}, {"pending", st.pending},
                {"analyzed", st.analyzed}, {"loaded", st.loaded}, {"readyMs", st.ready_ms}, {"threads", st.threads}};
        }
        out.update({{"analyzed", closed_analyses_}, {"loaded", index_.loads()}, {"loadMs", index_.load_ms()}, {"stored", index_.stores()},
            {"dir", index_.dir()}, {"completions", completion_translations_},
            {"tokenRequests", native_token_requests_.load()}, {"translations", translations_.load()},
            {"liteHits", lite_hits_.load()}, {"liteFallback", lite_fallbacks_.load()}, {"liteMismatch", lite_mismatches_.load()},
            {"translateMs", static_cast<double>(translate_us_.load()) / 1000.0}, {"tableHits", table_hits_},
            {"tableAnalyses", table_analyses_}, {"entryHits", entry_hits_}, {"entryMisses", entry_misses_}});
        {
            // The last update() of an open script, in ms. Additive fields for the measurement.
            std::lock_guard<std::mutex> lock(last_update_mutex_);
            const auto ms = [](long long ns) { return static_cast<double>(ns) / 1e6; };
            const hotc::TranslateTiming& t = last_update_.timing;
            out["lastUpdate"] = {{"frontMs", ms(t.front_ns)}, {"lowerMs", ms(t.lower_ns)}, {"checkMs", ms(t.check_ns)},
                {"emitMs", ms(t.emit_ns)}, {"parseMs", ms(t.parse_ns)}, {"parseWaitMs", ms(t.parse_wait_ns)},
                {"parses", t.parses}, {"passes", last_update_.passes}, {"postWorkMs", ms(last_update_.post_ns)},
                {"analyzeMs", ms(last_update_.analyze_ns)}, {"seq", last_update_.seq},
                {"importReadMs", ms(t.import_read_ns)}, {"importParseMs", ms(t.import_parse_ns)},
                {"importViewMs", ms(t.import_view_ns)}, {"foreignClassMs", ms(t.foreign_class_ns)},
                {"foreignClasses", t.foreign_classes}, {"ownClassMs", ms(t.own_class_ns)},
                {"ownClasses", t.own_classes}, {"checkClassMs", ms(t.check_class_ns)}};
            for (int k = 0; k < 16; ++k)
                out["lastUpdate"]["frontStep" + std::to_string(k) + "Ms"] = ms(t.front_step_ns[k]);
            for (int f = 0; f < 2; ++f)
                for (int k = 0; k < 40; ++k)
                    out["lastUpdate"][std::string(f ? "ownStep" : "foreignStep") + std::to_string(k) + "Ms"] =
                        ms(t.step_ns[f][k]);
        }
        return reply(id, out);
    }
    if (method == "hot/toScript")
        return reply(id, on_to_script(p));

    // These requests carry an item of a call hierarchy, not a text document.
    if (method == "callHierarchy/incomingCalls" || method == "callHierarchy/outgoingCalls")
        return reply(id, on_call_hierarchy(method, p));

    static const std::set<std::string> doc_methods = {"textDocument/completion", "textDocument/hover",
        "textDocument/definition", "textDocument/references", "textDocument/documentSymbol",
        "textDocument/semanticTokens/full", "textDocument/rename", "textDocument/prepareRename", "textDocument/signatureHelp",
        "textDocument/formatting", "textDocument/rangeFormatting", "textDocument/codeAction", "textDocument/inlayHint", "textDocument/codeLens", "textDocument/documentLink",
        "textDocument/prepareCallHierarchy"};
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
    if (method == "textDocument/inlayHint")
        return reply(id, on_inlay_hint(*d, p));
    if (method == "textDocument/codeLens")
        return reply(id, on_code_lens(*d));
    if (method == "textDocument/documentLink")
        return reply(id, on_document_link(*d));
    if (method == "textDocument/prepareCallHierarchy")
        return reply(id, on_prepare_call_hierarchy(*d, p));
    if (method == "textDocument/formatting")
        return reply(id, on_formatting(*d, p));
    if (method == "textDocument/rangeFormatting")
        return reply(id, on_range_formatting(*d, p));
    if (method == "textDocument/codeAction")
        return reply(id, on_code_action(*d, p));
    if (method == "textDocument/definition" || method == "textDocument/references")
        return reply(id, on_locations(*d, p, method));
    if (method == "textDocument/documentSymbol")
        return reply(id, on_document_symbol(*d));
    if (method == "textDocument/semanticTokens/full")
        return reply(id, on_semantic_tokens(*d));
    auto r = method == "textDocument/prepareRename" ? on_prepare_rename(*d, p) : on_rename(*d, p);
    if (auto* e = std::get_if<std::string>(&r))
        return reply_error(id, kRequestFailed, *e);
    return reply(id, std::get<json>(r));
}

void Server::reader_loop()
{
    MessageReader reader(in_);
    std::string body;
    while (reader.read(body))
    {
        json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded() || !msg.is_object())
            continue;
        if (msg.contains("method") && !msg["method"].is_string())
            continue; // not a message of the protocol
        const std::string method = msg.value("method", std::string());
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (method == "$/cancelRequest")
            {
                // Only the id of a request that is queued or runs counts. The set stays small.
                const json& params = msg.contains("params") ? msg["params"] : json();
                if (params.is_object() && params.contains("id"))
                {
                    const std::string key = params["id"].dump();
                    if (pending_ids_.count(key) && cancelled_ids_.size() < kMaxCancelled)
                    {
                        cancelled_ids_.insert(key);
                        any_cancel_ = true;
                    }
                }
                continue;
            }
            if (method != "exit" && msg.contains("id") && !method.empty())
                pending_ids_.insert(id_key(msg));
            queue_.push_back(std::move(msg));
        }
        queue_cv_.notify_one();
        if (method == "exit")
            break; // the state thread stops at "exit"
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_closed_ = true;
    }
    queue_cv_.notify_one();
}

bool Server::next_message(json& out)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [&] { return !queue_.empty() || queue_closed_; });
    if (queue_.empty())
        return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    // The uri of a didChange with well-formed params. Else null: such a message does not join.
    auto change_uri = [](const json& m) -> const json*
    {
        if (!m.is_object() || !m.contains("method") || m["method"] != "textDocument/didChange")
            return nullptr;
        const auto params = m.find("params");
        if (params == m.end() || !params->is_object())
            return nullptr;
        const auto td = params->find("textDocument");
        const auto changes = params->find("contentChanges");
        if (td == params->end() || !td->is_object() || changes == params->end() || !changes->is_array() || changes->empty())
            return nullptr;
        const auto uri = td->find("uri");
        return uri != td->end() && uri->is_string() ? &*uri : nullptr;
    };
    if (const json* first = change_uri(out))
    {
        // Debounce: a later didChange of the same uri replaces this one. The messages
        // between them stay in order, so only the directly next ones join.
        const std::string uri = first->get<std::string>();
        while (!queue_.empty() && change_uri(queue_.front()) && change_uri(queue_.front())->get<std::string>() == uri)
        {
            out = std::move(queue_.front());
            queue_.pop_front();
        }
    }
    return true;
}

void Server::finish_request(const std::string& key)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_ids_.erase(key);
    cancelled_ids_.erase(key);
    any_cancel_ = !cancelled_ids_.empty();
}

void Server::run_request(const json& msg)
{
    request_key_ = id_key(msg);
    if (!msg.contains("id") && !msg.value("method", std::string()).empty())
        ++epoch_; // a client notification
    state_lock_.lock_fast();
    try
    {
        if (!request_key_.empty() && cancelled())
            throw RequestCancelled{};
        handle(msg);
    }
    catch (const RequestCancelled&)
    {
        reply_error(msg["id"], kRequestCancelled, "request cancelled");
    }
    catch (const StaleRequest&)
    {
        reply_error(msg["id"], kContentModified, "content modified");
    }
    catch (const std::exception& e)
    {
        if (msg.contains("id"))
            reply_error(msg["id"], kRequestFailed, std::string("internal error: ") + e.what());
    }
    state_lock_.unlock();
    request_key_.clear();
}

int Server::run()
{
    state_thread_ = std::this_thread::get_id();
    std::thread reader([this] { reader_loop(); });
    json msg;
    while (next_message(msg))
    {
        if (msg.value("method", std::string()) == "exit")
            break;
        const std::string key = id_key(msg);
        run_request(msg);
        if (!key.empty())
            finish_request(key);
    }
    // The reader ends at the end of the input or after "exit".
    reader.join();
    windex_.stop();
    if (clangd_)
        clangd_->stop();
    for (auto& [uri, doc] : docs_) // the headers of the units (best effort)
        remove_shell(*doc);
    if (!shell_dir_.empty())
    {
        std::error_code ec;
        fs::remove_all(shell_dir_, ec);
    }
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
           "              [--compile-commands-dir dir] [--private-clangd-dir] [--index-dir dir]\n"
           "              [--hosts-dir dir] [--clangd-log file] [--no-native-tokens]\n"
           "              [--no-rename-host-headers]\n"
           "  --clangd path      the clangd binary (default: clangd from PATH). \"\" turns clangd off.\n"
           "  -I dir             an include dir for the script imports and for clangd.\n"
           "  --compile-commands-dir dir\n"
           "                     the directory of compile_commands.json from hotc --compile-commands.\n"
           "                     clangd runs its background index on it. The index is in\n"
           "                     <dir>/.cache/clangd/index, shared with a C++ clangd.\n"
           "  --private-clangd-dir\n"
           "                     the clangd child keeps its own index in .cache/hot/clangd\n"
           "                     (first workspace folder) and does not share the C++ index.\n"
           "  --index-dir dir    the directory of the saved symbol index (default:\n"
           "                     .cache/hot/index in the first workspace folder).\n"
           "  --hosts-dir dir    the directory of the host files from hot_script_host (default:\n"
           "                     .cache/hot/hosts in each workspace folder).\n"
           "  --no-native-tokens\n"
           "                     the semantic tokens have no tokens for native (C++) names.\n"
           "  --no-rename-host-headers\n"
           "                     a native rename does not edit a header in an include dir of the host.\n";
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
        else if (a == "--private-clangd-dir")
        {
            out.private_clangd_dir = true;
        }
        else if (a == "--no-native-tokens")
        {
            out.native_semantic_tokens = false;
        }
        else if (a == "--no-rename-host-headers")
        {
            out.rename_host_headers = false;
        }
        else if (a == "--no-background-index")
        {
            out.background_index = false;
        }
        else if (a == "--index-threads")
        {
            std::string n;
            if (!next(n))
                return false;
            out.index_threads = std::atoi(n.c_str());
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

bool is_shell_include_diagnostic(bool shell_active, int unit_line)
{
    return shell_active && unit_line == 0;
}

// The unit text: the include of the header, then the lines of the shell as empty lines, then the own part.
std::optional<std::string> shell_unit_with_include(const std::string& header, const std::string& cpp, size_t at)
{
    const size_t lines = static_cast<size_t>(std::count(cpp.begin(), cpp.begin() + at + 1, '\n'));
    const std::string path = fs::path(header).generic_string();
    // The include has no escapes: a path with a quote or a line break uses the one-unit path.
    if (lines < 2 || path.find_first_of("\"\n\r") != std::string::npos)
        return std::nullopt;
    std::string out = "#include \"" + path + "\"\n";
    out.append(lines - 1, '\n');
    out.append(cpp, at + 1, std::string::npos);
    return out;
}

} // namespace hot::ls
