// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Mask of the script blocks in a markup document (for example an RmlUi `.rml` file).
// The function keeps the code of the blocks and removes all other text.
// Line and column positions of the code stay the same.
// The header needs no engine and no exceptions. The run time is linear in the text size.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace hot
{

namespace embedded_script_detail
{

inline char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// True when the text at "pos" starts with "word". The compare ignores the ASCII case.
inline bool starts_nocase(std::string_view t, size_t pos, std::string_view word)
{
    if (pos > t.size() || t.size() - pos < word.size())
        return false;
    for (size_t i = 0; i < word.size(); ++i)
        if (lower(t[pos + i]) != lower(word[i]))
            return false;
    return true;
}

// The text at "pos" is "</", spaces, "name", spaces, ">" (any case). Returns the position after
// the ">". Returns npos for no match.
inline size_t match_close(std::string_view t, size_t pos, std::string_view name)
{
    if (t.compare(pos, 2, "</") != 0)
        return std::string_view::npos;
    size_t i = pos + 2;
    while (i < t.size() && is_space(t[i]))
        ++i;
    if (!starts_nocase(t, i, name))
        return std::string_view::npos;
    i += name.size();
    while (i < t.size() && is_space(t[i]))
        ++i;
    if (i < t.size() && t[i] == '>')
        return i + 1;
    return std::string_view::npos;
}

// Skips a tag that starts at "pos" ("<"). A quoted value can hold a ">". Returns the position
// after the ">". Without a ">" it returns the size of the text and sets "closed" to false.
inline size_t skip_tag(std::string_view t, size_t pos, bool& self_closing, bool& closed)
{
    char quote = 0;
    self_closing = false;
    closed = true;
    for (size_t i = pos + 1; i < t.size(); ++i)
    {
        const char c = t[i];
        if (quote != 0)
        {
            if (c == quote)
                quote = 0;
        }
        else if (c == '"' || c == '\'')
            quote = c;
        else if (c == '>')
        {
            self_closing = t[i - 1] == '/';
            return i + 1;
        }
    }
    closed = false;
    return t.size();
}

} // namespace embedded_script_detail

// Returns the code of the `tag` blocks of `text`. Rules:
//  1. The scan stops at the first `</head>`. A comment `<!-- -->`, a CDATA section, a processing
//     instruction and the attributes of other tags hold no block.
//  2. A block starts at `<tag` (any case) and ends at `</`, spaces, `tag`, spaces, `>`.
//     A block without an end runs to the end of the text.
//  3. The code of a block is copied as is. Other text keeps only the line ends (CR and LF).
//     The first line of a block gets spaces up to the column of the code in `text`.
//     The columns count bytes.
//  4. The result ends with the last block. It has no text from after the last end tag.
//  5. `prefix` goes in front of line 1 and has no line end. Only the columns of the code on
//     line 1 change.
// A text without a block gives an empty string, also with a prefix.
// `found` (if not null) is true when the text has a block. A block with `src` and no code counts.
inline std::string mask_embedded_scripts(std::string_view text, std::string_view tag = "script",
    bool* found = nullptr, std::string_view prefix = {})
{
    using namespace embedded_script_detail;
    constexpr size_t npos = std::string_view::npos;
    const size_t n = text.size();

    std::string out;
    std::string pending;    // line ends of the text outside the blocks, not yet written
    size_t out_col = 0;     // length of the last output line (without the prefix)
    size_t line_start = 0;  // start of the line of "scanned" in the text
    size_t scanned = 0;
    size_t outside_from = 0;
    bool any = false;

    auto track_lines = [&](size_t upto) {
        for (; scanned < upto; ++scanned)
            if (text[scanned] == '\n')
                line_start = scanned + 1;
    };

    size_t pos = 0;
    while (pos < n)
    {
        pos = text.find('<', pos);
        if (pos == npos)
            break;
        if (text.compare(pos, 4, "<!--") == 0)
        {
            const size_t e = text.find("-->", pos + 4);
            pos = e == npos ? n : e + 3;
            continue;
        }
        if (text.compare(pos, 9, "<![CDATA[") == 0)
        {
            const size_t e = text.find("]]>", pos + 9);
            pos = e == npos ? n : e + 3;
            continue;
        }
        if (pos + 1 >= n)
            break;
        const char next = text[pos + 1];
        if (next == '/')
        {
            if (match_close(text, pos, "head") != npos)
                break;
            pos += 2;
            continue;
        }
        const bool letter = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z');
        if (!letter && next != '!' && next != '?')
        {
            ++pos;
            continue;
        }
        bool self_closing = false;
        bool closed = true;
        const size_t after = skip_tag(text, pos, self_closing, closed);
        const bool is_block = letter && starts_nocase(text, pos + 1, tag) && pos + 1 + tag.size() < n
            && (is_space(text[pos + 1 + tag.size()]) || text[pos + 1 + tag.size()] == '>'
                || text[pos + 1 + tag.size()] == '/');
        if (!is_block)
        {
            pos = after;
            continue;
        }
        if (!closed)
            break;
        any = true;
        if (self_closing)
        {
            pos = after;
            continue;
        }

        size_t content_end = n;
        size_t close_end = n;
        for (size_t p = after;;)
        {
            const size_t lt = text.find("</", p);
            if (lt == npos)
                break;
            const size_t e = match_close(text, lt, tag);
            if (e != npos)
            {
                content_end = lt;
                close_end = e;
                break;
            }
            p = lt + 2;
        }

        if (content_end > after)
        {
            for (size_t i = outside_from; i < after; ++i)
                if (text[i] == '\r' || text[i] == '\n')
                    pending += text[i];
            for (const char c : pending)
                out_col = c == '\n' ? 0 : out_col + 1;
            out += pending;
            pending.clear();
            track_lines(after);
            const size_t column = after - line_start;
            if (out_col < column)
            {
                out.append(column - out_col, ' ');
                out_col = column;
            }
            const std::string_view code = text.substr(after, content_end - after);
            out += code;
            const size_t nl = code.rfind('\n');
            out_col = nl == npos ? out_col + code.size() : code.size() - nl - 1;
            outside_from = content_end;
        }
        else
        {
            for (size_t i = outside_from; i < content_end; ++i)
                if (text[i] == '\r' || text[i] == '\n')
                    pending += text[i];
            outside_from = content_end;
        }
        pos = close_end;
    }

    if (found != nullptr)
        *found = any;
    if (!any)
        return std::string();
    std::string result;
    result.reserve(prefix.size() + out.size());
    result.append(prefix);
    result += out;
    return result;
}

} // namespace hot
