// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "attributes.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace hot::ls
{

const std::vector<AttributeInfo>& attribute_table()
{
    static const std::vector<AttributeInfo> t = {
        {"native", "The stub names a native class.", 0, {}, "stub"},
        {"value", "The stub names a value type.", 0, {}, "stub"},
        {"self", "The rule is for this type only, not for the subclasses.", 0, {}, "stub"},
        {"checked_index", "The index operator has a bounds test.", 0, {}, "stub"},
        {"nonnull", "A native call that returns the type gives a non null value.", 0, {}, "stub"},
        {"weak", "A weak reference to a script object.", 0, {}, "field, parameter, stub"},
        {"attribute", "Declares a script attribute class: [attribute] class Name;", 0, {}, "class"},
        {"handle", "The handle template of the type.", 1, {}, "stub, module"},
        {"default_handle", "The default handle template of the stubs.", 1, {}, "stub"},
        {"weak_handle", "The weak handle template of the type.", 1, {}, "stub"},
        {"weak_source", "The traits base of a weak source type.", 1, {}, "stub"},
        {"factory", "The factory that makes an object of the type.", 1, {}, "stub"},
        {"raw_pointer", "The member of the handle that gives the raw pointer. Default: get.", 1, {}, "stub"},
        {"alias", "The old key of a renamed member, class or lambda.", 1, {}, "class, member, lambda"},
        {"cpp_prelude", "C++ text for this class only.", 1, {}, "class, stub"},
        {"cpp_overrides", "The names of the native virtuals that the prelude overrides.", 1, {}, "stub"},
        {"std", "The name of the standard namespace.", 1, {}, "module"},
        {"lib", "A standard-like library namespace.", 1, {}, "module"},
        {"array", "The C++ template of a fixed-size array type.", 1, {}, "module"},
        {"optional", "The namespace of the optional type T? and of null for a T?.", 1, {}, "module"},
        {"fn_invoker", "A macro that makes the call entry <name>@call of each public free function.", 1, {}, "module"},
        {"intrusive", "The add and release functions of an intrusive handle.", 2, {}, "stub"},
        {"byval", "The parameter passes by value.", 0, {}, "parameter"},
        {"name", "The key of a reloadable lambda.", 1, {}, "lambda"},
    };
    return t;
}

const AttributeInfo* find_attribute(const std::string& name)
{
    for (const AttributeInfo& a : attribute_table())
        if (a.name == name)
            return &a;
    return nullptr;
}

namespace
{

bool id_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The names of the arguments that the list before the offset already has: "name =".
std::set<std::string> used_named(std::string_view list)
{
    std::set<std::string> r;
    const std::string s(list);
    const std::regex re("(\\w+)\\s*=");
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
        r.insert((*it)[1].str());
    return r;
}

} // namespace

// The text with the comments and the string literals replaced by spaces. The length and the
// line breaks stay.
static std::string blank_comments_and_strings(std::string_view l)
{
    std::string out(l);
    const auto fill = [&](size_t b, size_t e)
    {
        for (size_t k = b; k < e; ++k)
            if (out[k] != '\n')
                out[k] = ' ';
    };
    for (size_t i = 0; i < l.size();)
    {
        if (l.compare(i, 2, "//") == 0)
        {
            const size_t e = std::min(l.find('\n', i), l.size());
            fill(i, e);
            i = e;
        }
        else if (l.compare(i, 2, "/*") == 0)
        {
            const size_t c = l.find("*/", i + 2);
            const size_t e = c == std::string_view::npos ? l.size() : c + 2;
            fill(i, e);
            i = e;
        }
        else if (l[i] == '"' || l[i] == '\'')
        {
            size_t e = i + 1;
            while (e < l.size() && l[e] != l[i] && l[e] != '\n')
                e += l[e] == '\\' ? 2 : 1;
            e = std::min(e + 1, l.size());
            fill(i, e);
            i = e;
        }
        else
            ++i;
    }
    return out;
}

nlohmann::json attribute_completion(std::string_view text, std::size_t offset, std::size_t prefix_start, bool arg)
{
    using nlohmann::json;
    json items = json::array();
    offset = std::min(offset, text.size());
    if (arg)
    {
        // Find the "(" that is not closed before the cursor, then the name before it.
        int depth = 0;
        std::size_t open = std::string_view::npos;
        for (std::size_t i = offset; i-- > 0;)
        {
            if (text[i] == ')' || text[i] == ']')
                ++depth;
            else if (text[i] == '[' && depth == 0)
                break;
            else if (text[i] == '[')
                --depth;
            else if (text[i] == '(' && depth == 0)
            {
                open = i;
                break;
            }
            else if (text[i] == '(')
                --depth;
        }
        if (open != std::string_view::npos)
        {
            std::size_t b = open;
            while (b > 0 && text[b - 1] == ' ')
                --b;
            std::size_t a = b;
            while (a > 0 && id_char(text[a - 1]))
                --a;
            const AttributeInfo* info = find_attribute(std::string(text.substr(a, b - a)));
            if (info)
            {
                const std::set<std::string> used = used_named(text.substr(open + 1, prefix_start - open - 1));
                const std::string prefix(text.substr(prefix_start, offset - prefix_start));
                for (const std::string& n : info->named)
                    if (!used.count(n) && n.compare(0, prefix.size(), prefix) == 0)
                        items.push_back({{"label", n}, {"kind", 5}, {"filterText", n}, {"sortText", "0" + n},
                            {"insertText", n + " = "}});
            }
        }
        return {{"isIncomplete", false}, {"items", items}};
    }

    const std::string prefix(text.substr(prefix_start, offset - prefix_start));
    // The word end. A "]" or "(" after the word means that the user has the closing part.
    std::size_t e = offset;
    while (e < text.size() && id_char(text[e]))
        ++e;
    while (e < text.size() && (text[e] == ' ' || text[e] == '\t'))
        ++e;
    const bool close = e >= text.size() || (text[e] != ']' && text[e] != '(' && text[e] != ',');
    std::set<std::string> seen;
    auto add = [&](const std::string& name, const std::string& doc, const std::string& detail, int arity, bool user)
    {
        if (name.compare(0, prefix.size(), prefix) != 0 || !seen.insert(name).second)
            return;
        std::string snippet = name;
        for (int i = 0; i < arity; ++i)
            snippet += (i ? ", \"$" : "(\"$") + std::to_string(i + 1) + "\"";
        if (arity > 0)
            snippet += ")";
        if (close)
            snippet += "]";
        snippet += "$0";
        json item = {{"label", name}, {"kind", 20}, {"detail", detail}, {"filterText", name},
            {"sortText", (user ? "0" : "1") + name}, {"insertTextFormat", 2}, {"insertText", snippet}};
        if (!doc.empty())
            item["documentation"] = doc;
        items.push_back(std::move(item));
    };
    // The attribute classes of the document first.
    const std::string all = blank_comments_and_strings(text);
    const std::regex re("\\[\\s*attribute\\s*\\]\\s*class\\s+(\\w+)");
    for (std::sregex_iterator it(all.begin(), all.end(), re), end; it != end; ++it)
        add((*it)[1].str(), "A script attribute class of this file.", "script attribute", 0, true);
    for (const AttributeInfo& a : attribute_table())
        add(a.name, a.doc, a.where, a.arity, false);
    return {{"isIncomplete", false}, {"items", items}};
}

} // namespace hot::ls
