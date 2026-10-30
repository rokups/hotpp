#include "symbol_index.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unistd.h>

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

// Reads the first line of a shard file.
std::optional<Header> read_header(const std::string& file)
{
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line))
        return std::nullopt;
    std::istringstream ss(line);
    std::string magic;
    Header h;
    if (!(ss >> magic >> h.version >> h.digest) || magic != kMagic)
        return std::nullopt;
    ss.get(); // the space before the path
    std::getline(ss, h.path);
    return h;
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
    return classes;
}

IndexShard shard_of(const json& classes)
{
    IndexShard shard;
    for (const json& cj : classes)
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

std::string SymbolIndex::digest(
    const std::string& script_path, const std::string& content, const std::vector<std::string>& include_dirs)
{
    static const std::regex using_line(R"re(\busing\s*"([^"\n]+\.hot)")re");
    std::string all = content;
    std::set<std::string> seen; // the files that are read already
    std::vector<std::pair<std::string, std::string>> todo = {{script_path, content}};
    while (!todo.empty())
    {
        const auto [path, text] = todo.back();
        todo.pop_back();
        for (std::sregex_iterator it(text.begin(), text.end(), using_line), end; it != end; ++it)
        {
            const std::string name = (*it)[1].str();
            std::vector<fs::path> dirs = {fs::path(path).parent_path()};
            for (const std::string& d : include_dirs)
                dirs.push_back(d);
            for (const fs::path& d : dirs)
            {
                const std::string file = (d / name).string();
                std::ifstream in(file, std::ios::binary);
                if (!in)
                    continue;
                if (seen.insert(file).second)
                {
                    std::stringstream ss;
                    ss << in.rdbuf();
                    all += '\0' + file + '\0' + ss.str();
                    todo.emplace_back(file, ss.str());
                }
                break;
            }
        }
    }
    return digest(all);
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
    const auto h = read_header(file);
    if (!h)
        return std::nullopt; // no shard (or a file that is not a shard: store() replaces it)
    std::error_code ec;
    if (h->version != kFormatVersion)
    {
        fs::remove(file, ec);
        return std::nullopt;
    }
    if (h->path != script_path || h->digest != dig)
        return std::nullopt;
    try
    {
        std::ifstream in(file, std::ios::binary);
        std::string line;
        std::getline(in, line);
        std::stringstream body;
        body << in.rdbuf();
        IndexShard shard = shard_of(json::parse(body.str()));
        ++loads_;
        return shard;
    }
    catch (const std::exception&)
    {
        fs::remove(file, ec); // a broken shard
        return std::nullopt;
    }
}

bool SymbolIndex::store(const std::string& script_path, const std::string& dig, const IndexShard& shard)
{
    if (!enabled() || script_path.find('\n') != std::string::npos)
        return false;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const std::string file = shard_path(script_path);
    const std::string tmp = file + "." + std::to_string(::getpid()) + ".tmp";
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

} // namespace hot::ls
