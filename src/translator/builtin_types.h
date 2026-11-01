// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The builtin type names of the script. A plain header: no lexy include.
//
// A script type name has a fixed width. Each alias maps to one canonical C++ name.
// The canonical name is also the emitted text, so the emit, the override key and the
// reload type key all see the same spelling.
#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace hotc
{

struct BuiltinType
{
    const char* script;
    const char* alias; // nullptr when there is no alias
    const char* cpp;
};

inline const std::vector<BuiltinType>& builtin_types()
{
    static const std::vector<BuiltinType> table = {
        {"bool", nullptr, "bool"},
        {"sbyte", "int8", "int8_t"},
        {"byte", "uint8", "uint8_t"},
        {"short", "int16", "int16_t"},
        {"ushort", "uint16", "uint16_t"},
        {"int", "int32", "int32_t"},
        {"uint", "uint32", "uint32_t"},
        {"long", "int64", "int64_t"},
        {"ulong", "uint64", "uint64_t"},
        {"nint", "intptr", "intptr_t"},
        {"nuint", "uintptr", "uintptr_t"},
        {"float", nullptr, "float"},
        {"double", nullptr, "double"},
        {"char", nullptr, "char"},
        {"wchar", "char16", "char16_t"},
    };
    // "Task<T>" is a built-in template type too. It has no row here, because the table maps whole
    // names. canonical_type below rewrites the name "Task" before "<" to "hot::Task". The class
    // is in include/hot/runtime/task.h. It is a value type, not a handle. A script writes it only
    // as the type of a field or a local. An "async" method makes it from the value type.
    return table;
}

// The canonical C++ name of a script builtin name or alias. Gives nullptr for any other word.
inline const char* builtin_cpp_name(std::string_view word)
{
    for (const BuiltinType& b : builtin_types())
        if (word == b.script || (b.alias && word == b.alias))
            return b.cpp;
    return nullptr;
}

namespace builtin_detail
{

inline bool is_id_start(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
inline bool is_id_char(char c)
{
    return is_id_start(c) || std::isdigit(static_cast<unsigned char>(c));
}

// A C++ word that makes a multi-word integer type.
inline bool is_cpp_int_word(std::string_view w)
{
    return w == "unsigned" || w == "signed" || w == "int" || w == "long" || w == "short" || w == "char"
        || w == "double";
}

// The script name for a run of C++ integer words, for the hint of an error.
inline std::string script_name_for_run(const std::vector<std::string>& run)
{
    bool u = false, s = false, ch = false, sh = false, dbl = false;
    int longs = 0;
    for (const std::string& w : run)
    {
        if (w == "unsigned")
            u = true;
        if (w == "signed")
            s = true;
        if (w == "char")
            ch = true;
        if (w == "short")
            sh = true;
        if (w == "double")
            dbl = true;
        if (w == "long")
            ++longs;
    }
    (void)s;
    if (dbl)
        return "double";
    if (ch)
        return u ? "byte" : "sbyte";
    if (sh)
        return u ? "ushort" : "short";
    if (longs)
        return u ? "ulong" : "long";
    return u ? "uint" : "int";
}

// The fixed-width name of a C++ run of integer words. The raw text stays for "long double".
inline std::string native_name_for_run(const std::vector<std::string>& run, const std::string& raw)
{
    bool u = false, ch = false, sh = false, dbl = false;
    int longs = 0;
    for (const std::string& w : run)
    {
        if (w == "unsigned")
            u = true;
        if (w == "char")
            ch = true;
        if (w == "short")
            sh = true;
        if (w == "double")
            dbl = true;
        if (w == "long")
            ++longs;
    }
    if (dbl)
        return raw;
    if (ch)
        return run.size() == 1 ? "char" : (u ? "uint8_t" : "int8_t");
    if (sh)
        return u ? "uint16_t" : "int16_t";
    if (longs >= 2 || (longs == 1 && sizeof(long) == 8))
        return u ? "uint64_t" : "int64_t";
    return u ? "uint32_t" : "int32_t";
}

} // namespace builtin_detail

// Rewrites the builtin words of a type text to the canonical C++ names. A word after "::" is
// a member name and stays. The multi-word C++ spellings (`unsigned`, `unsigned int`,
// `long long`, `unsigned char` and so on) are not script types. The text stays as it is for
// them, and *error gets the message with the hint. Only the first error is kept.
//
// With native = true the text is a C++ spelling from libclang. Then the multi-word spellings
// map to the fixed-width names, with the size of "long" of this host. This makes the key of
// a native virtual parameter equal to the key of the script parameter.
inline std::string canonical_type(std::string_view text, std::string* error = nullptr, bool native = false)
{
    std::string out;
    const size_t n = text.size();
    size_t i = 0;
    while (i < n)
    {
        const char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            while (i < n && (builtin_detail::is_id_char(text[i]) || text[i] == '.'))
                out += text[i++];
            continue;
        }
        if (!builtin_detail::is_id_start(c))
        {
            out += c;
            ++i;
            continue;
        }
        const size_t b = i;
        while (i < n && builtin_detail::is_id_char(text[i]))
            ++i;
        const std::string_view word = text.substr(b, i - b);
        const bool scoped = out.size() >= 2 && out.compare(out.size() - 2, 2, "::") == 0;
        if (scoped || !builtin_detail::is_cpp_int_word(word))
        {
            if (!scoped)
            {
                const char* cpp = builtin_cpp_name(word);
                size_t k = i;
                while (k < n && std::isspace(static_cast<unsigned char>(text[k])))
                    ++k;
                // The built-in template "Task<T>" is the class hot::Task (not for a libclang spelling).
                if (!cpp && !native && word == "Task" && k < n && text[k] == '<')
                    cpp = "hot::Task";
                out += cpp ? cpp : std::string(word);
            }
            else
            {
                out += word;
            }
            continue;
        }
        // A run of C++ integer words that only white space separates.
        std::vector<std::string> run{std::string(word)};
        std::string raw(word);
        size_t j = i;
        for (;;)
        {
            size_t k = j;
            while (k < n && std::isspace(static_cast<unsigned char>(text[k])))
                ++k;
            if (k == j || k >= n || !builtin_detail::is_id_start(text[k]))
                break;
            size_t e = k;
            while (e < n && builtin_detail::is_id_char(text[e]))
                ++e;
            if (!builtin_detail::is_cpp_int_word(text.substr(k, e - k)))
                break;
            run.emplace_back(text.substr(k, e - k));
            raw += ' ';
            raw += run.back();
            j = e;
        }
        if (native)
        {
            out += builtin_detail::native_name_for_run(run, raw);
        }
        else if (run.size() >= 2 || run[0] == "unsigned" || run[0] == "signed")
        {
            if (error && error->empty())
                *error = "'" + raw + "' is not a script type; use '" + builtin_detail::script_name_for_run(run) + "'";
            out += raw;
        }
        else
        {
            const char* cpp = builtin_cpp_name(run[0]);
            out += cpp ? cpp : run[0];
        }
        i = j;
    }
    return out;
}

// True when the type text names a task type "Task<...>" (also "hot::Task<...>"), at any depth
// of a template, for example "List<Task<int>>".
inline bool mentions_task_type(std::string_view text)
{
    for (size_t at = text.find("Task<"); at != std::string_view::npos; at = text.find("Task<", at + 1))
    {
        const char before = at ? text[at - 1] : ' ';
        if (!std::isalnum(static_cast<unsigned char>(before)) && before != '_')
            return true;
    }
    return false;
}

// The value type T of a task type "hot::Task<T>" (also "Task<T>"). Gives an empty text for any
// other type and for "Task<void>".
inline std::string task_value_type(std::string_view text)
{
    size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])))
        --e;
    text = text.substr(b, e - b);
    for (std::string_view head : {std::string_view("hot::Task<"), std::string_view("Task<")})
    {
        if (text.size() <= head.size() || text.substr(0, head.size()) != head || text.back() != '>')
            continue;
        std::string_view inner = text.substr(head.size(), text.size() - head.size() - 1);
        while (!inner.empty() && std::isspace(static_cast<unsigned char>(inner.front())))
            inner.remove_prefix(1);
        while (!inner.empty() && std::isspace(static_cast<unsigned char>(inner.back())))
            inner.remove_suffix(1);
        return inner == "void" ? std::string() : std::string(inner);
    }
    return {};
}

// The script spelling of a canonical type text, for a message to the script author:
// "int32_t&" gives "int&". A word after "::" stays.
inline std::string script_type_text(std::string_view text)
{
    std::string out;
    const size_t n = text.size();
    size_t i = 0;
    while (i < n)
    {
        if (!builtin_detail::is_id_start(text[i]))
        {
            out += text[i++];
            continue;
        }
        const size_t b = i;
        while (i < n && builtin_detail::is_id_char(text[i]))
            ++i;
        const std::string_view word = text.substr(b, i - b);
        const bool scoped = out.size() >= 2 && out.compare(out.size() - 2, 2, "::") == 0;
        const BuiltinType* hit = nullptr;
        if (!scoped)
            for (const BuiltinType& t : builtin_types())
                if (word == t.cpp)
                    hit = &t;
        out += hit ? std::string(hit->script) : std::string(word);
    }
    return out;
}

} // namespace hotc
