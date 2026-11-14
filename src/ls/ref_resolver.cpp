// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "ref_resolver.h"

#include <algorithm>
#include <charconv>
#include <limits>
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

std::vector<std::string> ImportGraph::imports_of(const std::string& file_id) const
{
    const auto it = imports_.find(file_id);
    return it == imports_.end() ? std::vector<std::string>() : it->second;
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

namespace
{

bool id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string_view name_at(const RenameFile& f, uint32_t offset, uint32_t length)
{
    if (offset > f.text.size() || length > f.text.size() - offset)
        return {};
    return f.text.substr(offset, length);
}

// True when the name at "offset" is not after "." or "::": a bare name that a local can hide.
bool bare_at(const RenameFile& f, uint32_t offset)
{
    size_t q = offset;
    while (q > 0 && (f.text[q - 1] == ' ' || f.text[q - 1] == '\t' || f.text[q - 1] == '\n' || f.text[q - 1] == '\r'))
        --q;
    return q == 0 || (f.text[q - 1] != '.' && f.text[q - 1] != ':');
}

int line_of(const RenameFile& f, size_t offset)
{
    return 1 + static_cast<int>(std::count(f.text.begin(), f.text.begin() + static_cast<long>(std::min(offset, f.text.size())), '\n'));
}

std::string where(const RenameFile& f, size_t offset)
{
    return f.table->file_id + ":" + std::to_string(line_of(f, offset));
}

// The function that holds "at": the range between the declarations that are not locals or
// parameters. The table has no function ranges: this range is conservative.
std::pair<uint32_t, uint32_t> fn_range(const RenameFile& f, uint32_t at)
{
    uint32_t begin = 0;
    uint32_t end = static_cast<uint32_t>(f.text.size());
    for (const hotc::ResolvedRef& r : f.table->refs)
        if (r.is_decl && !file_local(r.kind))
        {
            if (r.offset < at)
                begin = r.offset;
            else if (r.offset > at && r.offset < end)
                end = r.offset;
        }
    return {begin, end};
}

// The offset of a local or a parameter declaration "name" in [begin, end). Else npos.
size_t local_named(const RenameFile& f, uint32_t begin, uint32_t end, const std::string& name)
{
    for (const hotc::ResolvedRef& r : f.table->refs)
        if (r.is_decl && file_local(r.kind) && r.offset >= begin && r.offset < end && name_at(f, r.offset, r.length) == name)
            return r.offset;
    return std::string_view::npos;
}

// The classes of "start", their script bases and their derived classes in "files".
std::set<std::string> family_of(const std::vector<RenameFile>& files, std::set<std::string> family)
{
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const RenameFile& f : files)
            if (f.table)
                for (const auto& [cls, base] : f.table->bases)
                {
                    const bool has_cls = family.count(cls) != 0, has_base = family.count(base) != 0;
                    if (has_cls != has_base && !base.empty())
                    {
                        family.insert(cls);
                        family.insert(base);
                        grew = true;
                    }
                }
    }
    return family;
}

// The offset of a member declaration "name" of a class in "family". Else npos. "file" gets the file.
size_t member_named(const std::vector<RenameFile>& files, const std::set<std::string>& family, const std::string& name,
    const std::string& skip_id, const RenameFile*& file)
{
    for (const RenameFile& f : files)
        if (f.table)
            for (const hotc::ResolvedRef& r : f.table->refs)
            {
                if (!r.is_decl || r.id == skip_id)
                    continue;
                const size_t rd = r.decl.rfind('.');
                if (rd == std::string::npos || r.decl.compare(rd + 1, std::string::npos, name) != 0)
                    continue;
                if (family.count(r.decl.substr(0, rd))
                    || (!r.near_decl.empty() && family.count(r.near_decl.substr(0, r.near_decl.rfind('.')))))
                {
                    file = &f;
                    return r.offset;
                }
            }
    return std::string_view::npos;
}

// The offset of a bare word "name" in "f" that is not the symbol "skip_id" and not a local or a
// parameter: a ref, an unresolved name or a word in a raw body. Else npos.
size_t bare_word(const RenameFile& f, const std::string& name, const std::string& skip_id)
{
    for (const hotc::ResolvedRef& r : f.table->refs)
        if (!r.is_decl && r.id != skip_id && !file_local(r.kind) && name_at(f, r.offset, r.length) == name
            && bare_at(f, r.offset))
            return r.offset;
    for (const hotc::UnresolvedName& u : f.table->unresolved)
        if (name_at(f, u.offset, u.length) == name && bare_at(f, u.offset))
            return u.offset;
    for (const auto& [b, e] : f.table->raw_ranges)
        for (size_t at = f.text.find(name, b); at != std::string_view::npos && at < e; at = f.text.find(name, at + 1))
            if ((at == 0 || !id_char(f.text[at - 1]))
                && (at + name.size() >= f.text.size() || !id_char(f.text[at + name.size()])))
                return at;
    return std::string_view::npos;
}

} // namespace

std::string rename_conflict(
    const std::vector<RenameFile>& files, const hotc::ResolvedRef& target, const std::string& new_name)
{
    const std::string declared = "cannot rename to " + new_name + ": the name is declared at ";
    if (file_local(target.kind))
    {
        const RenameFile* file = nullptr;
        for (const RenameFile& f : files)
            if (f.table && f.table->file_id == target.decl_file)
                file = &f;
        uint32_t decl = 0;
        const std::string& d = target.decl;
        if (!file || d.size() < 2 || d[0] != '@'
            || std::from_chars(d.data() + 1, d.data() + d.size(), decl).ec != std::errc())
            return "cannot rename to " + new_name + ": the declaration of the local is not known";
        const auto [begin, end] = fn_range(*file, decl);
        for (const hotc::ResolvedRef& r : file->table->refs)
        {
            if (r.offset < begin || r.offset >= end || r.id == target.id || name_at(*file, r.offset, r.length) != new_name)
                continue;
            if (r.is_decl && file_local(r.kind))
                return declared + where(*file, r.offset);
            if (r.offset > decl && bare_at(*file, r.offset))
                return "cannot rename to " + new_name + ": the local would hide the name at " + where(*file, r.offset);
        }
        for (const hotc::UnresolvedName& u : file->table->unresolved)
            if (u.offset > decl && u.offset < end && name_at(*file, u.offset, u.length) == new_name && bare_at(*file, u.offset))
                return "cannot rename to " + new_name + ": the local would hide the name at " + where(*file, u.offset);
        for (const auto& [b, e] : file->table->raw_ranges)
            for (size_t at = file->text.find(new_name, std::max(b, decl)); at != std::string_view::npos && at < std::min(e, end);
                at = file->text.find(new_name, at + 1))
                if ((at == 0 || !id_char(file->text[at - 1]))
                    && (at + new_name.size() >= file->text.size() || !id_char(file->text[at + new_name.size()])))
                    return "cannot rename to " + new_name + ": the local would hide the name at " + where(*file, at);
        return std::string();
    }
    const std::string hide = "cannot rename to " + new_name + ": ";
    // A bare use of the symbol in a function with a local or a parameter "new_name": the local
    // hides the renamed symbol.
    for (const RenameFile& f : files)
        if (f.table)
            for (const hotc::ResolvedRef& r : f.table->refs)
                if (r.id == target.id && !r.is_decl && bare_at(f, r.offset))
                {
                    const auto [b, e] = fn_range(f, r.offset);
                    if (const size_t l = local_named(f, b, e, new_name); l != std::string_view::npos)
                        return hide + "the local at " + where(f, l) + " would hide the name at " + where(f, r.offset);
                }
    const size_t dot = target.decl.rfind('.');
    if (dot == std::string::npos)
    {
        // A module symbol.
        for (const RenameFile& f : files)
            if (f.table)
                for (const hotc::ResolvedRef& r : f.table->refs)
                    if (r.is_decl && !file_local(r.kind) && r.decl == new_name && r.id != target.id)
                        return declared + where(f, r.offset);
        for (const RenameFile& f : files)
        {
            if (!f.table)
                continue;
            // A bare use in a file with classes can be in a class: a member "new_name" of the
            // family of a class of the file hides the renamed symbol.
            std::set<std::string> classes;
            for (const auto& [cls, base] : f.table->bases)
                classes.insert(cls);
            bool used = false;
            for (const hotc::ResolvedRef& r : f.table->refs)
                used = used || (r.id == target.id && !r.is_decl && bare_at(f, r.offset));
            const RenameFile* mf = nullptr;
            if (used && !classes.empty())
                if (const size_t m = member_named(files, family_of(files, classes), new_name, target.id, mf);
                    m != std::string_view::npos)
                    return hide + "the member at " + where(*mf, m) + " would hide the name in " + f.table->file_id;
            // A bare native or unresolved name "new_name": the renamed module symbol can hide it.
            for (const hotc::UnresolvedName& u : f.table->unresolved)
                if (name_at(f, u.offset, u.length) == new_name && bare_at(f, u.offset))
                    return hide + "the name would hide the name at " + where(f, u.offset);
            // A word "new_name" in a raw body can be a C++ local or a native name.
            for (const auto& [b, e] : f.table->raw_ranges)
                for (size_t at = f.text.find(new_name, b); at != std::string_view::npos && at < e;
                    at = f.text.find(new_name, at + 1))
                    if ((at == 0 || !id_char(f.text[at - 1]))
                        && (at + new_name.size() >= f.text.size() || !id_char(f.text[at + new_name.size()])))
                        return hide + "the name would hide the name at " + where(f, at);
        }
        return std::string();
    }
    // A member: the class family.
    const std::set<std::string> family = family_of(files, {target.decl.substr(0, dot)});
    const RenameFile* mf = nullptr;
    if (const size_t m = member_named(files, family, new_name, target.id, mf); m != std::string_view::npos)
        return declared + where(*mf, m);
    // A bare word "new_name" in a file of a family class can be in a method of the family: the
    // renamed member hides it.
    for (const RenameFile& f : files)
    {
        if (!f.table)
            continue;
        bool has_family = false;
        for (const auto& [cls, base] : f.table->bases)
            has_family = has_family || family.count(cls) != 0;
        if (!has_family)
            continue;
        if (const size_t w = bare_word(f, new_name, target.id); w != std::string_view::npos)
            return hide + "the member would hide the name at " + where(f, w);
    }
    return std::string();
}

} // namespace hot::ls
