// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "script_text.h"

#include "semantic.h"

#include <algorithm>
#include <filesystem>

namespace hot::ls
{

Text::Text()
    : starts{0}
{
}

Text::Text(std::string t)
    : s(std::move(t))
    , starts(line_starts(s))
{
}

std::string_view Text::line(int l) const { return line_text(s, starts, l); }

size_t Text::offset(int l, int c) const
{
    if (l < 0)
        return 0;
    if (static_cast<size_t>(l) >= starts.size())
        return s.size();
    return std::min(starts[static_cast<size_t>(l)] + static_cast<size_t>(std::max(c, 0)), s.size());
}

std::pair<int, int> Text::pos(size_t off) const
{
    const auto it = std::upper_bound(starts.begin(), starts.end(), off);
    const int l = static_cast<int>(it - starts.begin()) - 1;
    return {l, static_cast<int>(off - starts[static_cast<size_t>(l)])};
}

// Replaces the text of the string literals, the character literals and the comments of a line by
// spaces. "in_block" is true inside a block comment that goes on to the next line.
std::string blank_non_code(std::string_view l, bool& in_block)
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
        {
            // Skip the "/*" before the search for "*/": "/*/" does not end the comment.
            in_block = true;
            out[i] = out[i + 1] = ' ';
            i += 2;
        }
        // A raw string and a digit separator are errors in a script (doc/limits.md), so a quote
        // here always starts an ordinary string or character literal.
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

// The columns of the whole-word matches of "w" in a line.
std::vector<int> whole_word_cols(std::string_view l, const std::string& w)
{
    std::vector<int> cols;
    for (size_t from = 0, at; (at = l.find(w, from)) != std::string_view::npos; from = at + 1)
        if ((at == 0 || !is_id_char(l[at - 1])) && (at + w.size() >= l.size() || !is_id_char(l[at + w.size()])))
            cols.push_back(static_cast<int>(at));
    return cols;
}

Pos class_name_pos_in(const Text& t, const hotc::AnalyzedClass& c)
{
    // The begin of a class is at the keyword "class". The name comes after it on the line.
    const Pos b = {c.begin.line - 1, c.begin.col - 1};
    const std::string_view l = t.line(b.line);
    for (size_t at = l.find(c.name, static_cast<size_t>(b.col)); at != std::string_view::npos;
        at = l.find(c.name, at + 1))
        if ((at == 0 || !is_id_char(l[at - 1]))
            && (at + c.name.size() >= l.size() || !is_id_char(l[at + c.name.size()])))
            return {b.line, static_cast<int>(at)};
    return b;
}

std::set<Pos> class_places_in(const Text& t, const hotc::AnalyzeResult& facts, const std::string& name)
{
    // The class name has no segment in the source map, so clangd cannot find it. The semantic
    // tokens of the facts mark each word that is the name of a script class.
    constexpr int kClassToken = 2; // the index of "class" in semantic_token_types()
    std::set<Pos> out;
    for (const SemanticToken& tok : script_tokens(t.s, facts))
        if (tok.type == kClassToken
            && t.line(tok.line).substr(static_cast<size_t>(tok.col), static_cast<size_t>(tok.len)) == name)
            out.insert({tok.line, tok.col});
    return out;
}

bool table_lacks_word_in(const FileRefs& t, const std::string& text, const std::string& word)
{
    const Text body(text);
    bool block = false;
    for (int line = 0; line < static_cast<int>(body.starts.size()); ++line)
        for (const int col : whole_word_cols(blank_non_code(body.line(line), block), word))
        {
            const hotc::ResolvedRef* r = ref_at(t, static_cast<uint32_t>(body.offset(line, col)));
            if (!r || r->length != word.size() || body.offset(line, col) != static_cast<size_t>(r->offset))
                return false;
        }
    return true;
}

void collect_words(const FileRefs& t, const std::string& text, std::set<std::string>& words,
    std::set<std::string>& unclaimed)
{
    const Text body(text);
    bool block = false;
    for (int line = 0; line < static_cast<int>(body.starts.size()); ++line)
    {
        const std::string_view raw = body.line(line);
        const std::string code = blank_non_code(raw, block);
        for (size_t i = 0; i < raw.size();)
        {
            if (!is_id_char(raw[i]))
            {
                ++i;
                continue;
            }
            size_t e = i;
            while (e < raw.size() && is_id_char(raw[e]))
                ++e;
            words.emplace(raw.substr(i, e - i));
            // A code word is a run of id chars that the blanking keeps whole.
            if (code.compare(i, e - i, raw, i, e - i) == 0)
            {
                const hotc::ResolvedRef* r = ref_at(t, static_cast<uint32_t>(body.offset(line, static_cast<int>(i))));
                if (!r || r->length != e - i || body.offset(line, static_cast<int>(i)) != static_cast<size_t>(r->offset))
                    unclaimed.emplace(raw.substr(i, e - i));
            }
            i = e;
        }
    }
}

std::vector<std::string> script_imports_in(
    const hotc::TranslateOptions& options, const std::string& path, const std::string& text)
{
    const std::string dir = std::filesystem::path(path).parent_path().string();
    std::vector<std::string> out;
    for (const std::string& pth : hotc::analyze_imports(text, std::string()).imports)
    {
        if (pth.size() <= 4 || pth.compare(pth.size() - 4, 4, ".hot") != 0)
            continue;
        std::string itext, file, unit;
        if (!hotc::find_import(options, pth, dir, std::string(), itext, file, unit))
            continue;
        const std::string id = hotc::ref_file_id(file);
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    }
    return out;
}

FileRefs table_of_facts(const std::string& path, const hotc::AnalyzeResult& facts, std::vector<std::string> imports)
{
    FileRefs t;
    t.file_id = hotc::ref_file_id(path);
    t.refs = facts.refs;
    t.unresolved = facts.unresolved;
    t.raw_ranges = facts.raw_ranges;
    t.imports = std::move(imports);
    for (const hotc::AnalyzeError& e : facts.errors)
        if (!e.note)
            t.errors.emplace_back(e.pos.line, e.message);
    // A script that does not translate has no table, or a table with gaps. Line 0: no line.
    if (t.errors.empty() && !facts.ok)
        t.errors.emplace_back(0, facts.diagnostics.substr(0, facts.diagnostics.find('\n')));
    for (const hotc::AnalyzedClass& c : facts.classes)
        t.bases.emplace_back(c.name, c.base);
    sort_refs(t);
    return t;
}

IndexShard shard_of_facts(
    const std::string& path, const Text& text, const hotc::AnalyzeResult& facts, std::vector<std::string> imports)
{
    IndexShard shard;
    shard.table = table_of_facts(path, facts, std::move(imports));
    shard.has_table = true;
    for (const hotc::AnalyzedClass& c : facts.classes)
    {
        IndexClass ic;
        ic.name = c.name;
        const Pos b = class_name_pos_in(text, c);
        ic.begin = {b.line, b.col};
        for (const Pos& q : class_places_in(text, facts, c.name))
            ic.refs.push_back({q.line, q.col});
        for (const hotc::AnalyzedMember& m : c.members)
            ic.members.push_back({m.name, m.kind, {m.begin.line - 1, m.begin.col - 1}, std::nullopt});
        shard.classes.push_back(std::move(ic));
    }
    return shard;
}

} // namespace hot::ls
