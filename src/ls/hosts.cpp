// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hosts.h"
#include "long_path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace hot::ls
{

using json = nlohmann::json;

namespace
{

constexpr long kExactScore = 1000000;

std::string get_string(const json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> get_strings(const json& j, const char* key)
{
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return out;
    for (const json& v : *it)
        if (v.is_string())
            out.push_back(v.get<std::string>());
    return out;
}

// Reads the lines of a side file. A missing file gives no line. The code removes a trailing
// "\r" and skips the empty lines. A name with a directory part is relative to dir.
std::vector<std::string> read_lines(const fs::path& dir, const std::string& name)
{
    std::vector<std::string> out;
    if (name.empty())
        return out;
    std::ifstream in(dir / utf8_path(name));
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            out.push_back(std::move(line));
    }
    return out;
}

// True for a file that makes a host: the JSON file and its three side files.
bool is_host_file(const fs::path& p)
{
    const std::string ext = p.extension().string();
    return ext == ".json" || ext == ".defines" || ext == ".options" || ext == ".includes";
}

// The form of a path that the host set compares: lexically normal, with "/" as the separator.
// On Windows a drive letter is upper case ("c:\\x" and "C:/x" are the same path). The host files
// and the editor can give either separator, so both sides of a compare use this form. On Windows
// the 8.3 short names become long names ("RUNNER~1" is "runneradmin"), as in the editor URIs.
// On macOS the path becomes the real path ("/var/x" is "/private/var/x"), as the server makes
// the document paths with long_path. Linux keeps the path.
std::string path_key(const fs::path& p)
{
    // The callers make p with utf8_path, so the UTF-8 text of p is the text of the caller.
    std::string out = long_path(path_utf8(p.lexically_normal()));
#ifdef _WIN32
    std::replace(out.begin(), out.end(), '\\', '/');
#endif
#ifdef _WIN32
    if (out.size() > 1 && out[1] == ':')
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
#endif
    return out;
}

// Removes the trailing slashes. The root directory stays "/" and a drive root stays "C:/".
std::string strip_slash(std::string s)
{
    while (s.size() > 1 && s.back() == '/' && s[s.size() - 2] != ':')
        s.pop_back();
    return s;
}

// Makes a path absolute against base (when the path is relative) and normalizes it with no
// access to the file system (path_key). The result has no trailing slash. An empty path stays
// empty.
std::string resolve_path(const std::string& base, const std::string& path)
{
    if (path.empty())
        return path;
    fs::path p = utf8_path(path);
    if (p.is_relative() && !base.empty())
        p = utf8_path(base) / p;
    return strip_slash(path_key(p));
}

// Resolves all paths against base. With unique, a repeated path is dropped (the first stays).
std::vector<std::string> resolve_paths(const std::string& base, const std::vector<std::string>& in, bool unique)
{
    std::vector<std::string> out;
    for (const std::string& s : in)
    {
        std::string r = resolve_path(base, s);
        if (r.empty())
            continue;
        if (unique && std::find(out.begin(), out.end(), r) != out.end())
            continue;
        out.push_back(std::move(r));
    }
    return out;
}

// True when dir contains path: the path starts with dir and the next character is a "/". A root
// ("/" or "C:/") contains each path that starts with it.
bool contains(const std::string& dir, const std::string& path)
{
    if (dir.empty() || path.compare(0, dir.size(), dir) != 0)
        return false;
    return dir.back() == '/' || path.size() == dir.size() || path[dir.size()] == '/';
}

// True when the build directory of the host has a CMakeCache.txt.
bool has_cache(const Host& h)
{
    std::error_code ec;
    return fs::exists(utf8_path(h.binary_dir) / "CMakeCache.txt", ec);
}

// Gives the sorted names of the "*.json" files in dir. A refresh compares them, because the
// modification time of a directory can have a coarse resolution (for example, one second on
// Windows). Then a new host file does not always change the time.
std::vector<std::string> json_names(const std::string& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(utf8_path(dir), ec))
    {
        if (e.is_regular_file(ec) && e.path().extension() == ".json")
            out.push_back(path_utf8(e.path().filename()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

std::vector<std::string> Host::cxx_flags() const
{
    std::vector<std::string> out = flags;
    for (const std::string& d : defines)
        out.push_back("-D" + d);
    return out;
}

void HostSet::load(const std::string& dir, bool require_cache)
{
    dir_ = dir;
    require_cache_ = require_cache;
    loaded_ = true;
    hosts_.clear();
    skipped_.clear();
    std::error_code ec;
    mtime_ = fs::last_write_time(dir, ec);
    names_ = json_names(dir);
    if (ec)
        return;
    for (const fs::directory_entry& e : fs::directory_iterator(utf8_path(dir), ec))
    {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json")
            continue;
        std::ifstream in(e.path());
        std::ostringstream ss;
        ss << in.rdbuf();
        const json j = json::parse(ss.str(), nullptr, false);
        if (!j.is_object() || !j.contains("version") || j["version"] != 1)
            continue;
        auto host = std::make_shared<Host>();
        host->name = get_string(j, "name");
        if (host->name.empty())
            host->name = path_utf8(e.path().stem());
        host->binary_dir = get_string(j, "binaryDir");
        host->compiler = get_string(j, "compiler");
        host->std_version = get_string(j, "std");
        host->resource_dir = get_string(j, "resourceDir");
        const std::string source_dir = get_string(j, "sourceDir");
        host->script_dirs = resolve_paths(source_dir, get_strings(j, "scriptDirs"), false);
        host->scripts = resolve_paths(source_dir, get_strings(j, "scripts"), false);
        host->preludes = resolve_paths(source_dir, get_strings(j, "preludes"), false);
        host->prelude_suffix = get_string(j, "preludeSuffix");
        // The first entry of "includeDirs" is the project dir. The dirs of the target (from the
        // side file) come next, then the other entries. Each relative path is relative to
        // "sourceDir". A repeated dir is dropped; the first one stays.
        std::vector<std::string> inc = get_strings(j, "includeDirs");
        std::vector<std::string> ordered;
        if (!inc.empty())
            ordered.push_back(inc.front());
        for (std::string& d : read_lines(e.path().parent_path(), get_string(j, "includesFile")))
            ordered.push_back(std::move(d));
        if (inc.size() > 1)
            ordered.insert(ordered.end(), inc.begin() + 1, inc.end());
        host->include_dirs = resolve_paths(source_dir, ordered, true);
        // The options of the target come first, then the explicit flags (the last flag wins).
        host->flags = read_lines(e.path().parent_path(), get_string(j, "optionsFile"));
        for (std::string& f : get_strings(j, "flags"))
            host->flags.push_back(std::move(f));
        host->defines = get_strings(j, "defines");
        for (std::string& d : read_lines(e.path().parent_path(), get_string(j, "definesFile")))
            host->defines.push_back(std::move(d));
        if (require_cache && !has_cache(*host))
        {
            skipped_.push_back(host); // refresh() checks it again
            continue;
        }
        hosts_.push_back(std::move(host));
    }
    std::sort(hosts_.begin(), hosts_.end(), [](const auto& a, const auto& b) { return a->name < b->name; });
}

bool HostSet::refresh()
{
    if (!loaded_)
        return false;
    std::error_code ec;
    const auto now = fs::last_write_time(dir_, ec);
    const bool same_time = ec ? mtime_ == fs::file_time_type{} : now == mtime_;
    if (forced_ || !same_time || json_names(dir_) != names_)
    {
        forced_ = false;
        load(dir_, require_cache_);
        return true;
    }
    if (!require_cache_)
        return false;
    // The directory is the same. A build directory can still appear or vanish.
    bool changed = false;
    for (auto it = skipped_.begin(); it != skipped_.end();)
    {
        if (has_cache(**it))
        {
            hosts_.push_back(*it);
            it = skipped_.erase(it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }
    for (auto it = hosts_.begin(); it != hosts_.end();)
    {
        if (has_cache(**it))
        {
            ++it;
        }
        else
        {
            skipped_.push_back(*it);
            it = hosts_.erase(it);
            changed = true;
        }
    }
    if (changed)
        std::sort(hosts_.begin(), hosts_.end(), [](const auto& a, const auto& b) { return a->name < b->name; });
    return changed;
}

bool HostSet::force_reload_for(const std::string& file)
{
    const fs::path f = utf8_path(file);
    if (!loaded_ || !is_host_file(f))
        return false;
    if (path_key(f.parent_path()) != path_key(utf8_path(dir_))) // the long names and "/" (Windows)
        return false;
    forced_ = true;
    return true;
}

std::shared_ptr<const Host> HostSet::match(const std::string& script_path) const
{
    const std::string path = path_key(utf8_path(script_path));
    std::shared_ptr<const Host> best;
    long best_score = 0;
    for (const auto& h : hosts_)
    { // sorted: the first host wins a tie
        long score = 0;
        for (const std::string& s : h->scripts)
            if (s == path)
                score = kExactScore;
        if (score == 0)
        {
            for (const std::string& d : h->script_dirs)
            {
                const std::string dir = strip_slash(d);
                if (contains(dir, path))
                    score = std::max(score, static_cast<long>(dir.size()));
            }
        }
        if (score > best_score)
        {
            best_score = score;
            best = h;
        }
    }
    return best;
}

std::shared_ptr<const Host> HostSet::nearest(const std::string& script_path, fs::path& common) const
{
    const fs::path script_dir = utf8_path(path_key(utf8_path(script_path))).parent_path();
    std::shared_ptr<const Host> best;
    size_t best_count = 0;
    for (const auto& h : hosts_)
    {
        std::vector<fs::path> dirs;
        for (const std::string& d : h->script_dirs)
            dirs.push_back(utf8_path(strip_slash(d)));
        for (const std::string& s : h->scripts)
            dirs.push_back(utf8_path(s).parent_path());
        for (const fs::path& d : dirs)
        {
            const fs::path norm = d.lexically_normal();
            fs::path out;
            size_t n = 0;
            auto a = script_dir.begin();
            auto b = norm.begin();
            for (; a != script_dir.end() && b != norm.end() && *a == *b; ++a, ++b, ++n)
                out /= *a;
            if (n > best_count)
            {
                best_count = n;
                best = h;
                common = out;
            }
        }
    }
    return best;
}

} // namespace hot::ls
