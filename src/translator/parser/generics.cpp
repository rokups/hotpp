// Script generics: the scan of the declarations and the table of the instantiations (generics.h).
#include "generics.h"

#include "../builtin_types.h"

#include <cctype>

namespace hotc
{

namespace
{

bool id0(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool idc(char c)
{
    return id0(c) || std::isdigit(static_cast<unsigned char>(c));
}

// The script name of one word of a canonical type: "int32_t" gives "int".
std::string script_word(const std::string& w)
{
    for (const BuiltinType& b : builtin_types())
        if (w == b.cpp)
            return b.script;
    return w;
}

// A lexical token of the scan: a word, a literal, or one punctuation character ("=>" is one token).
struct Tok
{
    std::string text;
    uint32_t pos = 0;
    bool word = false;
};

// The tokens of a ".hot" text without comments. A string or a character literal is one token "\"".
std::vector<Tok> tokens(const std::string& t)
{
    std::vector<Tok> out;
    const size_t n = t.size();
    size_t i = 0;
    while (i < n)
    {
        const char c = t[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && t[i + 1] == '/')
        {
            while (i < n && t[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && t[i + 1] == '*')
        {
            const size_t e = t.find("*/", i + 2);
            i = e == std::string::npos ? n : e + 2;
            continue;
        }
        const uint32_t at = static_cast<uint32_t>(i);
        if (c == '"' && i > 0 && t[i - 1] == '$')
        {
            // An interpolated string: "{expr}" can hold strings and braces.
            ++i;
            while (i < n && t[i] != '"')
            {
                if (t[i] == '\\')
                    i += 2;
                else if (t[i] == '{' && i + 1 < n && t[i + 1] == '{')
                    i += 2;
                else if (t[i] == '{')
                {
                    int depth = 1;
                    ++i;
                    while (i < n && depth > 0)
                    {
                        if (t[i] == '"')
                        {
                            ++i;
                            while (i < n && t[i] != '"')
                                i += t[i] == '\\' ? 2 : 1;
                        }
                        else if (t[i] == '{')
                            ++depth;
                        else if (t[i] == '}')
                            --depth;
                        ++i;
                    }
                }
                else
                    ++i;
            }
            ++i;
            out.push_back({"\"", at, false});
            continue;
        }
        if (c == '"' || (c == '\'' && (i == 0 || !idc(t[i - 1]))))
        {
            ++i;
            while (i < n && t[i] != c)
                i += t[i] == '\\' ? 2 : 1;
            ++i;
            out.push_back({"\"", at, false});
            continue;
        }
        if (id0(c))
        {
            const size_t b = i;
            while (i < n && idc(t[i]))
                ++i;
            out.push_back({t.substr(b, i - b), at, true});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            while (i < n && (idc(t[i]) || t[i] == '.' || t[i] == '\''))
                ++i;
            out.push_back({"0", at, false});
            continue;
        }
        if (c == '=' && i + 1 < n && t[i + 1] == '>')
        {
            out.push_back({"=>", at, false});
            i += 2;
            continue;
        }
        out.push_back({std::string(1, c), at, false});
        ++i;
    }
    return out;
}

// "<" Name { "," Name } ">" at tokens[k] ("<" is tokens[k]). Gives the index after ">", or 0.
size_t param_list(const std::vector<Tok>& ts, size_t k, std::vector<std::string>* names)
{
    if (k >= ts.size() || ts[k].text != "<")
        return 0;
    ++k;
    for (;;)
    {
        if (k >= ts.size() || !ts[k].word)
            return 0;
        if (names)
            names->push_back(ts[k].text);
        ++k;
        if (k < ts.size() && ts[k].text == ">")
            return k + 1;
        if (k >= ts.size() || ts[k].text != ",")
            return 0;
        ++k;
    }
}

bool is_keyword_before_call(const std::string& w)
{
    static const std::set<std::string> kw = {"return", "new", "await", "throw", "in", "is", "as", "case", "else", "operator"};
    return kw.count(w) != 0;
}

// True when the token before a function name ends a return type.
bool ends_type(const Tok& t)
{
    if (t.word)
        return !is_keyword_before_call(t.text);
    return t.text == ">" || t.text == "]" || t.text == "?" || t.text == "*" || t.text == "&";
}

void blank(std::string& text, uint32_t b, uint32_t e)
{
    for (uint32_t i = b; i < e && i < text.size(); ++i)
        if (text[i] != '\n')
            text[i] = ' ';
}

} // namespace

GenericContext*& generic_context()
{
    static thread_local GenericContext* ctx = nullptr;
    return ctx;
}

const GenericDecl* GenericContext::find(const std::string& name) const
{
    for (const GenericDecl& d : decls)
        if (d.name == name)
            return &d;
    return nullptr;
}

void GenericContext::error(uint32_t pos, std::string msg) const
{
    if (!diag)
        return;
    for (const Diag& d : diag->items)
        if (d.pos == pos && d.msg == msg)
            return;
    if (map)
        diag->add(pos, *map, std::move(msg));
    else
    {
        Diag d;
        d.pos = pos;
        d.msg = std::move(msg);
        diag->items.push_back(std::move(d));
    }
}

std::string display_type(const std::string& t)
{
    std::string out;
    for (size_t i = 0; i < t.size();)
    {
        if (t[i] == '\x01')
        {
            ++i;
            continue;
        }
        if (t.compare(i, 2, "::") == 0)
        {
            out += '.';
            i += 2;
            continue;
        }
        if (id0(t[i]))
        {
            const size_t b = i;
            while (i < t.size() && idc(t[i]))
                ++i;
            out += script_word(t.substr(b, i - b));
            continue;
        }
        if (t[i] == ',')
        {
            out += ", ";
            ++i;
            while (i < t.size() && t[i] == ' ')
                ++i;
            continue;
        }
        out += t[i++];
    }
    return out;
}

std::vector<std::string> split_type_args(const std::string& inner)
{
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    auto push = [&]
    {
        const size_t b = cur.find_first_not_of(' ');
        const size_t e = cur.find_last_not_of(' ');
        out.push_back(b == std::string::npos ? std::string() : cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (char c : inner)
    {
        if (c == '<' || c == '(' || c == '[')
            ++depth;
        else if (c == '>' || c == ')' || c == ']')
            --depth;
        if (c == ',' && depth == 0)
        {
            push();
            continue;
        }
        cur += c;
    }
    push();
    return out;
}

void GenericContext::bare(const std::string& name, uint32_t pos)
{
    const GenericDecl* d = find(name);
    if (!d)
        return;
    if (d->kind == GenericDecl::Kind::Fn)
        error(pos, "the generic function '" + name + "' needs explicit type arguments: write '" + name + "<...>(...)'");
    else
        error(pos, "the generic type '" + name + "' needs type arguments: write '" + name + "<...>'");
}

std::string GenericContext::use(const std::string& name, const std::string& inner, uint32_t pos)
{
    const GenericDecl* d = find(name);
    const std::string text = name + "<" + inner + ">";
    if (!d)
        return text;
    std::vector<std::string> args = split_type_args(inner);
    if (args.size() != d->params.size())
    {
        error(pos, "the generic '" + name + "' has " + std::to_string(d->params.size()) + " type parameter"
            + (d->params.size() == 1 ? "" : "s") + ", not " + std::to_string(args.size()));
        return text;
    }
    std::string mangled = name, display = name + "<";
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i].empty())
        {
            error(pos, "a type argument of '" + name + "' is empty");
            return text;
        }
        const std::string shown = display_type(args[i]);
        display += (i ? ", " : "") + shown;
        // Each run of other characters becomes one "_". A "$" is "H" (a handle).
        mangled += "_G_";
        bool sep = false;
        for (char c : shown)
        {
            if (idc(c) && c != '_')
            {
                mangled += c;
                sep = false;
            }
            else if (c == '$')
                mangled += "H";
            else if (!sep && c != ' ')
            {
                mangled += '_';
                sep = true;
            }
        }
        while (!mangled.empty() && mangled.back() == '_')
            mangled.pop_back();
    }
    display += ">";
    for (const GenericInst& g : insts)
        if (g.mangled == mangled)
        {
            if (g.decl != static_cast<size_t>(d - decls.data()) || g.args != args)
            {
                error(pos, "the instantiations '" + g.display + "' and '" + display + "' get the same C++ name '" + mangled
                    + "'. Rename a type");
                return text;
            }
            return mangled;
        }
    if (!record)
        return mangled;
    if (names.count(mangled))
    {
        error(pos, "the instantiation '" + display + "' gets the C++ name '" + mangled
            + "', and a declaration of the module has this name. Rename the declaration");
        return text;
    }
    if (depth >= kMaxGenericDepth)
    {
        error(pos, "the instantiation '" + display + "' is too deep (more than " + std::to_string(kMaxGenericDepth)
            + " levels). A generic uses itself with a larger type argument");
        return text;
    }
    GenericInst g;
    g.decl = static_cast<size_t>(d - decls.data());
    g.args = std::move(args);
    g.mangled = mangled;
    g.display = std::move(display);
    g.pos = pos;
    g.depth = depth + 1;
    insts.push_back(std::move(g));
    return mangled;
}

std::vector<GenericDecl> scan_generics(std::string& text, const LineMap& map, Diagnostics& diag)
{
    std::vector<GenericDecl> out;
    const std::vector<Tok> ts = tokens(text);
    GenericContext err;
    err.diag = &diag;
    err.map = &map;
    int braces = 0, parens = 0;
    bool in_init = false; // after "=" or "=>" of a declaration, up to the ";"
    bool type_body = false; // the brace of depth 1 is the body of a class, a struct or an interface
    bool item_is_type = false; // the item at depth 0 has the word class, struct or interface
    uint32_t item_begin = 0;
    GenericDecl* open = nullptr; // the generic declaration of the item at depth 0
    auto close_item = [&](uint32_t end)
    {
        if (open)
            open->item_end = end;
        open = nullptr;
        item_is_type = false;
        type_body = false;
        in_init = false;
        item_begin = end;
    };
    for (size_t k = 0; k < ts.size(); ++k)
    {
        const Tok& t = ts[k];
        if (t.text == "(" || t.text == "[")
            ++parens;
        else if (t.text == ")" || t.text == "]")
            --parens;
        else if (t.text == "{")
        {
            if (braces == 0)
                type_body = item_is_type && !in_init;
            ++braces;
            continue;
        }
        else if (t.text == "}")
        {
            --braces;
            if (braces == 1)
                in_init = false;
            if (braces <= 0)
            {
                braces = 0;
                if (!in_init)
                    close_item(t.pos + 1);
            }
            continue;
        }
        const bool decl_depth = parens == 0 && (braces == 0 || (braces == 1 && type_body));
        if (!decl_depth)
            continue;
        if (t.text == ";")
        {
            if (braces == 0)
                close_item(t.pos + 1);
            in_init = false;
            continue;
        }
        if (t.text == "=" || t.text == "=>")
        {
            in_init = true;
            continue;
        }
        if (in_init || !t.word)
            continue;
        const bool is_type_kw = t.text == "class" || t.text == "struct";
        if (braces == 0 && (is_type_kw || t.text == "interface" || t.text == "enum"))
        {
            item_is_type = t.text != "enum";
            if (k + 2 >= ts.size() || !ts[k + 1].word || ts[k + 2].text != "<")
                continue;
            if (ts[k + 3 < ts.size() ? k + 3 : k + 2].text == "*")
                continue; // a stub "class Name<*>;"
            const Tok& name = ts[k + 1];
            std::vector<std::string> params;
            const size_t after = param_list(ts, k + 2, &params);
            if (!is_type_kw)
            {
                err.error(name.pos, std::string("a generic ") + t.text + " is not supported. Only a struct, a class and a free function can be generic");
                if (after != 0)
                    blank(text, ts[k + 2].pos, ts[after - 1].pos + 1);
                continue;
            }
            if (after == 0)
            {
                err.error(ts[k + 2].pos, "a type parameter of a generic is a plain name: 'struct Box<T>'");
                continue;
            }
            if (after < ts.size() && ts[after].text == ";")
            {
                err.error(name.pos, "a generic class needs a body");
                continue;
            }
            GenericDecl d;
            d.kind = t.text == "struct" ? GenericDecl::Kind::Struct : GenericDecl::Kind::Class;
            d.name = name.text;
            d.params = std::move(params);
            d.item_begin = item_begin;
            d.name_pos = name.pos;
            blank(text, ts[k + 2].pos, ts[after - 1].pos + 1);
            out.push_back(std::move(d));
            open = &out.back();
            k = after - 1;
            continue;
        }
        // "Name<T>(" after a return type: a generic function, or a generic method in a type body.
        if (k == 0 || !ends_type(ts[k - 1]) || k + 1 >= ts.size() || ts[k + 1].text != "<")
            continue;
        std::vector<std::string> params;
        const size_t after = param_list(ts, k + 1, &params);
        if (after == 0 || after >= ts.size() || ts[after].text != "(")
            continue;
        if (braces == 1)
        {
            err.error(t.pos, "a generic method is not supported. Use a generic free function or a generic class");
            blank(text, ts[k + 1].pos, ts[after - 1].pos + 1);
            k = after - 1;
            continue;
        }
        if (t.text == "main")
        {
            err.error(t.pos, "the function 'main' cannot be generic");
            continue;
        }
        GenericDecl d;
        d.kind = GenericDecl::Kind::Fn;
        d.name = t.text;
        d.params = std::move(params);
        d.item_begin = item_begin;
        d.name_pos = t.pos;
        blank(text, ts[k + 1].pos, ts[after - 1].pos + 1);
        out.push_back(std::move(d));
        open = &out.back();
        k = after - 1;
    }
    if (open)
        open->item_end = static_cast<uint32_t>(text.size());
    // A parameter name is unique and is not a type of the language.
    for (GenericDecl& d : out)
    {
        std::set<std::string> seen;
        for (const std::string& p : d.params)
        {
            if (!seen.insert(p).second)
                err.error(d.name_pos, "the type parameter '" + p + "' of '" + d.name + "' is a duplicate");
            else if (builtin_cpp_name(p) || p == "void" || p == "var" || p == "string")
                err.error(d.name_pos, "the type parameter '" + p + "' of '" + d.name + "' is a type of the language. Use another name");
        }
    }
    return out;
}

std::vector<uint32_t> find_generic_uses(const std::string& text, const std::string& name)
{
    std::vector<uint32_t> out;
    const std::vector<Tok> ts = tokens(text);
    for (size_t k = 0; k + 1 < ts.size(); ++k)
        if (ts[k].word && ts[k].text == name && ts[k + 1].text == "<" && (k == 0 || ts[k - 1].text != "."))
            out.push_back(ts[k].pos);
    return out;
}

} // namespace hotc
