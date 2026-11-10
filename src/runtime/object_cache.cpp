// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/object_cache.h"

#include "hot/runtime/process.h"

#include "cache_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <sstream>

namespace hot
{

namespace fs = std::filesystem;

namespace cache_io
{

// ------------------------------------------------------------ Files

int64_t to_ns(fs::file_time_type t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// The path of a temporary file in the directory of "final_path".
fs::path temp_name(const fs::path& final_path)
{
    static std::atomic<unsigned> counter{0};
    return final_path.parent_path() /
        (final_path.filename().string() + ".tmp" + std::to_string(detail::process_id()) + "." +
            std::to_string(counter.fetch_add(1)));
}

// Writes the file with a temporary file and a rename.
bool write_file_atomic(const fs::path& final_path, const std::string& content, std::string& error)
{
    const fs::path tmp = temp_name(final_path);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out)
        {
            error = "cannot write " + tmp.string();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, final_path, ec);
    if (ec)
    {
        error = "cannot rename " + tmp.string() + ": " + ec.message();
        std::error_code ec2;
        fs::remove(tmp, ec2);
        return false;
    }
    return true;
}

bool read_file(const fs::path& path, std::string& content)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        return false;
    content = ss.str();
    return true;
}

// ------------------------------------------------------------ Small JSON

class JsonParser
{
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse(Json& out)
    {
        if (!value(out, 0))
            return false;
        skip_space();
        return pos_ == s_.size();
    }

private:
    void skip_space()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\t' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool value(Json& out, int depth)
    {
        if (depth > 8)
            return false;
        skip_space();
        if (pos_ >= s_.size())
            return false;
        const char c = s_[pos_];
        if (c == '"')
        {
            out.type = Json::Type::String;
            return string(out.text);
        }
        if (c == '-' || (c >= '0' && c <= '9'))
            return number(out);
        if (c == '[')
        {
            out.type = Json::Type::Array;
            ++pos_;
            skip_space();
            if (pos_ < s_.size() && s_[pos_] == ']')
            {
                ++pos_;
                return true;
            }
            for (;;)
            {
                Json item;
                if (!value(item, depth + 1))
                    return false;
                out.items.push_back(std::move(item));
                skip_space();
                if (pos_ >= s_.size())
                    return false;
                if (s_[pos_] == ',')
                {
                    ++pos_;
                    continue;
                }
                if (s_[pos_] == ']')
                {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '{')
        {
            out.type = Json::Type::Object;
            ++pos_;
            skip_space();
            if (pos_ < s_.size() && s_[pos_] == '}')
            {
                ++pos_;
                return true;
            }
            for (;;)
            {
                skip_space();
                std::string name;
                if (pos_ >= s_.size() || s_[pos_] != '"' || !string(name))
                    return false;
                skip_space();
                if (pos_ >= s_.size() || s_[pos_] != ':')
                    return false;
                ++pos_;
                Json member;
                if (!value(member, depth + 1))
                    return false;
                out.members.emplace_back(std::move(name), std::move(member));
                skip_space();
                if (pos_ >= s_.size())
                    return false;
                if (s_[pos_] == ',')
                {
                    ++pos_;
                    continue;
                }
                if (s_[pos_] == '}')
                {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    bool number(Json& out)
    {
        out.type = Json::Type::Number;
        bool negative = false;
        if (s_[pos_] == '-')
        {
            negative = true;
            ++pos_;
        }
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9')
            return false;
        uint64_t v = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
        {
            const uint64_t digit = static_cast<uint64_t>(s_[pos_] - '0');
            if (v > (UINT64_MAX - digit) / 10)
                return false;
            v = v * 10 + digit;
            ++pos_;
        }
        out.number = static_cast<int64_t>(negative ? (~v + 1) : v);
        return true;
    }

    bool string(std::string& out)
    {
        ++pos_; // the opening quote
        while (pos_ < s_.size())
        {
            const char c = s_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size())
                return false;
            const char e = s_[pos_++];
            switch (e)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                if (pos_ + 4 > s_.size())
                    return false;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char h = s_[pos_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9')
                        code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        code |= static_cast<unsigned>(h - 'A' + 10);
                    else
                        return false;
                }
                // The writer escapes only control characters, so a code above 0x7f is rare.
                if (code < 0x80)
                    out.push_back(static_cast<char>(code));
                else if (code < 0x800)
                {
                    out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

void append_json_string(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            }
            else
                out.push_back(c);
        }
    }
    out.push_back('"');
}

bool parse_json(const std::string& text, Json& out)
{
    return JsonParser(text).parse(out);
}

} // namespace cache_io

namespace
{

using namespace cache_io;

constexpr const char* work_marker = "@work@";
constexpr int manifest_version = 1;
constexpr uint64_t second_seed = 0x9e3779b97f4a7c15ULL;

// ------------------------------------------------------------ Paths

std::string slashes(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// The work directory without the separator at the end.
std::string trim_dir(std::string dir)
{
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\'))
        dir.pop_back();
    return dir;
}

// True when "path" is "dir" or is below it. The two separators are the same. "dir" has no
// separator at the end.
bool is_under(const std::string& path, const std::string& dir)
{
    if (dir.empty() || path.size() < dir.size())
        return false;
    if (slashes(path.substr(0, dir.size())) != slashes(dir))
        return false;
    return path.size() == dir.size() || path[dir.size()] == '/' || path[dir.size()] == '\\';
}

// ------------------------------------------------------------ Entries

struct Stamp
{
    std::string path; // with "@work@" for a file in the work directory
    int64_t mtime_ns = 0;
    int64_t size = 0;
};

bool is_temp_file(const std::string& name)
{
    return name.find(".tmp") != std::string::npos;
}

bool older_than_one_hour(const fs::path& p)
{
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(p, ec);
    if (ec)
        return false;
    return fs::file_time_type::clock::now() - t > std::chrono::hours(1);
}

} // namespace

// ------------------------------------------------------------ Key

std::string ObjectCache::Key::hex() const
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

ObjectCache::Key ObjectCache::make_key(const KeyInputs& inputs)
{
    std::vector<std::pair<std::string, std::string>> files = inputs.virtual_files;
    std::sort(files.begin(), files.end());

    Hash64 h1;
    Hash64 h2(second_seed);
    for (Hash64* h : {&h1, &h2})
    {
        h->add_text("hot-object-cache-1");
        h->add_text(inputs.cpp_text);
        h->add_u64(inputs.args.size());
        for (const std::string& arg : inputs.args)
            h->add_text(arg);
        h->add_text(inputs.compiler_id);
        h->add_u64(files.size());
        for (const auto& f : files)
        {
            h->add_text(f.first);
            h->add_text(f.second);
        }
    }
    return Key{h1.value(), h2.value()};
}

std::vector<std::string> ObjectCache::normalize_args(const std::vector<std::string>& args, const std::string& work_dir)
{
    const std::string dir = slashes(trim_dir(work_dir));
    std::vector<std::string> result;
    result.reserve(args.size());
    for (const std::string& arg : args)
    {
        if (dir.empty())
        {
            result.push_back(arg);
            continue;
        }
        const std::string flat = slashes(arg);
        std::string out;
        size_t pos = 0;
        while (pos < arg.size())
        {
            const size_t found = flat.find(dir, pos);
            if (found == std::string::npos)
                break;
            const size_t end = found + dir.size();
            // The work directory is a whole path: it is at the start, after "=", or after a short flag.
            const bool at_start = found == 0 || arg[found - 1] == '=' || (arg[0] == '-' && found <= 3);
            const bool at_end = end == arg.size() || flat[end] == '/';
            if (!at_start || !at_end)
            {
                out.append(arg, pos, end - pos);
                pos = end;
                continue;
            }
            out.append(arg, pos, found - pos);
            out += work_marker;
            pos = end;
        }
        out.append(arg, pos, std::string::npos);
        result.push_back(std::move(out));
    }
    return result;
}

bool ObjectCache::enabled_by_env()
{
    const char* env = std::getenv("HOT_CACHE");
    return !(env && std::string(env) == "0");
}

// ------------------------------------------------------------ Depfile

bool ObjectCache::parse_depfile(const std::string& text, std::vector<std::string>& deps)
{
    deps.clear();
    // The logical lines. A backslash and a line end join two lines.
    std::vector<std::vector<std::string>> lines(1);
    std::string token;
    bool in_token = false;
    auto flush = [&]()
    {
        if (in_token)
            lines.back().push_back(token);
        token.clear();
        in_token = false;
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size())
        {
            const char n = text[i + 1];
            if (n == '\n')
            {
                flush();
                ++i;
            }
            else if (n == '\r' && i + 2 < text.size() && text[i + 2] == '\n')
            {
                flush();
                i += 2;
            }
            else if (n == ' ' || n == '#' || n == '\\')
            {
                // An escaped space, "#" or backslash. A backslash before another character is
                // a Windows separator: it stays.
                token.push_back(n);
                in_token = true;
                ++i;
            }
            else
            {
                token.push_back('\\');
                in_token = true;
            }
        }
        else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$')
        {
            token.push_back('$');
            in_token = true;
            ++i;
        }
        else if (c == '\n')
        {
            flush();
            if (!lines.back().empty())
                lines.emplace_back();
        }
        else if (c == ' ' || c == '\t' || c == '\r')
            flush();
        else
        {
            token.push_back(c);
            in_token = true;
        }
    }
    flush();

    // The first rule has the target. The headers follow the token that ends with ":".
    const std::vector<std::string>& rule = lines.front();
    size_t colon = rule.size();
    for (size_t i = 0; i < rule.size(); ++i)
    {
        if (!rule[i].empty() && rule[i].back() == ':')
        {
            colon = i;
            break;
        }
    }
    if (colon == rule.size())
        return false;
    for (size_t i = colon + 1; i < rule.size(); ++i)
        if (std::find(deps.begin(), deps.end(), rule[i]) == deps.end())
            deps.push_back(rule[i]);
    return true;
}

// ------------------------------------------------------------ Cache

ObjectCache::ObjectCache(fs::path dir, uint64_t max_bytes) : dir_(std::move(dir)), max_bytes_(max_bytes) {}

fs::path ObjectCache::obj_dir() const
{
    return dir_ / "obj";
}

fs::path ObjectCache::entry_file(const Key& key, const char* ext) const
{
    return obj_dir() / (key.hex() + ext);
}

fs::path ObjectCache::object_path(const Key& key) const
{
    return entry_file(key, ".o");
}

ObjectCache::Stats ObjectCache::stats() const
{
    Stats s;
    s.hits = hits_.load();
    s.misses = misses_.load();
    return s;
}

std::optional<ObjectCache::Hit> ObjectCache::lookup(const Key& key)
{
    std::optional<Hit> hit;
    try
    {
        const fs::path manifest = entry_file(key, ".json");
        const fs::path object = entry_file(key, ".o");
        std::string text;
        Json root;
        bool ok = read_file(manifest, text) && parse_json(text, root) && root.type == Json::Type::Object;
        if (ok)
        {
            const Json* version = root.find("version");
            const Json* hex = root.find("key");
            const Json* headers = root.find("headers");
            ok = version && version->type == Json::Type::Number && version->number == manifest_version && hex &&
                hex->type == Json::Type::String && hex->text == key.hex() && headers &&
                headers->type == Json::Type::Array;
            if (ok)
            {
                for (const Json& h : headers->items)
                {
                    const Json* p = h.find("path");
                    const Json* m = h.find("mtime");
                    const Json* s = h.find("size");
                    if (!p || !m || !s || p->type != Json::Type::String || m->type != Json::Type::Number ||
                        s->type != Json::Type::Number)
                    {
                        ok = false;
                        break;
                    }
                    if (p->text.compare(0, std::char_traits<char>::length(work_marker), work_marker) == 0)
                        continue;
                    std::error_code ec;
                    const fs::file_time_type t = fs::last_write_time(fs::path(p->text), ec);
                    const uintmax_t size = ec ? 0 : fs::file_size(fs::path(p->text), ec);
                    if (ec || to_ns(t) != m->number || static_cast<int64_t>(size) != s->number)
                    {
                        ok = false;
                        break;
                    }
                }
            }
        }
        std::error_code ec;
        if (ok && fs::is_regular_file(object, ec))
        {
            std::error_code touch_ec;
            fs::last_write_time(manifest, fs::file_time_type::clock::now(), touch_ec);
            hit = Hit{object};
        }
    }
    catch (...)
    {
        hit.reset();
    }
    if (hit)
        ++hits_;
    else
        ++misses_;
    return hit;
}

bool ObjectCache::store(const Key& key, const fs::path& obj_tmp, const std::string& depfile_text,
    const std::string& work_dir, std::string& error)
{
    try
    {
        std::vector<std::string> deps;
        if (!parse_depfile(depfile_text, deps))
        {
            error = "cannot parse the depfile";
            return false;
        }
        const std::string work = trim_dir(work_dir);
        std::vector<Stamp> stamps;
        std::string list;
        for (const std::string& dep : deps)
        {
            Stamp st;
            if (is_under(dep, work))
                st.path = work_marker + dep.substr(work.size());
            else
            {
                st.path = dep;
                std::error_code ec;
                const fs::file_time_type t = fs::last_write_time(fs::path(dep), ec);
                const uintmax_t size = ec ? 0 : fs::file_size(fs::path(dep), ec);
                if (ec)
                {
                    error = "cannot stat " + dep + ": " + ec.message();
                    return false;
                }
                st.mtime_ns = to_ns(t);
                st.size = static_cast<int64_t>(size);
            }
            list += st.path;
            list += '\n';
            stamps.push_back(std::move(st));
        }

        std::error_code ec;
        fs::create_directories(obj_dir(), ec);
        if (ec)
        {
            error = "cannot create " + obj_dir().string() + ": " + ec.message();
            return false;
        }

        // The old manifest goes first, so a reader never pairs it with a new object file.
        fs::remove(entry_file(key, ".json"), ec);

        if (!write_file_atomic(entry_file(key, ".d"), list, error))
            return false;

        // The object file: a rename. A copy is the fallback for another file system.
        const fs::path object = entry_file(key, ".o");
        fs::rename(obj_tmp, object, ec);
        if (ec)
        {
            const fs::path copy = temp_name(object);
            std::error_code ec2;
            fs::copy_file(obj_tmp, copy, fs::copy_options::overwrite_existing, ec2);
            if (!ec2)
                fs::rename(copy, object, ec2);
            if (ec2)
            {
                std::error_code ec3;
                fs::remove(copy, ec3);
                error = "cannot move " + obj_tmp.string() + ": " + ec2.message();
                return false;
            }
            std::error_code ec4;
            fs::remove(obj_tmp, ec4);
        }

        std::string json = "{\"version\":" + std::to_string(manifest_version) + ",\"key\":";
        append_json_string(json, key.hex());
        json += ",\"headers\":[";
        for (size_t i = 0; i < stamps.size(); ++i)
        {
            if (i)
                json += ',';
            json += "{\"path\":";
            append_json_string(json, stamps[i].path);
            json += ",\"mtime\":" + std::to_string(stamps[i].mtime_ns);
            json += ",\"size\":" + std::to_string(stamps[i].size) + "}";
        }
        json += "]}\n";
        return write_file_atomic(entry_file(key, ".json"), json, error);
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

void ObjectCache::remove(const Key& key)
{
    std::error_code ec;
    fs::remove(entry_file(key, ".json"), ec);
    fs::remove(entry_file(key, ".o"), ec);
    fs::remove(entry_file(key, ".d"), ec);
}

void ObjectCache::trim()
{
    try
    {
        struct Entry
        {
            std::string stem;
            uint64_t size = 0;
            bool has_manifest = false;
            fs::file_time_type mtime;
            std::vector<fs::path> files;
        };
        std::map<std::string, Entry> entries;
        // The object entries and the translate entries (TranslateCache, "<dir>/tr") share the limit.
        std::error_code ec;
        for (const fs::path& sub : {obj_dir(), dir_ / "tr"})
        {
        std::error_code dec;
        for (fs::directory_iterator it(sub, dec), end; !dec && it != end; it.increment(dec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec))
                continue;
            const fs::path p = it->path();
            const std::string name = p.filename().string();
            if (is_temp_file(name))
            {
                // A young temporary file belongs to a running store.
                if (older_than_one_hour(p))
                    fs::remove(p, fec);
                continue;
            }
            Entry& e = entries[(sub.filename() / p.stem()).string()];
            e.stem = p.stem().string();
            e.files.push_back(p);
            const uintmax_t size = fs::file_size(p, fec);
            if (!fec)
                e.size += size;
            if (p.extension() == ".json")
            {
                e.has_manifest = true;
                e.mtime = fs::last_write_time(p, fec);
            }
        }
        }

        std::vector<Entry> complete;
        uint64_t total = 0;
        for (auto& kv : entries)
        {
            Entry& e = kv.second;
            if (e.has_manifest)
            {
                total += e.size;
                complete.push_back(std::move(e));
            }
            else
            {
                // An entry without a manifest is a miss. A young one can belong to a running store.
                for (const fs::path& f : e.files)
                    if (older_than_one_hour(f))
                        fs::remove(f, ec);
            }
        }

        if (max_bytes_ == 0 || total <= max_bytes_)
            return;
        const uint64_t goal = max_bytes_ / 4 * 3;
        std::sort(complete.begin(), complete.end(),
            [](const Entry& a, const Entry& b) { return a.mtime != b.mtime ? a.mtime < b.mtime : a.stem < b.stem; });
        for (const Entry& e : complete)
        {
            if (total <= goal)
                break;
            // The manifest goes first.
            std::vector<fs::path> files = e.files;
            std::stable_partition(files.begin(), files.end(), [](const fs::path& f) { return f.extension() == ".json"; });
            for (const fs::path& f : files)
                fs::remove(f, ec);
            total -= std::min(total, e.size);
        }
    }
    catch (...)
    {
    }
}

} // namespace hot
