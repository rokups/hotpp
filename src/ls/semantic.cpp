// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "semantic.h"

#include "builtin_types.h"

#include <algorithm>
#include <map>
#include <set>

namespace hot::ls
{

namespace
{

enum Type
{
    Keyword,
    TypeName,
    ClassName,
    Method,
    Property,
    Number,
    String,
    Comment
};

// The keywords of the script. The list follows the TextMate grammar in editors/vscode.
const std::set<std::string>& keywords()
{
    static const std::set<std::string> k = {"if", "else", "while", "do", "for", "foreach", "in", "return", "break",
        "continue", "switch", "case", "default", "is", "as", "new", "delete", "public", "private", "protected",
        "override", "virtual", "const", "get", "set", "this", "base", "true", "false", "null", "try", "catch",
        "finally", "throw", "var", "void", "class", "struct", "enum", "using", "native", "handle", "value",
        "intrusive", "default_handle", "cpp_prelude", "self", "alias", "weak"};
    return k;
}

} // namespace

const std::vector<std::string>& semantic_token_types()
{
    static const std::vector<std::string> t = {
        "keyword", "type", "class", "method", "property", "number", "string", "comment"};
    return t;
}

std::vector<size_t> line_starts(const std::string& text)
{
    std::vector<size_t> r = {0};
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
            r.push_back(i + 1);
    return r;
}

std::string_view line_text(const std::string& text, const std::vector<size_t>& starts, int line)
{
    if (line < 0 || static_cast<size_t>(line) >= starts.size())
        return {};
    const size_t b = starts[static_cast<size_t>(line)];
    size_t e = static_cast<size_t>(line) + 1 < starts.size() ? starts[static_cast<size_t>(line) + 1] - 1 : text.size();
    if (e > b && text[e - 1] == '\r')
        --e;
    return std::string_view(text).substr(b, e - b);
}

int utf16_to_byte(std::string_view line, int col16)
{
    int units = 0;
    size_t i = 0;
    while (i < line.size() && units < col16)
    {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += n == 4 ? 2 : 1;
        i += n;
    }
    if (units < col16)
        return static_cast<int>(line.size()) + (col16 - units); // past the end: count bytes
    return static_cast<int>(i);
}

int byte_to_utf16(std::string_view line, int col)
{
    int units = 0;
    size_t i = 0;
    const size_t end = static_cast<size_t>(std::max(col, 0));
    while (i < line.size() && i < end)
    {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += n == 4 ? 2 : 1;
        i += n;
    }
    if (end > line.size())
        units += static_cast<int>(end - line.size());
    return units;
}

namespace
{

bool is_helper(const std::string& w)
{
    return w == "enumerate" || w == "range";
}

// True when the next character after the byte e that is not a space is "(".
bool next_char_is_paren(const std::string& text, size_t e)
{
    while (e < text.size() && (text[e] == ' ' || text[e] == '\t'))
        ++e;
    return e < text.size() && text[e] == '(';
}

} // namespace

std::vector<SemanticToken> script_tokens(const std::string& text, const hotc::AnalyzeResult& facts)
{
    std::set<std::string> classes, methods, properties;
    for (const hotc::AnalyzedClass& c : facts.classes)
    {
        classes.insert(c.name);
        if (!c.base.empty())
            classes.insert(c.base);
        for (const hotc::AnalyzedMember& m : c.members)
        {
            if (m.kind == "method")
                methods.insert(m.name);
            else if (m.kind == "field" || m.kind == "property")
                properties.insert(m.name);
        }
    }
    std::vector<SemanticToken> out;
    int line = 0;
    size_t line_begin = 0;
    // Adds a token from byte b to byte e. A token that holds a line break is split.
    auto add = [&](size_t b, size_t e, int type)
    {
        size_t s = b;
        for (size_t i = b; i < e; ++i)
        {
            if (text[i] != '\n')
                continue;
            if (i > s)
                out.push_back({line, static_cast<int>(s - line_begin), static_cast<int>(i - s), type});
            ++line;
            line_begin = i + 1;
            s = i + 1;
        }
        if (e > s)
            out.push_back({line, static_cast<int>(s - line_begin), static_cast<int>(e - s), type});
    };
    size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (c == '\n')
        {
            ++line;
            line_begin = ++i;
        }
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            size_t e = text.find('\n', i);
            if (e == std::string::npos)
                e = text.size();
            add(i, e, Comment);
            i = e;
        }
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
            size_t e = text.find("*/", i + 2);
            e = e == std::string::npos ? text.size() : e + 2;
            add(i, e, Comment);
            i = e;
        }
        else if (c == '"' || c == '\'')
        {
            size_t e = i + 1;
            while (e < text.size() && text[e] != c && text[e] != '\n')
                e += text[e] == '\\' ? 2 : 1;
            e = std::min(e + 1, text.size());
            add(i, e, String);
            i = e;
        }
        else if (c >= '0' && c <= '9')
        {
            size_t e = i;
            while (e < text.size() && (is_id_char(text[e]) || text[e] == '.'))
                ++e;
            add(i, e, Number);
            i = e;
        }
        else if (is_id_char(c))
        {
            size_t e = i;
            while (e < text.size() && is_id_char(text[e]))
                ++e;
            const std::string w = text.substr(i, e - i);
            int type = -1;
            if (keywords().count(w))
                type = Keyword;
            else if (hotc::builtin_cpp_name(w))
                type = TypeName;
            else if (classes.count(w))
                type = ClassName;
            else if (methods.count(w))
                type = Method;
            else if (properties.count(w))
                type = Property;
            else if (is_helper(w) && (i == 0 || text[i - 1] != '.') && next_char_is_paren(text, e))
                type = Method; // enumerate(...) and range(...)
            if (type >= 0)
                add(i, e, type);
            i = e;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

const std::vector<std::string>& script_keywords()
{
    static const std::vector<std::string> k(keywords().begin(), keywords().end());
    return k;
}

const std::vector<std::string>& script_helpers()
{
    static const std::vector<std::string> h = {"enumerate", "range"};
    return h;
}

const std::vector<std::string>& builtin_type_names()
{
    static const std::vector<std::string> n = []
    {
        std::vector<std::string> r;
        for (const hotc::BuiltinType& b : hotc::builtin_types())
        {
            r.push_back(b.script);
            if (b.alias)
                r.push_back(b.alias);
        }
        return r;
    }();
    return n;
}

namespace
{

struct Tok
{
    std::string s;
    size_t off = 0;
    bool id = false;
};

// The identifiers and the punctuation of a script. Comments, strings and numbers are left out.
std::vector<Tok> tokenize(const std::string& text)
{
    std::vector<Tok> out;
    size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
        }
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
            const size_t e = text.find("*/", i + 2);
            i = e == std::string::npos ? text.size() : e + 2;
        }
        else if (c == '"' || c == '\'')
        {
            size_t e = i + 1;
            while (e < text.size() && text[e] != c && text[e] != '\n')
                e += text[e] == '\\' ? 2 : 1;
            i = std::min(e + 1, text.size());
        }
        else if (c >= '0' && c <= '9')
        {
            while (i < text.size() && (is_id_char(text[i]) || text[i] == '.'))
                ++i;
        }
        else if (is_id_char(c))
        {
            size_t e = i;
            while (e < text.size() && is_id_char(text[e]))
                ++e;
            out.push_back({text.substr(i, e - i), i, true});
            i = e;
        }
        else
        {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                out.push_back({std::string(1, c), i, false});
            ++i;
        }
    }
    return out;
}

enum class Block
{
    Type,
    Enum,
    Body
};

struct Scope
{
    Block kind = Block::Body;
    std::string enum_name;
    std::vector<ScopeName> locals;
};

} // namespace

ScriptScope scan_scope(const std::string& text, size_t at, size_t typed_begin)
{
    ScriptScope r;
    const std::vector<Tok> toks = tokenize(text);
    const std::set<std::string>& kw = keywords();
    auto is_name = [&](const Tok& t) { return t.id && !kw.count(t.s); };
    // A word that can stand for a type in a declaration.
    auto is_type = [&](const Tok& t) { return t.id && (!kw.count(t.s) || t.s == "var"); };
    auto add_unique = [](std::vector<std::string>& v, const std::string& n)
    {
        if (std::find(v.begin(), v.end(), n) == v.end())
            v.push_back(n);
    };

    std::vector<Scope> stack;
    std::vector<ScopeName> params;
    std::string pending; // "class", "struct" or "enum" after the keyword, until "{" or ";"
    std::string pending_name;
    int par = 0;
    std::vector<ScopeName> snapshot;
    bool snapped = false;
    auto snap = [&]
    {
        snapped = true;
        std::vector<ScopeName> all;
        for (const Scope& sc : stack)
            if (sc.kind == Block::Body)
                all.insert(all.end(), sc.locals.begin(), sc.locals.end());
        // Parameters of a function header that the cursor is in.
        all.insert(all.end(), params.begin(), params.end());
        for (const ScopeName& n : all)
        {
            snapshot.erase(
                std::remove_if(snapshot.begin(), snapshot.end(), [&](const ScopeName& o) { return o.name == n.name; }),
                snapshot.end());
            snapshot.push_back(n);
        }
    };

    for (size_t i = 0; i < toks.size(); ++i)
    {
        const Tok& t = toks[i];
        if (!snapped && t.off >= at)
            snap();
        if (t.id && t.off == typed_begin)
            continue; // the word that is typed
        const Tok* prev = i > 0 ? &toks[i - 1] : nullptr;
        const Tok* next = i + 1 < toks.size() ? &toks[i + 1] : nullptr;
        if (prev && prev->id && prev->off == typed_begin)
            prev = nullptr;
        if (!t.id)
        {
            if (t.s == "(")
                ++par;
            else if (t.s == ")")
                par = std::max(0, par - 1);
            else if (t.s == ";")
            {
                params.clear();
                pending.clear();
            }
            else if (t.s == "{")
            {
                Scope sc;
                if (pending == "enum")
                {
                    sc.kind = Block::Enum;
                    sc.enum_name = pending_name;
                }
                else if (pending == "class" || pending == "struct")
                {
                    sc.kind = Block::Type;
                }
                else
                {
                    sc.locals = params;
                }
                pending.clear();
                params.clear();
                par = 0;
                stack.push_back(std::move(sc));
            }
            else if (t.s == "}")
            {
                if (!stack.empty())
                    stack.pop_back();
                params.clear();
            }
            continue;
        }
        if (t.s == "class" || t.s == "struct" || t.s == "enum")
        {
            if (next && is_name(*next))
            {
                pending = t.s;
                pending_name = next->s;
                if (t.s == "enum")
                {
                    add_unique(r.enums, next->s);
                    r.enum_members[next->s];
                }
                else
                {
                    add_unique(t.s == "class" ? r.classes : r.structs, next->s);
                }
            }
            continue;
        }
        const Block here = stack.empty() ? Block::Type : stack.back().kind;
        const bool at_module = stack.empty();
        if (!at_module && here == Block::Enum)
        {
            if (is_name(t) && prev && (prev->s == "{" || prev->s == ","))
                r.enum_members[stack.back().enum_name].push_back(t.s);
            continue;
        }
        if (!at_module && here == Block::Body && t.s == "var" && next)
        {
            // A structured binding: "var (a, b)" or "var& (a, b)". Each name is a local.
            size_t o = i + 1;
            if (toks[o].s == "&" && o + 1 < toks.size())
                ++o;
            if (toks[o].s == "(")
            {
                size_t close = o + 1;
                while (close < toks.size() && toks[close].s != ")" && toks[close].s != ";" && toks[close].s != "{")
                    ++close;
                if (close + 1 < toks.size() && toks[close].s == ")"
                    && (toks[close + 1].s == "=" || (toks[close + 1].id && toks[close + 1].s == "in")))
                {
                    for (size_t k = o + 1; k < close; ++k)
                        if (is_name(toks[k]) && toks[k].off != typed_begin)
                            stack.back().locals.push_back({toks[k].s, std::string(), "local"});
                    i = close;
                    continue;
                }
            }
        }
        if (!at_module && here == Block::Body && t.id && t.s == "is")
        {
            // A binding of a type test: "x is T name". The type can have dots and template arguments.
            size_t k = i + 1;
            std::string type;
            while (k < toks.size() && is_name(toks[k]) && toks[k].off != typed_begin)
            {
                type += toks[k].s;
                if (k + 2 < toks.size() && toks[k + 1].s == "." && toks[k + 2].id)
                {
                    type += ".";
                    k += 2;
                    continue;
                }
                ++k;
                break;
            }
            if (!type.empty())
            {
                if (k < toks.size() && toks[k].s == "<")
                {
                    int depth = 0;
                    for (; k < toks.size(); ++k)
                    {
                        depth += toks[k].s == "<" ? 1 : toks[k].s == ">" ? -1 : 0;
                        if (depth == 0 || toks[k].s == ";" || toks[k].s == "{")
                            break;
                    }
                    if (k < toks.size() && depth == 0)
                        ++k;
                }
                while (
                    k < toks.size() && (toks[k].s == "*" || toks[k].s == "&") && k + 1 < toks.size() && toks[k + 1].id)
                    ++k;
                if (k < toks.size() && is_name(toks[k]) && toks[k].off != typed_begin)
                    stack.back().locals.push_back({toks[k].s, type, "local"});
            }
            continue;
        }
        if (!is_name(t) || !prev || !next)
            continue;
        // A declaration: "Type name" followed by "=", ";", "," or ")", or "name in".
        // "var& name" is a declaration too: the "&" follows "var".
        const bool var_ref = prev->s == "&" && i > 1 && toks[i - 2].id && toks[i - 2].s == "var";
        const bool by_type =
            is_type(*prev) || var_ref || ((prev->s == ">" || prev->s == "]") && (next->s == "=" || next->s == ";"));
        const bool foreach_name = prev->s == "(" && next->id && next->s == "in";
        const bool ends =
            next->s == "=" || next->s == ";" || next->s == "," || next->s == ")" || (next->id && next->s == "in");
        if (!((by_type && ends) || foreach_name))
            continue;
        std::string type = by_type && prev->id ? prev->s : std::string();
        if (type == "var")
            type.clear();
        if (at_module && par == 0)
        {
            r.module_vars.push_back({t.s, type, "module variable"});
        }
        else if (here == Block::Body)
        {
            stack.back().locals.push_back({t.s, type, "local"});
        }
        else if (par > 0)
        {
            params.push_back({t.s, type, "parameter"});
        }
    }
    if (!snapped)
        snap();
    r.locals = std::move(snapshot);
    return r;
}

} // namespace hot::ls
