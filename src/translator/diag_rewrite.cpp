// The diagnostic rewriter (design 4.2, experiment F2). See rewrite_diagnostic in sppc_lib.h.
//
// The input is the text that Clang prints for the generated C++. The rewriter works on lines.
// A header line has the form "file:line:col: level: message". Clang writes the lines of the
// source snippet after it. This file does these steps:
//   1. Find the header lines and group each error with its notes.
//   2. Rewrite the message: a plain text rule first, then the generated names.
//   3. Replace or drop the snippet lines that show generated code.
// The table of the common mistakes is kMistakes. Each row has a test in
// tests/golden/diagnostics (one script and one expected text).
#include "spp/sppc_lib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace sppc
{

namespace
{

// A message longer than this gets the plain-text steps only. A regex on a very long line
// can use too much stack, and a message of that size is a template dump.
constexpr size_t kMaxRegexMessage = 3000;

bool ends_with(const std::string& s, const char* suffix)
{
    const std::string x(suffix);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

void replace_all(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
}

// ------------------------------------------------------------------ header lines

struct Header
{
    std::string file;
    int line = 0;
    int col = 0;
    std::string level; // "error", "fatal error", "warning", "note" or "remark"
    std::string message;
};

// Reads "file:line:col" that ends at "end" (the position of ": level: ").
bool split_location(const std::string& l, size_t end, Header& h)
{
    auto digits_back = [&](size_t e, size_t& begin)
    {
        begin = e;
        while (begin > 0 && std::isdigit(static_cast<unsigned char>(l[begin - 1])))
            --begin;
        return begin < e && e - begin <= 9;
    };
    size_t col_begin = 0;
    if (!digits_back(end, col_begin) || col_begin < 2 || l[col_begin - 1] != ':')
        return false;
    const size_t line_end = col_begin - 1;
    size_t line_begin = 0;
    if (!digits_back(line_end, line_begin) || line_begin < 2 || l[line_begin - 1] != ':')
        return false;
    h.file = l.substr(0, line_begin - 1);
    h.line = std::atoi(l.substr(line_begin, line_end - line_begin).c_str());
    h.col = std::atoi(l.substr(col_begin, end - col_begin).c_str());
    return true;
}

bool parse_header(const std::string& l, Header& h)
{
    static const char* const kLevels[] = {"fatal error", "error", "warning", "note", "remark"};
    size_t best = std::string::npos;
    const char* best_level = nullptr;
    for (const char* level : kLevels)
    {
        const std::string tag = std::string(": ") + level + ": ";
        for (size_t p = l.find(tag); p != std::string::npos && p < best; p = l.find(tag, p + 1))
        {
            Header probe;
            if (!split_location(l, p, probe))
                continue;
            best = p;
            best_level = level;
            break;
        }
    }
    if (!best_level)
        return false;
    split_location(l, best, h);
    h.level = best_level;
    h.message = l.substr(best + 4 + h.level.size());
    return true;
}

// A source line of a snippet starts with "  12 |". The caret line starts with "     |".
bool parse_gutter(const std::string& l, int& number, size_t& text_begin)
{
    size_t i = 0;
    while (i < l.size() && l[i] == ' ')
        ++i;
    size_t d = i;
    while (d < l.size() && std::isdigit(static_cast<unsigned char>(l[d])))
        ++d;
    const int n = d > i && d - i <= 9 ? std::atoi(l.substr(i, d - i).c_str()) : 0;
    size_t b = d;
    while (b < l.size() && l[b] == ' ')
        ++b;
    if (b >= l.size() || l[b] != '|')
        return false;
    number = n;
    text_begin = b + 1 < l.size() && l[b + 1] == ' ' ? b + 2 : b + 1;
    return true;
}

enum class Place
{
    Script,
    Generated,
    System,
    Other
};

Place classify(const std::string& file, const std::map<std::string, std::vector<std::string>>& scripts)
{
    if (scripts.count(file) || ends_with(file, ".spp"))
        return Place::Script;
    static const char* const kSystem[] = {
        "/include/c++/", "/lib/gcc/", "/lib64/gcc/", "/usr/include/", "/lib/clang/", "/lib64/clang/", "/lib/llvm"};
    for (const char* m : kSystem)
        if (file.find(m) != std::string::npos)
            return Place::System;
    if (ends_with(file, ".cpp"))
        return Place::Generated;
    return Place::Other;
}

// ------------------------------------------------------------------ names

const char* script_type_name(const std::string& t)
{
    static const std::map<std::string, const char*> kTypes = {
        {"int8_t", "sbyte"},
        {"uint8_t", "byte"},
        {"int16_t", "short"},
        {"uint16_t", "ushort"},
        {"int32_t", "int"},
        {"uint32_t", "uint"},
        {"int64_t", "long"},
        {"uint64_t", "ulong"},
        {"intptr_t", "nint"},
        {"uintptr_t", "nuint"},
        {"char16_t", "wchar"},
        {"nullptr_t", "null"},
    };
    auto it = kTypes.find(t);
    return it == kTypes.end() ? nullptr : it->second;
}

// "Player_Shell" -> "Player". "PlayerState_g3" -> "Player". "Player__heal" -> "Player.heal".
std::string script_name(const std::string& t)
{
    if (const char* s = script_type_name(t))
        return s;
    if (t.empty() || !std::isalpha(static_cast<unsigned char>(t[0])))
        return t;
    if (t.size() > 6 && ends_with(t, "_Shell"))
        return t.substr(0, t.size() - 6);
    const size_t st = t.rfind("State_g");
    if (st != std::string::npos && st > 0 && st + 7 < t.size()
        && std::all_of(t.begin() + static_cast<long>(st) + 7, t.end(),
            [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return t.substr(0, st);
    const size_t us = t.find("__");
    if (us != std::string::npos && us > 0 && us + 2 < t.size())
    {
        std::string member = t.substr(us + 2);
        if (member.size() > 5 && ends_with(member, "_virt"))
            member.resize(member.size() - 5);
        return t.substr(0, us) + "." + member;
    }
    return t;
}

// Removes " (aka 'type')". The script has no alias for a type.
void drop_aka(std::string& s)
{
    for (size_t p = s.find(" (aka '"); p != std::string::npos; p = s.find(" (aka '", p))
    {
        const size_t e = s.find("')", p);
        if (e == std::string::npos)
            break;
        s.erase(p, e + 2 - p);
    }
}

// "const char[5]" -> "string".
void rename_char_arrays(std::string& s)
{
    for (size_t p = s.find("const char["); p != std::string::npos; p = s.find("const char[", p + 1))
    {
        size_t e = p + 11;
        while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])))
            ++e;
        if (e == p + 11 || e >= s.size() || s[e] != ']')
            continue;
        s.replace(p, e + 1 - p, "string");
    }
}

// Generated names to script names, in any text of a message.
std::string rename_generated(std::string s)
{
    drop_aka(s);
    replace_all(s, "std::__cxx11::basic_string<char>", "std::string");
    replace_all(s, "std::basic_string<char>", "std::string");
    replace_all(s, "std::nullptr_t", "nullptr_t");
    rename_char_arrays(s);
    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c) && c != '_')
        {
            out += s[i++];
            continue;
        }
        size_t e = i;
        while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_'))
            ++e;
        const std::string token = s.substr(i, e - i);
        out += std::isdigit(c) ? token : script_name(token); // a number or "1st" stays
        i = e;
    }
    replace_all(out, "::", ".");
    return out;
}

// ------------------------------------------------------------------ messages

std::string ordinal(int n)
{
    const int t = n % 100;
    const char* suffix = (t >= 11 && t <= 13) ? "th"
        : n % 10 == 1                         ? "st"
        : n % 10 == 2                         ? "nd"
        : n % 10 == 3                         ? "rd"
                                              : "th";
    return std::to_string(n) + suffix;
}

// Calls "fn" on each match of "re" in "s". The result of "fn" replaces the match.
std::string replace_matches(
    const std::string& s, const std::regex& re, const std::function<std::string(const std::smatch&)>& fn)
{
    std::string out;
    auto last = s.cbegin();
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
    {
        out.append(last, (*it)[0].first);
        out += fn(*it);
        last = (*it)[0].second;
    }
    out.append(last, s.cend());
    return out;
}

// The number words of a call. A script function has a hidden first argument "self" (the object).
// The script does not write it, so it leaves the count and the position.
std::string fix_counts(const std::string& msg, bool script_function)
{
    static const std::regex expected(R"(expected (\d+), have (\d+))");
    static const std::regex requires_(
        R"(requires (at least |at most )?(\d+) arguments?, but (\d+) (?:was|were) provided)");
    static const std::regex position(R"(for (\d+)(?:st|nd|rd|th) argument)");
    auto minus = [&](const std::string& n) { return std::max(std::atoi(n.c_str()) - (script_function ? 1 : 0), 0); };
    std::string out = replace_matches(msg, requires_, [&](const std::smatch& m)
    {
        const int n = minus(m[2]);
        const int given = minus(m[3]);
        return "takes " + m[1].str() + std::to_string(n) + (n == 1 ? " argument" : " arguments") + ", but "
            + std::to_string(given) + (given == 1 ? " was" : " were") + " given";
    });
    if (script_function)
    {
        out = replace_matches(out, expected, [&](const std::smatch& m)
        { return "expected " + std::to_string(minus(m[1])) + ", have " + std::to_string(minus(m[2])); });
        out = replace_matches(out, position, [&](const std::smatch& m)
        {
            const int k = std::atoi(m[1].str().c_str()) - 1;
            return k < 1 ? std::string("for the object") : "for " + ordinal(k) + " argument";
        });
    }
    return out;
}

struct Rule
{
    std::regex re;
    const char* replacement;
};

// The table of the common mistakes. The match is at the start of the message. Names in the
// replacement are renamed afterwards, so 'Player__heal' shows as 'Player.heal'.
// Order matters: a specific row comes before a general row.
const std::vector<Rule>& mistakes()
{
    static const std::vector<Rule> kMistakes = {
        // 1. A typo in a member name of a script class (field, property or method).
        {std::regex(R"(^no member named '([^']+)' in '(\w+?)(?:_Shell|State_g\d+)')"),
            "script class '$2' has no member '$1'"},
        // 2. A typo in a member name, or a native method that does not exist.
        {std::regex(R"(^no member named '([^']+)' in '([^']+)')"), "'$2' has no member '$1'"},
        // 3. A call of a script method that does not exist.
        {std::regex(R"(^use of undeclared identifier '([A-Za-z]\w*?)__(\w+)')"),
            "script class '$1' has no method '$2'"},
        // 4. A name that is not declared.
        {std::regex(R"(^use of undeclared identifier '([^']+)')"),
            "unknown name '$1': it is not a local, a parameter, a field or a type"},
        // 5. A type that is not known.
        {std::regex(R"(^unknown type name '([^']+)')"), "unknown type '$1': add a 'using' line or a native stub"},
        // 6. Too few arguments.
        {std::regex(R"(^too few arguments to function call, single argument '([^']+)' was not specified)"),
            "missing argument '$1'"},
        {std::regex(R"(^too few arguments to function call, expected (\d+), have (\d+))"),
            "too few arguments: expected $1, got $2"},
        // 7. Too many arguments.
        {std::regex(R"(^too many arguments to function call, expected (\d+), have (\d+))"),
            "too many arguments: expected $1, got $2"},
        // 8. No overload fits the call.
        {std::regex(R"(^no matching (?:member )?function for call to '([^']+)')"),
            "no method or function '$1' fits these arguments"},
        {std::regex(R"(^no matching constructor for initialization of '([^']+)')"),
            "no constructor of '$1' fits these arguments"},
        {std::regex(R"(^candidate does not fit: no known conversion from '([^']+)' to '([^']+)' for (.+?) argument)"),
            "candidate does not fit: the $3 argument needs '$2', but it has type '$1'"},
        // 9. A value of the wrong type.
        {std::regex(R"(^assigning to '([^']+)' from incompatible type '([^']+)')"),
            "cannot assign a value of type '$2' to '$1'"},
        {std::regex(R"(^cannot initialize a variable of type '[^']+' with an? [lr]value of type 'void')"),
            "the method returns void: its result is not a value"},
        {std::regex(R"(^cannot initialize a variable of type '([^']+)' with an? [lr]value of type '([^']+)')"),
            "cannot initialize a variable of type '$1' with a value of type '$2'"},
        {std::regex(R"(^cannot initialize return object of type '([^']+)' with an? [lr]value of type '([^']+)')"),
            "wrong return type: the method returns '$1', but the value has type '$2'"},
        {std::regex(R"(^cannot initialize a parameter of type '([^']+)' with an? [lr]value of type '([^']+)')"),
            "wrong argument type: the parameter has type '$1', but the value has type '$2'"},
        {std::regex(R"(^no viable conversion from '([^']+)' to '([^']+)')"), "cannot convert '$1' to '$2'"},
        {std::regex(R"(^no viable overloaded '=')"), "cannot assign: no assignment fits these types"},
        {std::regex(R"(^invalid operands to binary expression \((.*)\))"),
            "the operator does not fit these operand types ($1)"},
        // 9b. A temporary value where the code needs a variable.
        {std::regex(R"(^cannot take the address of an rvalue of type '([^']+)')"),
            "cannot take the address of a temporary value of type '$1'. Store the value in a local first, "
            "then use '&' on the local"},
        {std::regex(R"(^non-const lvalue reference to type '([^']+)' cannot bind to a temporary of type '([^']+)')"),
            "a 'T&' needs a variable, but this is a temporary value (type '$2'). Store the value in a local first, "
            "or use 'const $1&'"},
        {std::regex(
             R"(^non-const lvalue reference to type '([^']+)' cannot bind to a value of unrelated type '([^']+)')"),
            "a 'T&' needs a variable of the type '$1', but the value has type '$2'. Use a variable of the type '$1', "
            "or use 'const $1&'"},
        {std::regex(R"(^candidate does not fit: expects an lvalue for (.+?) argument)"),
            "candidate does not fit: the $1 argument is a 'T&' parameter and needs a variable. Store the value in "
            "a local first"},
        // 10. A method that does not return a value on each path.
        {std::regex(R"(^non-void function does not return a value( in all control paths)?)"),
            "the method does not return a value on every path"},
        // 11. A const value that the code changes.
        {std::regex(R"(^cannot assign to variable '([^']+)' with const-qualified type '([^']+)')"),
            "cannot assign to '$1': it is const (type '$2')"},
        {std::regex(R"(^variable '([^']+)' declared const here)"), "'$1' is declared const here"},
        {std::regex(
             R"(^'this' argument to member function '([^']+)' has type '([^']+)', but function is not marked const)"),
            "'$1' changes the object, but the object is const (type '$2')"},
        // 12. A name that is declared two times.
        {std::regex(R"(^redefinition of '([^']+)')"), "'$1' is already declared in this scope"},
        {std::regex(R"(^previous definition is here)"), "the first declaration is here"},
    };
    return kMistakes;
}

// The first quoted name with "__" in it: a generated script function such as 'Player__heal'.
bool has_script_function(const std::string& msg)
{
    if (msg.size() > kMaxRegexMessage)
        return false;
    static const std::regex fn(R"('[A-Za-z]\w*?__\w+')");
    return std::regex_search(msg, fn);
}

std::string rewrite_message(std::string msg, bool script_function)
{
    drop_aka(msg);
    // The warning flag, for example " [-Werror,-Wreturn-type]". A script has no such switch.
    if (!msg.empty() && msg.back() == ']')
    {
        const size_t flag = msg.rfind(" [-W");
        if (flag != std::string::npos && msg.find(' ', flag + 1) == std::string::npos)
            msg.erase(flag);
    }
    replace_all(msg, "candidate function not viable: ", "candidate does not fit: ");
    if (msg.size() <= kMaxRegexMessage)
    {
        msg = fix_counts(msg, script_function);
        for (const Rule& r : mistakes())
        {
            if (!std::regex_search(msg, r.re))
                continue;
            msg = std::regex_replace(msg, r.re, r.replacement, std::regex_constants::format_first_only);
            break;
        }
    }
    return rename_generated(msg);
}

// ------------------------------------------------------------------ the pass over the lines

std::vector<std::string> split_lines(const std::string& text, bool& final_newline)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    final_newline = cur.empty();
    if (!cur.empty())
        lines.push_back(cur);
    return lines;
}

// The last "#line N "script"" before the line "before" (a 1-based line of the generated text).
// A directive that names the generated file itself (the reset) does not count.
bool last_script_line(const std::vector<std::string>& gen, size_t before, std::string& file, int& line)
{
    for (size_t i = std::min(before, gen.size() + 1); i-- > 1;)
    {
        const std::string& l = gen[i - 1];
        if (l.rfind("#line ", 0) != 0)
            continue;
        const size_t q1 = l.find('"');
        const size_t q2 = l.rfind('"');
        if (q1 == std::string::npos || q2 <= q1)
            continue;
        const std::string name = l.substr(q1 + 1, q2 - q1 - 1);
        if (ends_with(name, ".cpp"))
            continue;
        file = name;
        line = std::atoi(l.c_str() + 6);
        return line > 0;
    }
    return false;
}

enum class Snippet
{
    None,
    Keep,
    Replace,
    Drop
};

// The physical lines of a generated text, by the script location that "#line" gives them.
// The key is the name and the presumed line.
using LineIndex = std::map<std::pair<std::string, int>, std::vector<int>>;

LineIndex index_generated(const std::vector<std::string>& gen)
{
    LineIndex index;
    std::string name;
    int next = 0; // the presumed line of the next physical line; 0: no directive yet
    for (size_t i = 0; i < gen.size(); ++i)
    {
        const std::string& l = gen[i];
        if (l.rfind("#line ", 0) == 0)
        {
            const size_t q1 = l.find('"');
            const size_t q2 = l.rfind('"');
            if (q1 != std::string::npos && q2 > q1)
            {
                name = l.substr(q1 + 1, q2 - q1 - 1);
                next = std::atoi(l.c_str() + 6);
            }
            continue;
        }
        if (next > 0)
            index[{name, next}].push_back(static_cast<int>(i) + 1);
        if (next > 0)
            ++next;
    }
    return index;
}

// The script column of a generated column on the script line "line". Zero: not known. The
// column of a text that the translator writes by itself (for example "spp::deref(") has no
// segment, so the function looks at the next columns of the same generated line. Several
// generated lines can have the same script line (a generation function, a member head, a
// body). "snippet" is the generated line that the compiler shows. It selects the right one.
int script_column(const SourceMap& map, const LineIndex& index, const std::vector<std::string>& gen,
    const std::string& file, int line, int col, const std::string& snippet)
{
    auto it = index.find({file, line});
    if (it == index.end() || col < 1)
        return 0;
    auto trimmed = [](const std::string& t)
    {
        const size_t b = t.find_first_not_of(" \t");
        return b == std::string::npos ? std::string() : t.substr(b, t.find_last_not_of(" \t\r") - b + 1);
    };
    std::vector<int> candidates;
    if (!trimmed(snippet).empty())
        for (int physical : it->second)
            if (trimmed(gen[static_cast<size_t>(physical) - 1]) == trimmed(snippet))
                candidates.push_back(physical);
    if (candidates.empty())
        candidates = it->second;
    // The file number in the map: 0 is the main script, else an imported script.
    uint32_t want = 0;
    for (size_t i = 0; i < map.files().size(); ++i)
        if (map.files()[i].name == file)
            want = static_cast<uint32_t>(i + 1);
    for (int physical : candidates)
    {
        const size_t width = gen[static_cast<size_t>(physical) - 1].size();
        for (size_t c = static_cast<size_t>(col); c <= width && c < static_cast<size_t>(col) + 80; ++c)
        {
            const auto p = map.locate(physical, static_cast<int>(c));
            if (p && p->file == want && p->pos.line == line)
                return p->pos.col;
        }
    }
    return 0;
}

} // namespace

std::string rewrite_diagnostic(
    const std::string& text, const ScriptTexts& scripts, const ScriptTexts& generated, const SourceMap* map)
{
    bool final_newline = true;
    const std::vector<std::string> lines = split_lines(text, final_newline);

    std::map<std::string, std::vector<std::string>> script_lines;
    for (const auto& s : scripts)
    {
        bool nl = true;
        script_lines[s.first] = split_lines(s.second, nl);
    }
    // The imported scripts of the source map. The caller needs not list them.
    if (map)
        for (const SourceFile& f : map->files())
        {
            bool nl = true;
            if (!script_lines.count(f.name))
                script_lines[f.name] = split_lines(f.text, nl);
        }

    std::map<std::string, std::vector<std::string>> generated_lines;
    for (const auto& g : generated)
    {
        bool nl = true;
        generated_lines[g.first] = split_lines(g.second, nl);
    }

    LineIndex line_index;
    const std::vector<std::string>* map_gen = nullptr; // the generated text that "map" describes
    if (map && !map->empty() && !generated.empty())
    {
        map_gen = &generated_lines[generated.front().first];
        line_index = index_generated(*map_gen);
    }

    // Pass 1: the headers, and for each error group whether it calls a script function.
    std::vector<Header> headers(lines.size());
    std::vector<bool> is_header(lines.size(), false);
    std::vector<bool> script_function(lines.size(), false);
    for (size_t i = 0; i < lines.size(); ++i)
        is_header[i] = parse_header(lines[i], headers[i]);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!is_header[i] || headers[i].level == "note")
            continue;
        size_t end = i + 1;
        bool found = has_script_function(headers[i].message);
        for (; end < lines.size() && (!is_header[end] || headers[end].level == "note"); ++end)
            if (is_header[end] && !found)
                found = has_script_function(headers[end].message);
        for (size_t k = i; k < end; ++k)
            script_function[k] = found;
    }

    // Pass 2: rewrite.
    std::vector<std::string> out;
    std::string primary_location; // "file:line:col" of the last error that points into a script
    Snippet snippet = Snippet::None;
    int hidden = 0;
    const std::vector<std::string>* snippet_script = nullptr;

    auto flush_hidden = [&]
    {
        if (hidden == 0)
            return;
        out.push_back((primary_location.empty() ? std::string() : primary_location + ": ")
            + "note: " + std::to_string(hidden) + (hidden == 1 ? " note" : " notes")
            + " in system headers hidden (use --raw-diagnostics to see them)");
        hidden = 0;
    };

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& l = lines[i];
        if (is_header[i])
        {
            Header h = headers[i];
            const bool note = h.level == "note";
            Place place = classify(h.file, script_lines);
            if (!note)
            {
                flush_hidden();
                primary_location.clear();
            }
            if (note && place == Place::System)
            {
                ++hidden;
                snippet = Snippet::Drop;
                continue;
            }
            bool mapped = false; // the snippet numbers are then lines of the generated file
            if (place == Place::Generated && !note)
            {
                // Use the last script line before the position in the generated file.
                auto g = generated_lines.find(h.file);
                if (g != generated_lines.end() && h.line > 0)
                {
                    std::string file;
                    int script_line = 0;
                    if (last_script_line(g->second, static_cast<size_t>(h.line), file, script_line))
                    {
                        h.file = file;
                        h.line = script_line;
                        place = Place::Script;
                        mapped = true;
                    }
                }
            }
            if (place == Place::Script)
            {
                // The generated column means nothing here, unless the source map gives the real one.
                std::string shown; // the generated line that the compiler shows under the header
                int shown_number = 0;
                size_t shown_begin = 0;
                if (i + 1 < lines.size() && parse_gutter(lines[i + 1], shown_number, shown_begin))
                    shown = lines[i + 1].substr(shown_begin);
                const int real =
                    map_gen && !mapped ? script_column(*map, line_index, *map_gen, h.file, h.line, h.col, shown) : 0;
                h.col = real > 0 ? real : 1;
            }
            std::string location = h.file + ":" + std::to_string(h.line) + ":" + std::to_string(h.col);
            if (place == Place::Script && !note)
                primary_location = location;
            if (place == Place::Generated && note && !primary_location.empty())
                location = primary_location;
            out.push_back(location + ": " + h.level + ": " + rewrite_message(h.message, script_function[i]));
            switch (place)
            {
            case Place::Script:
            {
                auto it = script_lines.find(h.file);
                snippet_script = it == script_lines.end() ? nullptr : &it->second;
                snippet = snippet_script && !mapped ? Snippet::Replace : Snippet::Drop;
                break;
            }
            case Place::Generated: snippet = Snippet::Drop; break;
            default: snippet = Snippet::Keep; break;
            }
            continue;
        }

        int number = 0;
        size_t text_begin = 0;
        const bool gutter = snippet != Snippet::None && parse_gutter(l, number, text_begin);
        if (snippet == Snippet::Keep && gutter)
        {
            out.push_back(l);
            continue;
        }
        if (gutter)
        {
            // A numbered line is a source line. A line without a number is the caret line or a
            // fix-it line. Both show generated code.
            if (snippet == Snippet::Replace && number > 0 && snippet_script
                && static_cast<size_t>(number) <= snippet_script->size())
            {
                out.push_back(l.substr(0, text_begin) + (*snippet_script)[static_cast<size_t>(number) - 1]);
            }
            continue;
        }
        // Any other line: "In file included from ...", "1 error generated." and so on.
        snippet = Snippet::None;
        flush_hidden();
        out.push_back(l.rfind("In ", 0) == 0 ? rename_generated(l) : l);
    }
    flush_hidden();

    std::string result;
    for (size_t i = 0; i < out.size(); ++i)
    {
        result += out[i];
        if (i + 1 < out.size() || final_newline)
            result += '\n';
    }
    return result;
}

std::string rewrite_translator_diagnostic(const std::string& text, const ScriptTexts& scripts)
{
    bool final_newline = true;
    const std::vector<std::string> lines = split_lines(text, final_newline);
    std::map<std::string, std::vector<std::string>> script_lines;
    for (const auto& s : scripts)
    {
        bool nl = true;
        script_lines[s.first] = split_lines(s.second, nl);
    }

    std::vector<std::string> out;
    int errors = 0, warnings = 0;
    for (const std::string& l : lines)
    {
        Header h;
        const bool header = parse_header(l, h);
        if (!header && l.rfind("sppc: error: ", 0) == 0)
            ++errors;
        if (!header)
        {
            out.push_back(l);
            continue;
        }
        if (h.level == "warning")
            ++warnings;
        if (h.level == "error" || h.level == "fatal error")
            ++errors;
        out.push_back(l);
        auto it = script_lines.find(h.file);
        if (it == script_lines.end() || h.line < 1 || static_cast<size_t>(h.line) > it->second.size())
            continue;
        char gutter[32];
        std::snprintf(gutter, sizeof gutter, "%5d | ", h.line);
        out.push_back(gutter + it->second[static_cast<size_t>(h.line) - 1]);
    }
    auto plural = [](int n, const char* word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); };
    if (errors > 0 || warnings > 0)
    {
        std::string count;
        if (warnings > 0)
            count += plural(warnings, "warning");
        if (warnings > 0 && errors > 0)
            count += " and ";
        if (errors > 0)
            count += plural(errors, "error");
        out.push_back(count + " generated.");
    }
    std::string result;
    for (size_t i = 0; i < out.size(); ++i)
    {
        result += out[i];
        if (i + 1 < out.size() || final_newline)
            result += '\n';
    }
    return result;
}

std::string rewrite_generated_names(const std::string& text)
{
    return rename_generated(text);
}

std::string rewrite_clang_message(const std::string& message)
{
    return rewrite_message(message, has_script_function(message));
}

} // namespace sppc
