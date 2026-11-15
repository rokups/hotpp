// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "workspace_index.h"

#include "script_text.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace hot::ls
{

void WorkspaceIndex::add_locked(const std::string& path, const Entry& e)
{
    for (const std::string& w : e.words)
        by_word_[w].insert(path);
    for (const std::string& w : e.unclaimed_words)
        by_unclaimed_[w].insert(path);
    if (e.table)
        for (const hotc::ResolvedRef& r : e.table->refs)
            if (!r.id.empty())
                by_ref_[r.id].insert(path);
}

void WorkspaceIndex::remove_locked(const std::string& path, const Entry& e)
{
    const auto drop = [&](FileMap& m, const std::string& key)
    {
        const auto it = m.find(key);
        if (it == m.end())
            return;
        it->second.erase(path);
        if (it->second.empty())
            m.erase(it);
    };
    for (const std::string& w : e.words)
        drop(by_word_, w);
    for (const std::string& w : e.unclaimed_words)
        drop(by_unclaimed_, w);
    if (e.table)
        for (const hotc::ResolvedRef& r : e.table->refs)
            if (!r.id.empty())
                drop(by_ref_, r.id);
}

void WorkspaceIndex::set(const std::string& path, std::shared_ptr<const Entry> entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(path);
    if (it != entries_.end())
    {
        remove_locked(path, *it->second);
        entries_.erase(it);
    }
    if (!entry)
        return;
    add_locked(path, *entry);
    entries_[path] = std::move(entry);
}

void WorkspaceIndex::erase(const std::string& path) { set(path, nullptr); }

std::shared_ptr<const Entry> WorkspaceIndex::entry(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : it->second;
}

std::vector<std::string> WorkspaceIndex::lookup(const FileMap& m, const std::string& key)
{
    const auto it = m.find(key);
    if (it == m.end())
        return {};
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

std::vector<std::string> WorkspaceIndex::files_with_word(const std::string& word) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(by_word_, word);
}

std::vector<std::string> WorkspaceIndex::files_with_unclaimed_word(const std::string& word) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(by_unclaimed_, word);
}

std::vector<std::string> WorkspaceIndex::files_with_ref(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(by_ref_, id);
}

std::vector<std::shared_ptr<const Entry>> WorkspaceIndex::all_entries() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<const Entry>> out;
    out.reserve(entries_.size());
    for (const auto& [path, e] : entries_)
        out.push_back(e);
    return out;
}

bool stat_file(const std::string& path, FileStamp& out)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec)
        return false;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
        return false;
    out.size = static_cast<uint64_t>(size);
    out.mtime = static_cast<int64_t>(time.time_since_epoch().count());
    return true;
}

bool fresh(const Entry& e)
{
    FileStamp s;
    if (!stat_file(e.path, s) || s != e.stamp)
        return false;
    for (const auto& [file, stamp] : e.dep_stamps)
        if (!stat_file(file, s) || s != stamp)
            return false;
    return true;
}

namespace
{
// The files of the import closure of the script, each one time, in the order of the search.
std::vector<std::string> import_closure(
    const hotc::TranslateOptions& options, const std::string& path, const std::string& text)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    seen.insert(fs::path(path).lexically_normal().string());
    struct Todo
    {
        std::string path, text;
    };
    std::vector<Todo> todo{{path, text}};
    for (size_t i = 0; i < todo.size(); ++i)
    {
        const std::string dir = fs::path(todo[i].path).parent_path().string();
        for (const std::string& pth : hotc::analyze_imports(todo[i].text, std::string()).imports)
        {
            if (pth.size() <= 4 || pth.compare(pth.size() - 4, 4, ".hot") != 0)
                continue;
            std::string itext, file, unit;
            if (!hotc::find_import(options, pth, dir, std::string(), itext, file, unit))
                continue;
            if (!seen.insert(fs::path(file).lexically_normal().string()).second)
                continue;
            out.push_back(file);
            todo.push_back({file, std::move(itext)});
        }
    }
    return out;
}
} // namespace

std::shared_ptr<const Entry> build_entry(const std::string& path, const std::string& text,
    const hotc::TranslateOptions& options, const std::string& digest, SymbolIndex& index,
    hotc::TranslatorSession& session)
{
    auto e = std::make_shared<Entry>();
    e->path = path;
    e->digest = digest;
    e->text = text;
    stat_file(path, e->stamp);
    for (const std::string& file : import_closure(options, path, text))
    {
        FileStamp s;
        stat_file(file, s); // a missing file has the zero stamp, so fresh() gives false
        e->dep_stamps.emplace_back(file, s);
    }
    auto shard = index.load(path, digest);
    if (!shard)
    {
        const hotc::AnalyzeResult facts = hotc::analyze(text, options, session);
        shard = shard_of_facts(path, Text(text), facts, script_imports_in(options, path, text));
        index.store(path, digest, *shard);
    }
    e->table = std::make_shared<const FileRefs>(std::move(shard->table));
    e->classes = std::move(shard->classes);
    collect_words(*e->table, text, e->words, e->unclaimed_words);
    return e;
}

} // namespace hot::ls
