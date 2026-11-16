// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "symbol_index.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <stdexcept>
#include <sstream>

#include "hot/runtime/process.h"
#include "long_path.h"

namespace fs = std::filesystem;

namespace hot::ls
{

using json = nlohmann::json;

namespace
{

constexpr const char* kMagic = "hot-index";

uint64_t fnv1a(const std::string& s, uint64_t h)
{
    for (const unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex16(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

struct Header
{
    int version = 0;
    std::string digest, path;
};

// Parses the first line of a shard file.
std::optional<Header> parse_header(const std::string& line)
{
    std::istringstream ss(line);
    std::string magic;
    Header h;
    if (!(ss >> magic >> h.version >> h.digest) || magic != kMagic)
        return std::nullopt;
    ss.get(); // the space before the path
    std::getline(ss, h.path);
    return h;
}

// Reads the first line of a shard file.
std::optional<Header> read_header(const std::string& file)
{
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line))
        return std::nullopt;
    return parse_header(line);
}

json pos_json(const IndexPos& p)
{
    return json::array({p.line, p.col});
}
IndexPos pos_of(const json& j)
{
    return {j.at(0).get<int>(), j.at(1).get<int>()};
}

json places_json(const std::vector<IndexPos>& v)
{
    json a = json::array();
    for (const IndexPos& p : v)
        a.push_back(pos_json(p));
    return a;
}
std::vector<IndexPos> places_of(const json& j)
{
    std::vector<IndexPos> v;
    for (const json& p : j)
        v.push_back(pos_of(p));
    return v;
}

// The lines of the table. Each name gives the start offset of its line: offset - (col - 1). The
// result has a pair (line, start) for each line with a name, sorted by the start.
template <typename T>
void add_lines(const std::vector<T>& items, std::map<int, uint32_t>& lines)
{
    for (const T& r : items)
        if (r.pos.line > 0 && r.pos.col > 0 && r.offset >= static_cast<uint32_t>(r.pos.col - 1))
            lines.emplace(r.pos.line, r.offset - static_cast<uint32_t>(r.pos.col - 1));
}

json table_json(const FileRefs& t)
{
    std::vector<std::string> strings;
    std::map<std::string, size_t> index;
    const auto str = [&](const std::string& v)
    {
        auto [it, added] = index.emplace(v, strings.size());
        if (added)
            strings.push_back(v);
        return it->second;
    };
    json refs = json::array();
    for (const hotc::ResolvedRef& r : t.refs)
        refs.push_back(json::array({r.offset, r.length, static_cast<int>(r.kind), r.is_decl ? 1 : 0, str(r.decl),
            str(r.decl_file), str(r.near_decl), str(r.near_file)}));
    json unresolved = json::array();
    for (const hotc::UnresolvedName& u : t.unresolved)
        unresolved.push_back(json::array({u.offset, u.length}));
    json raw = json::array();
    for (const auto& [b, e] : t.raw_ranges)
        raw.push_back(json::array({b, e}));
    std::map<int, uint32_t> lines;
    add_lines(t.refs, lines);
    add_lines(t.unresolved, lines);
    json lj = json::array();
    for (const auto& [line, start] : lines)
        lj.push_back(json::array({line, start}));
    return {{"f", t.file_id}, {"s", strings}, {"r", std::move(refs)}, {"u", std::move(unresolved)},
        {"w", std::move(raw)}, {"l", std::move(lj)}, {"i", t.imports}, {"e", t.errors}, {"b", t.bases}};
}

FileRefs table_of(const json& j)
{
    FileRefs t;
    t.file_id = j.at("f").get<std::string>();
    const std::vector<std::string> strings = j.at("s").get<std::vector<std::string>>();
    std::vector<std::pair<uint32_t, int>> lines; // (start, line), sorted by the start
    for (const json& l : j.at("l"))
        lines.emplace_back(l.at(1).get<uint32_t>(), l.at(0).get<int>());
    std::sort(lines.begin(), lines.end());
    const auto pos = [&](uint32_t offset)
    {
        auto it = std::upper_bound(lines.begin(), lines.end(), std::make_pair(offset, INT32_MAX));
        if (it == lines.begin())
            return hotc::SourcePos{};
        --it;
        return hotc::SourcePos{it->second, static_cast<int>(offset - it->first) + 1};
    };
    for (const json& rj : j.at("r"))
    {
        hotc::ResolvedRef r;
        r.offset = rj.at(0).get<uint32_t>();
        r.length = rj.at(1).get<uint32_t>();
        const int kind = rj.at(2).get<int>();
        if (kind < 0 || kind > static_cast<int>(hotc::RefKind::EnumValue))
            throw std::runtime_error("bad ref kind");
        r.kind = static_cast<hotc::RefKind>(kind);
        r.is_decl = rj.at(3).get<int>() != 0;
        r.decl = strings.at(rj.at(4).get<size_t>());
        r.decl_file = strings.at(rj.at(5).get<size_t>());
        r.id = r.decl_file + "#" + r.decl;
        r.near_decl = strings.at(rj.at(6).get<size_t>());
        r.near_file = strings.at(rj.at(7).get<size_t>());
        r.pos = pos(r.offset);
        t.refs.push_back(std::move(r));
    }
    for (const json& uj : j.at("u"))
    {
        hotc::UnresolvedName u;
        u.offset = uj.at(0).get<uint32_t>();
        u.length = uj.at(1).get<uint32_t>();
        u.pos = pos(u.offset);
        t.unresolved.push_back(u);
    }
    for (const json& w : j.at("w"))
        t.raw_ranges.emplace_back(w.at(0).get<uint32_t>(), w.at(1).get<uint32_t>());
    t.imports = j.at("i").get<std::vector<std::string>>();
    t.errors = j.at("e").get<std::vector<std::pair<int, std::string>>>();
    t.bases = j.at("b").get<std::vector<std::pair<std::string, std::string>>>();
    sort_refs(t);
    return t;
}

json shard_json(const IndexShard& shard)
{
    json classes = json::array();
    for (const IndexClass& c : shard.classes)
    {
        json members = json::array();
        for (const IndexMember& m : c.members)
        {
            json mj = {{"n", m.name}, {"k", m.kind}, {"p", pos_json(m.begin)}};
            if (m.refs)
                mj["r"] = places_json(*m.refs);
            members.push_back(std::move(mj));
        }
        classes.push_back(
            {{"n", c.name}, {"p", pos_json(c.begin)}, {"r", places_json(c.refs)}, {"m", std::move(members)}});
    }
    json body = {{"c", std::move(classes)}, {"t", table_json(shard.table)}};
    if (shard.has_table)
        body["tp"] = 1;
    return body;
}

IndexShard shard_of(const json& body)
{
    IndexShard shard;
    shard.table = table_of(body.at("t"));
    shard.has_table = body.contains("tp");
    for (const json& cj : body.at("c"))
    {
        IndexClass c;
        c.name = cj.at("n").get<std::string>();
        c.begin = pos_of(cj.at("p"));
        c.refs = places_of(cj.at("r"));
        for (const json& mj : cj.at("m"))
        {
            IndexMember m;
            m.name = mj.at("n").get<std::string>();
            m.kind = mj.at("k").get<std::string>();
            m.begin = pos_of(mj.at("p"));
            if (mj.contains("r"))
                m.refs = places_of(mj["r"]);
            c.members.push_back(std::move(m));
        }
        shard.classes.push_back(std::move(c));
    }
    return shard;
}

} // namespace

IndexMember* IndexClass::find_member(const std::string& member)
{
    for (IndexMember& m : members)
        if (m.name == member)
            return &m;
    return nullptr;
}

IndexClass* IndexShard::find_class(const std::string& name)
{
    for (IndexClass& c : classes)
        if (c.name == name)
            return &c;
    return nullptr;
}

void SymbolIndex::open(const std::string& dir)
{
    dir_ = dir;
}

std::string SymbolIndex::digest(const std::string& content)
{
    // Two FNV-1a runs with different starts, and the size.
    return hex16(fnv1a(content, 14695981039346656037ull)) + hex16(fnv1a(content, 0x9e3779b97f4a7c15ull)) + "-"
        + std::to_string(content.size());
}

namespace
{

std::atomic<long> g_digest_reads{0};

// Finds the names in the lines "using "x.hot"". The scan is the same as the regex
// \busing\s*"([^"\n]+\.hot)".
std::vector<std::string> using_names(const std::string& t)
{
    std::vector<std::string> out;
    const auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    size_t at = 0;
    while ((at = t.find("using", at)) != std::string::npos)
    {
        const size_t start = at;
        at += 5;
        if (start > 0 && is_word(t[start - 1]))
            continue;
        size_t i = at;
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
            ++i;
        if (i >= t.size() || t[i] != '"')
            continue;
        const size_t q = t.find_first_of("\"\n", i + 1);
        if (q == std::string::npos || t[q] != '"' || q - (i + 1) < 5)
            continue;
        if (t.compare(q - 4, 4, ".hot") != 0)
            continue;
        out.push_back(t.substr(i + 1, q - i - 1));
        at = q + 1;
    }
    return out;
}

// The key of a file in the "seen" set. The compiler reads an import through the OS, with the
// spelling "dir/name". So a "link/../x" path can be another file than "x". The key follows the OS
// for the parts that exist (weakly_canonical), and two spellings of one file give one key. It does
// not use the host set path_key: that one is only lexical on Linux.
// The path of a dependency in a stamp list. It has the form of the key of the server index
// (lexically_normal), so the index finds the importers of a path with "..".
std::string dep_key(const fs::path& p) { return p.lexically_normal().string(); }

std::string normal_key(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec)
        c = p.lexically_normal();
    std::string out = path_utf8(c);
#ifdef _WIN32
    std::replace(out.begin(), out.end(), '\\', '/');
    if (out.size() > 1 && out[1] == ':')
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
#endif
    return out;
}

struct FileStamp
{
    std::string path;
    std::uintmax_t size = 0;
    long long mtime = 0;
    bool operator==(const FileStamp& o) const { return path == o.path && size == o.size && mtime == o.mtime; }
};

FileStamp stamp_of(const std::string& path)
{
    FileStamp s;
    s.path = path;
    std::error_code ec;
    const fs::path p = utf8_path(path);
    s.size = fs::file_size(p, ec);
    if (ec)
        s.size = static_cast<std::uintmax_t>(-1);
    const auto t = fs::last_write_time(p, ec);
    s.mtime = ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
    return s;
}

} // namespace

long SymbolIndex::digest_file_reads()
{
    return g_digest_reads.load();
}

static std::string closure_digest(const std::string& script_path, const std::string& content,
    const std::vector<std::string>& include_dirs, std::vector<FileStamp>* deps)
{
    std::string all = content;
    std::set<std::string> seen; // the keys of the files that are read already
    // Each todo item has the spelling of the path (the compiler resolves the imports of a file
    // from the directory of this spelling) and the text.
    std::vector<std::pair<std::string, std::string>> todo = {{script_path, content}};
    seen.insert(normal_key(utf8_path(script_path)));
    while (!todo.empty())
    {
        const auto [path, text] = std::move(todo.back());
        todo.pop_back();
        for (const std::string& name : using_names(text))
        {
            std::vector<fs::path> dirs = {utf8_path(path).parent_path()};
            for (const std::string& d : include_dirs)
                dirs.push_back(utf8_path(d));
            for (const fs::path& d : dirs)
            {
                const fs::path spelling = d / utf8_path(name);
                const std::string file = normal_key(spelling);
                if (seen.count(file))
                    break; // read already
                // The stamp is taken before the read: a change after it makes the stamp old.
                const FileStamp stamp = stamp_of(path_utf8(spelling));
                std::ifstream in(spelling, std::ios::binary);
                if (!in)
                {
                    if (deps)
                        deps->push_back(FileStamp{dep_key(spelling), stamp.size, stamp.mtime}); // a missing file
                    continue;
                }
                seen.insert(file);
                ++g_digest_reads;
                std::stringstream ss;
                ss << in.rdbuf();
                std::string body = ss.str();
                all += '\0' + file + '\0' + body;
                if (deps)
                    deps->push_back(FileStamp{dep_key(spelling), stamp.size, stamp.mtime});
                todo.emplace_back(path_utf8(spelling), std::move(body));
                break;
            }
        }
    }
    return SymbolIndex::digest(all);
}

std::string SymbolIndex::digest(
    const std::string& script_path, const std::string& content, const std::vector<std::string>& include_dirs)
{
    return closure_digest(script_path, content, include_dirs, nullptr);
}

struct SymbolIndex::DigestEntry
{
    std::string content_digest;
    std::vector<std::string> include_dirs;
    std::vector<FileStamp> deps;
    std::string result;
};

SymbolIndex::SymbolIndex() = default;
SymbolIndex::~SymbolIndex() = default;

std::string SymbolIndex::cached_digest(const std::string& script_path, const std::string& content,
    const std::vector<std::string>& include_dirs, std::vector<DepStamp>* out_deps)
{
    const auto give = [out_deps](const DigestEntry& d)
    {
        if (!out_deps)
            return;
        out_deps->clear();
        for (const FileStamp& f : d.deps)
            out_deps->push_back(DepStamp{f.path, static_cast<uint64_t>(f.size), static_cast<int64_t>(f.mtime)});
    };
    const std::string cd = digest(content);
    std::shared_ptr<DigestEntry> old;
    {
        std::lock_guard<std::mutex> lock(digest_mutex_);
        auto it = digest_cache_.find(script_path);
        if (it != digest_cache_.end())
            old = it->second;
    }
    // The file reads run without the lock. An entry is not changed after the insert.
    if (old && old->content_digest == cd && old->include_dirs == include_dirs)
    {
        bool same = true;
        for (const FileStamp& d : old->deps)
            if (!(stamp_of(d.path) == d))
            {
                same = false;
                break;
            }
        if (same)
        {
            give(*old);
            return old->result;
        }
    }
    auto e = std::make_shared<DigestEntry>();
    e->content_digest = cd;
    e->include_dirs = include_dirs;
    e->result = closure_digest(script_path, content, include_dirs, &e->deps);
    {
        std::lock_guard<std::mutex> lock(digest_mutex_);
        digest_cache_[script_path] = e;
    }
    give(*e);
    return e->result;
}

std::string SymbolIndex::shard_path(const std::string& script_path) const
{
    return (fs::path(dir_) / (hex16(fnv1a(script_path, 14695981039346656037ull)) + ".idx")).string();
}

std::optional<IndexShard> SymbolIndex::load(const std::string& script_path, const std::string& dig)
{
    if (!enabled())
        return std::nullopt;
    const std::string file = shard_path(script_path);
    std::optional<Header> bad; // the header of a shard to delete
    std::optional<IndexShard> result;
    {
        std::shared_lock<std::shared_mutex> lock(shard_mutex_);
        std::ifstream in(file, std::ios::binary);
        std::string line;
        if (!in || !std::getline(in, line))
            return std::nullopt;
        const auto h = parse_header(line);
        if (!h)
            return std::nullopt; // no shard (or a file that is not a shard: store() replaces it)
        if (h->version != kFormatVersion)
            bad = h;
        else if (h->path == script_path && h->digest == dig)
        {
            const auto t0 = std::chrono::steady_clock::now();
            try
            {
                std::stringstream body;
                body << in.rdbuf();
                IndexShard shard = shard_of(json::parse(body.str()));
                if (shard.has_table)
                {
                    ++loads_;
                    load_us_ += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    result = std::move(shard);
                }
                else
                    bad = h; // a shard with no table
            }
            catch (const std::exception&)
            {
                bad = h; // a broken shard
            }
        }
    }
    if (bad)
    {
        // The delete needs the exclusive lock. A store() can replace the file after the shared
        // lock ends, so the delete happens only when the header is the same.
        std::unique_lock<std::shared_mutex> lock(shard_mutex_);
        const auto now = read_header(file);
        if (now && now->version == bad->version && now->digest == bad->digest && now->path == bad->path)
        {
            std::error_code ec;
            fs::remove(file, ec);
        }
        return std::nullopt;
    }
    return result;
}

bool SymbolIndex::store(const std::string& script_path, const std::string& dig, const IndexShard& shard)
{
    if (!enabled() || script_path.find('\n') != std::string::npos)
        return false;
    std::shared_lock<std::shared_mutex> lock(shard_mutex_);
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const std::string file = shard_path(script_path);
    static std::atomic<unsigned long long> tmp_counter{0};
    const std::string tmp = file + "." + std::to_string(hot::detail::process_id()) + "." +
                            std::to_string(tmp_counter.fetch_add(1)) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << kMagic << ' ' << kFormatVersion << ' ' << dig << ' ' << script_path << '\n' << shard_json(shard).dump();
        out.flush();
        if (!out)
        {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec); // atomic: a reader sees the old or the new shard
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    ++stores_;
    return true;
}

int SymbolIndex::prune()
{
    if (!enabled())
        return 0;
    std::unique_lock<std::shared_mutex> lock(shard_mutex_);
    int removed = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::path p = it->path();
        std::error_code ec2;
        if (p.extension() == ".tmp")
        {
            // A temporary file of a crashed write. A young file can belong to a writer.
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(p, ec2);
            if (!ec2 && age > std::chrono::minutes(1) && fs::remove(p, ec2))
                ++removed;
            continue;
        }
        if (p.extension() != ".idx")
            continue;
        const auto h = read_header(p.string());
        if (!h || h->version != kFormatVersion || !fs::exists(h->path, ec2))
        {
            if (fs::remove(p, ec2))
                ++removed;
        }
    }
    return removed;
}

int SymbolIndex::remove_if(const std::function<bool(const std::string&)>& match)
{
    if (!enabled())
        return 0;
    std::unique_lock<std::shared_mutex> lock(shard_mutex_);
    int removed = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::path p = it->path();
        if (p.extension() != ".idx")
            continue;
        const auto h = read_header(p.string());
        std::error_code ec2;
        if (h && match(h->path) && fs::remove(p, ec2))
            ++removed;
    }
    return removed;
}

} // namespace hot::ls
