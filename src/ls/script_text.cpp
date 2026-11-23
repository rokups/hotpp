// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "script_text.h"

#include "long_path.h"
#include "semantic.h"

#include <algorithm>
#include <cstring>
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

bool class_facts_hold(const Text& analyzed, const std::string& text, const hotc::AnalyzeResult& facts)
{
    const std::string& a = analyzed.s;
    const std::string& b = text;
    if (a == b)
        return true;
    size_t p = 0;
    const size_t n = std::min(a.size(), b.size());
    while (p < n && a[p] == b[p])
        ++p;
    size_t q = 0;
    while (q < n - p && a[a.size() - 1 - q] == b[b.size() - 1 - q])
        ++q;
    const size_t old_end = a.size() - q, new_end = b.size() - q;
    // The changed bytes of both texts are plain.
    const auto plain = [](char c) { return is_id_char(c) || std::strchr(" \t.,()[]=+-<>!?&|^%:", c) != nullptr; };
    for (size_t i = p; i < old_end; ++i)
        if (!plain(a[i]) || a[i] == '\0')
            return false;
    for (size_t i = p; i < new_end; ++i)
        if (!plain(b[i]) || b[i] == '\0')
            return false;
    // A byte next to the change can join with it to a comment sign, a quote or an escape.
    const auto sign = [](char c) { return c != '\0' && std::strchr("/*\"'\\#", c) != nullptr; };
    if ((p > 0 && sign(a[p - 1])) || (old_end < a.size() && sign(a[old_end])))
        return false;
    // The text without strings and comments, to match the braces. A byte of it is a space or the byte of "a".
    std::string code(a.size(), ' ');
    bool block = false;
    for (size_t l = 0; l < analyzed.starts.size(); ++l)
    {
        const std::string blank = blank_non_code(analyzed.line(static_cast<int>(l)), block);
        std::copy(blank.begin(), blank.end(), code.begin() + static_cast<long>(analyzed.starts[l]));
    }
    for (const hotc::AnalyzedClass& c : facts.classes)
        for (const hotc::AnalyzedMember& m : c.members)
        {
            if (m.kind != "method" && m.kind != "constructor" && m.kind != "destructor")
                continue;
            if (m.begin.line < 1 || m.end.line < 1)
                continue;
            const size_t mb = analyzed.offset(m.begin.line - 1, m.begin.col - 1);
            const size_t me = analyzed.offset(m.end.line - 1, m.end.col - 1);
            // The member ends after the closing brace of the body. The open brace of the body matches it.
            if (me <= mb || me > a.size() || code[me - 1] != '}' || old_end > me - 1)
                continue;
            int depth = 0;
            size_t open = std::string::npos;
            for (size_t i = me; i-- > mb;)
            {
                if (code[i] == '}')
                    ++depth;
                else if (code[i] == '{' && --depth == 0)
                {
                    open = i;
                    break;
                }
            }
            if (open != std::string::npos && p > open)
                return true;
        }
    return false;
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
    const std::string dir = path_utf8(utf8_path(path).parent_path());
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
        for (const hotc::AnalyzedMember& m : c.members)
            ic.members.push_back({m.name, m.kind, {m.begin.line - 1, m.begin.col - 1}});
        shard.classes.push_back(std::move(ic));
    }
    return shard;
}

} // namespace hot::ls
