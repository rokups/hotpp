// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "semantic.h"

#include "builtin_types.h"
#include "hot/hotc_lib.h"

#include <algorithm>
#include <map>
#include <optional>
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

// The keywords of the script. The list follows the TextMate grammar in editors/vscode. The
// attribute names come from the translator, so a new attribute is a keyword too.
const std::set<std::string>& keywords()
{
    static const std::set<std::string> k = []
    {
        std::set<std::string> r = {"if", "else", "while", "do", "for", "foreach", "in", "ref", "out", "return", "break",
            "continue", "switch", "case", "default", "is", "as", "new", "delete", "public", "private", "protected",
            "override", "virtual", "abstract", "async", "await", "const", "get", "set", "this", "base", "true", "false", "null", "try", "catch",
            "finally", "throw", "var", "void", "class", "struct", "enum", "interface", "using"};
        // "std", "lib", "array" and "attribute" stay names: a script uses them as a namespace or a variable.
        for (const std::string& a : hotc::class_attribute_names())
            if (a != "std" && a != "lib" && a != "array" && a != "attribute")
                r.insert(a);
        return r;
    }();
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

// The identifiers and the punctuation of a script. Comments are left out. Strings and numbers are
// left out too, unless "literals" is set: then each one is a token with "id" not set.
std::vector<Tok> tokenize(const std::string& text, bool literals = false)
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
            const size_t end = std::min(e + 1, text.size());
            if (literals)
                out.push_back({text.substr(i, end - i), i, false});
            i = end;
        }
        else if (c >= '0' && c <= '9')
        {
            const size_t b = i;
            while (i < text.size() && (is_id_char(text[i]) || text[i] == '.'))
                ++i;
            if (literals)
                out.push_back({text.substr(b, i - b), b, false});
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
    std::string pending; // "class", "struct", "interface" or "enum" after the keyword, until "{" or ";"
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
                else if (pending == "class" || pending == "struct" || pending == "interface")
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
        if (t.s == "class" || t.s == "struct" || t.s == "enum" || t.s == "interface")
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
                else if (t.s == "interface")
                {
                    add_unique(r.interfaces, next->s);
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
        if (!at_module && here == Block::Body && (t.s == "var" || t.s == "ref") && next)
        {
            // A structured binding: "var (a, b)" or "ref (a, b)". Each name is a local.
            size_t o = i + 1;
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
            if (k < toks.size() && toks[k].s == "$")
                ++k; // "$T" is the handle of "T": the type of the binding is "T"
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
        // "ref name" is a declaration too.
        const bool var_ref = prev->id && prev->s == "ref";
        const bool by_type =
            is_type(*prev) || var_ref || ((prev->s == ">" || prev->s == "]") && (next->s == "=" || next->s == ";"));
        const bool foreach_name = prev->s == "(" && next->id && next->s == "in";
        const bool ends =
            next->s == "=" || next->s == ";" || next->s == "," || next->s == ")" || (next->id && next->s == "in");
        if (!((by_type && ends) || foreach_name))
            continue;
        std::string type = by_type && prev->id ? prev->s : std::string();
        if (type == "var" || type == "ref")
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

std::vector<NameUse> script_name_uses(const std::string& text, const hotc::AnalyzeResult& facts)
{
    std::vector<NameUse> out;
    const std::vector<Tok> toks = tokenize(text);
    const std::vector<size_t> starts = line_starts(text);
    auto offset_of = [&](const hotc::SourcePos& p) -> std::optional<size_t>
    {
        if (p.line < 1 || static_cast<size_t>(p.line) > starts.size() || p.col < 1)
            return std::nullopt;
        return starts[static_cast<size_t>(p.line - 1)] + static_cast<size_t>(p.col - 1);
    };
    auto token_at = [&](size_t off)
    {
        const auto it = std::lower_bound(toks.begin(), toks.end(), off, [](const Tok& t, size_t o) { return t.off < o; });
        return it != toks.end() && it->off == off ? static_cast<size_t>(it - toks.begin()) : toks.size();
    };
    std::set<size_t> declared; // the offsets of the names in the declarations
    for (const hotc::AnalyzedClass& c : facts.classes)
    {
        // The begin of a class is at the keyword. The name is the first token with the name after it.
        if (const auto b = offset_of(c.begin))
        {
            const size_t first = token_at(*b);
            for (size_t i = first; i < toks.size() && i < first + 4; ++i)
                if (toks[i].s == c.name)
                {
                    declared.insert(toks[i].off);
                    break;
                }
        }
        for (const hotc::AnalyzedMember& m : c.members)
            if (const auto b = offset_of(m.begin))
            {
                declared.insert(*b);
                // The begin of a destructor is at the "~". The name is the token after it.
                if (m.kind == "destructor")
                {
                    const size_t i = token_at(*b);
                    if (i + 1 < toks.size() && toks[i].s == "~")
                        declared.insert(toks[i + 1].off);
                }
            }
    }
    for (size_t i = 0; i < toks.size(); ++i)
    {
        if (!toks[i].id || keywords().count(toks[i].s) || declared.count(toks[i].off))
            continue;
        out.push_back({toks[i].off, toks[i].s, i + 1 < toks.size() && toks[i + 1].s == "("});
    }
    return out;
}

std::vector<ParamHint> script_param_hints(const std::string& text, const hotc::AnalyzeResult& facts)
{
    std::vector<ParamHint> out;
    const std::vector<Tok> toks = tokenize(text, true);
    const std::vector<size_t> starts = line_starts(text);
    auto open_of = [](const Tok& t) { return t.s == "(" || t.s == "[" || t.s == "{"; };
    auto close_of = [](const Tok& t) { return t.s == ")" || t.s == "]" || t.s == "}"; };
    // The index of the token that starts at "off", or the size when there is none.
    auto token_at = [&](size_t off)
    {
        const auto it = std::lower_bound(toks.begin(), toks.end(), off, [](const Tok& t, size_t o) { return t.off < o; });
        return it != toks.end() && it->off == off ? static_cast<size_t>(it - toks.begin()) : toks.size();
    };
    // The index of the token that closes the bracket at "open", or the size. A closing bracket of
    // an other kind, or the end of the text, gives the size.
    auto match = [&](size_t open)
    {
        std::string stack;
        for (size_t k = open; k < toks.size(); ++k)
        {
            if (open_of(toks[k]))
            {
                stack += toks[k].s[0] == '(' ? ')' : toks[k].s[0] == '[' ? ']' : '}';
            }
            else if (close_of(toks[k]))
            {
                if (stack.empty() || stack.back() != toks[k].s[0])
                    return toks.size();
                stack.pop_back();
                if (stack.empty())
                    return k;
            }
        }
        return toks.size();
    };
    // The bounds [begin, end) of the arguments between the brackets "open" and "close".
    auto split = [&](size_t open, size_t close)
    {
        std::vector<std::pair<size_t, size_t>> parts;
        if (close == open + 1)
            return parts;
        size_t begin = open + 1;
        int depth = 0;
        for (size_t k = open + 1; k < close; ++k)
        {
            if (open_of(toks[k]))
                ++depth;
            else if (close_of(toks[k]))
                --depth;
            else if (depth == 0 && toks[k].s == ",")
            {
                parts.emplace_back(begin, k);
                begin = k + 1;
            }
        }
        parts.emplace_back(begin, close);
        return parts;
    };

    // ---- the callees: the methods and the constructors of the facts
    struct Callee
    {
        std::vector<std::string> names;
        size_t required = 0;
    };
    std::map<std::string, std::vector<Callee>> callees;
    std::set<size_t> declared; // the offsets of the names in the declarations
    for (const hotc::AnalyzedClass& c : facts.classes)
        for (const hotc::AnalyzedMember& m : c.members)
        {
            if ((m.kind != "method" && m.kind != "constructor") || m.begin.line < 1
                || static_cast<size_t>(m.begin.line) > starts.size())
                continue;
            const size_t i = token_at(starts[static_cast<size_t>(m.begin.line - 1)] + static_cast<size_t>(m.begin.col - 1));
            if (i + 1 >= toks.size() || toks[i].s != m.name || toks[i + 1].s != "(")
                continue;
            declared.insert(toks[i].off);
            const size_t close = match(i + 1);
            if (close == toks.size())
                continue;
            Callee callee;
            bool has_default = false;
            for (const auto& [b, e] : split(i + 1, close))
            {
                // The name is the last word before a default value "=".
                std::string name;
                bool defaulted = false;
                int depth = 0;
                for (size_t k = b; k < e; ++k)
                {
                    if (open_of(toks[k]))
                        ++depth;
                    else if (close_of(toks[k]))
                        --depth;
                    else if (depth == 0 && toks[k].s == "=")
                    {
                        defaulted = true;
                        break;
                    }
                    else if (depth == 0 && toks[k].id)
                        name = toks[k].s;
                }
                if (!has_default && !defaulted)
                    callee.required = callee.names.size() + 1;
                has_default = has_default || defaulted;
                callee.names.push_back(name);
            }
            callees[m.name].push_back(std::move(callee));
        }
    if (callees.empty())
        return out;

    // ---- the calls
    for (size_t i = 0; i + 1 < toks.size(); ++i)
    {
        if (!toks[i].id || toks[i + 1].s != "(" || declared.count(toks[i].off))
            continue;
        const auto it = callees.find(toks[i].s);
        if (it == callees.end() || keywords().count(toks[i].s))
            continue;
        const size_t close = match(i + 1);
        if (close == toks.size())
            continue;
        const auto args = split(i + 1, close);
        std::vector<const Callee*> fit;
        for (const Callee& c : it->second)
            if (c.required <= args.size() && args.size() <= c.names.size())
                fit.push_back(&c);
        if (fit.empty())
            continue;
        for (size_t k = 0; k < args.size(); ++k)
        {
            const std::string& name = fit[0]->names[k];
            const bool same = std::all_of(fit.begin(), fit.end(), [&](const Callee* c) { return c->names[k] == name; });
            const auto [b, e] = args[k];
            if (!same || name.empty() || b >= e)
                continue;
            if (e == b + 1 && toks[b].id && toks[b].s == name)
                continue;
            out.push_back({toks[b].off, name + ":"});
        }
    }
    return out;
}

} // namespace hot::ls
