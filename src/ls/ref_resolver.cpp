// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "ref_resolver.h"

#include <algorithm>
#include <tuple>

namespace hot::ls
{

using json = nlohmann::json;

void sort_refs(FileRefs& file)
{
    const auto by_offset = [](const auto& a, const auto& b) { return a.offset < b.offset; };
    if (!std::is_sorted(file.refs.begin(), file.refs.end(), by_offset))
        std::stable_sort(file.refs.begin(), file.refs.end(), by_offset);
    if (!std::is_sorted(file.unresolved.begin(), file.unresolved.end(), by_offset))
        std::stable_sort(file.unresolved.begin(), file.unresolved.end(), by_offset);
}

const hotc::ResolvedRef* ref_at(const FileRefs& file, uint32_t offset)
{
    // The last ref that starts at or before "offset".
    auto it = std::upper_bound(file.refs.begin(), file.refs.end(), offset,
        [](uint32_t at, const hotc::ResolvedRef& r) { return at < r.offset; });
    if (it == file.refs.begin())
        return nullptr;
    --it;
    return offset <= it->offset + it->length ? &*it : nullptr;
}

std::vector<const hotc::ResolvedRef*> places(const FileRefs& file, const std::string& id)
{
    std::vector<const hotc::ResolvedRef*> out;
    for (const hotc::ResolvedRef& r : file.refs)
        if (r.id == id)
            out.push_back(&r);
    return out;
}

const hotc::ResolvedRef* decl_of(const FileRefs& file, const std::string& id)
{
    for (const hotc::ResolvedRef& r : file.refs)
        if (r.is_decl && r.id == id)
            return &r;
    return nullptr;
}

bool file_local(hotc::RefKind kind)
{
    return kind == hotc::RefKind::Local || kind == hotc::RefKind::Param;
}

void ImportGraph::set(const std::string& file_id, const std::vector<std::string>& imports)
{
    erase(file_id);
    imports_[file_id] = imports;
    for (const std::string& i : imports)
        importers_[i].insert(file_id);
}

void ImportGraph::erase(const std::string& file_id)
{
    auto it = imports_.find(file_id);
    if (it == imports_.end())
        return;
    for (const std::string& i : it->second)
    {
        auto r = importers_.find(i);
        if (r == importers_.end())
            continue;
        r->second.erase(file_id);
        if (r->second.empty())
            importers_.erase(r);
    }
    imports_.erase(it);
}

std::vector<std::string> ImportGraph::importers_of(const std::string& file_id) const
{
    std::vector<std::string> out;
    std::set<std::string> seen = {file_id};
    std::vector<std::string> todo = {file_id};
    while (!todo.empty())
    {
        const std::string f = std::move(todo.back());
        todo.pop_back();
        auto it = importers_.find(f);
        if (it == importers_.end())
            continue;
        for (const std::string& i : it->second)
            if (seen.insert(i).second)
            {
                out.push_back(i);
                todo.push_back(i);
            }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace
{

std::tuple<int, int, int, int> range_key(const json& edit)
{
    const json& r = edit.at("range");
    return {r.at("start").at("line").get<int>(), r.at("start").at("character").get<int>(),
        r.at("end").at("line").get<int>(), r.at("end").at("character").get<int>()};
}

} // namespace

int merge_edits(json& changes, const std::string& uri, const json& edits)
{
    json& list = changes[uri];
    if (!list.is_array())
        list = json::array();
    for (const json& e : edits)
        list.push_back(e);
    std::vector<json> v(list.begin(), list.end());
    // The sort is stable, so the first edit of each range stays first.
    std::stable_sort(v.begin(), v.end(), [](const json& a, const json& b) { return range_key(a) < range_key(b); });
    int conflicts = 0;
    std::vector<json> out;
    for (json& e : v)
    {
        if (!out.empty() && range_key(out.back()) == range_key(e))
        {
            if (out.back().value("newText", std::string()) != e.value("newText", std::string()))
                ++conflicts;
            continue;
        }
        out.push_back(std::move(e));
    }
    list = json(std::move(out));
    return conflicts;
}

} // namespace hot::ls
