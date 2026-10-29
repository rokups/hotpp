#include "hosts.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace spp::ls {

using json = nlohmann::json;

namespace {

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
    if (it == j.end() || !it->is_array()) return out;
    for (const json& v : *it)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

// Removes the trailing slashes. The root directory stays "/".
std::string strip_slash(std::string s)
{
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// True when dir contains path: the path starts with dir and the next character is a "/".
bool contains(const std::string& dir, const std::string& path)
{
    if (dir.empty() || path.compare(0, dir.size(), dir) != 0) return false;
    return dir == "/" || path.size() == dir.size() || path[dir.size()] == '/';
}

// True when the build directory of the host has a CMakeCache.txt.
bool has_cache(const Host& h)
{
    std::error_code ec;
    return fs::exists(fs::path(h.binary_dir) / "CMakeCache.txt", ec);
}

}  // namespace

std::vector<std::string> Host::cxx_flags() const
{
    std::vector<std::string> out = flags;
    for (const std::string& d : defines) out.push_back("-D" + d);
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
    if (ec) return;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
        std::ifstream in(e.path());
        std::ostringstream ss;
        ss << in.rdbuf();
        const json j = json::parse(ss.str(), nullptr, false);
        if (!j.is_object() || !j.contains("version") || j["version"] != 1) continue;
        auto host = std::make_shared<Host>();
        host->name = get_string(j, "name");
        if (host->name.empty()) host->name = e.path().stem().string();
        host->binary_dir = get_string(j, "binaryDir");
        host->compiler = get_string(j, "compiler");
        host->std_version = get_string(j, "std");
        host->resource_dir = get_string(j, "resourceDir");
        host->script_dirs = get_strings(j, "scriptDirs");
        host->scripts = get_strings(j, "scripts");
        host->include_dirs = get_strings(j, "includeDirs");
        host->defines = get_strings(j, "defines");
        host->flags = get_strings(j, "flags");
        if (require_cache && !has_cache(*host)) {
            skipped_.push_back(host);  // refresh() checks it again
            continue;
        }
        hosts_.push_back(std::move(host));
    }
    std::sort(hosts_.begin(), hosts_.end(),
        [](const auto& a, const auto& b) { return a->name < b->name; });
}

bool HostSet::refresh()
{
    if (!loaded_) return false;
    std::error_code ec;
    const auto now = fs::last_write_time(dir_, ec);
    if (!(ec ? mtime_ == fs::file_time_type{} : now == mtime_)) {
        load(dir_, require_cache_);
        return true;
    }
    if (!require_cache_) return false;
    // The directory is the same. A build directory can still appear or vanish.
    bool changed = false;
    for (auto it = skipped_.begin(); it != skipped_.end();) {
        if (has_cache(**it)) {
            hosts_.push_back(*it);
            it = skipped_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (auto it = hosts_.begin(); it != hosts_.end();) {
        if (has_cache(**it)) {
            ++it;
        } else {
            skipped_.push_back(*it);
            it = hosts_.erase(it);
            changed = true;
        }
    }
    if (changed)
        std::sort(hosts_.begin(), hosts_.end(),
            [](const auto& a, const auto& b) { return a->name < b->name; });
    return changed;
}

std::shared_ptr<const Host> HostSet::match(const std::string& script_path) const
{
    std::shared_ptr<const Host> best;
    long best_score = 0;
    for (const auto& h : hosts_) {  // sorted: the first host wins a tie
        long score = 0;
        for (const std::string& s : h->scripts)
            if (s == script_path) score = kExactScore;
        if (score == 0) {
            for (const std::string& d : h->script_dirs) {
                const std::string dir = strip_slash(d);
                if (contains(dir, script_path))
                    score = std::max(score, static_cast<long>(dir.size()));
            }
        }
        if (score > best_score) {
            best_score = score;
            best = h;
        }
    }
    return best;
}

}  // namespace spp::ls
