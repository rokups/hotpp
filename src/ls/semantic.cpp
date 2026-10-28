#include "semantic.h"

#include <map>
#include <set>

#include "builtin_types.h"

namespace spp::ls {

namespace {

enum Type { Keyword, TypeName, ClassName, Method, Property, Number, String, Comment };

// The keywords of the script. The list follows the TextMate grammar in editors/vscode.
const std::set<std::string>& keywords()
{
    static const std::set<std::string> k = {
        "if", "else", "while", "do", "for", "foreach", "in", "of", "return", "break", "continue", "switch",
        "case", "default", "is", "as", "new", "delete", "public", "private", "protected", "override",
        "virtual", "const", "ref", "out", "get", "set", "this", "super", "true", "false", "null", "try",
        "catch", "finally", "throw", "var", "let", "void", "class", "struct", "enum", "using", "static",
        "module", "native", "handle", "value"};
    return k;
}

}  // namespace

const std::vector<std::string>& semantic_token_types()
{
    static const std::vector<std::string> t = {"keyword", "type", "class", "method", "property",
                                               "number", "string", "comment"};
    return t;
}

std::vector<size_t> line_starts(const std::string& text)
{
    std::vector<size_t> r = {0};
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') r.push_back(i + 1);
    return r;
}

std::string_view line_text(const std::string& text, const std::vector<size_t>& starts, int line)
{
    if (line < 0 || static_cast<size_t>(line) >= starts.size()) return {};
    const size_t b = starts[static_cast<size_t>(line)];
    size_t e = static_cast<size_t>(line) + 1 < starts.size() ? starts[static_cast<size_t>(line) + 1] - 1 : text.size();
    if (e > b && text[e - 1] == '\r') --e;
    return std::string_view(text).substr(b, e - b);
}

int utf16_to_byte(std::string_view line, int col16)
{
    int units = 0;
    size_t i = 0;
    while (i < line.size() && units < col16) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += n == 4 ? 2 : 1;
        i += n;
    }
    if (units < col16) return static_cast<int>(line.size()) + (col16 - units);  // past the end: count bytes
    return static_cast<int>(i);
}

int byte_to_utf16(std::string_view line, int col)
{
    int units = 0;
    size_t i = 0;
    const size_t end = static_cast<size_t>(std::max(col, 0));
    while (i < line.size() && i < end) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += n == 4 ? 2 : 1;
        i += n;
    }
    if (end > line.size()) units += static_cast<int>(end - line.size());
    return units;
}

std::vector<SemanticToken> script_tokens(const std::string& text, const sppc::AnalyzeResult& facts)
{
    std::set<std::string> classes, methods, properties;
    for (const sppc::AnalyzedClass& c : facts.classes) {
        classes.insert(c.name);
        if (!c.base.empty()) classes.insert(c.base);
        for (const sppc::AnalyzedMember& m : c.members) {
            if (m.kind == "method") methods.insert(m.name);
            else if (m.kind == "field" || m.kind == "property") properties.insert(m.name);
        }
    }
    std::vector<SemanticToken> out;
    int line = 0;
    size_t line_begin = 0;
    // Adds a token from byte b to byte e. A token that holds a line break is split.
    auto add = [&](size_t b, size_t e, int type) {
        size_t s = b;
        for (size_t i = b; i < e; ++i) {
            if (text[i] != '\n') continue;
            if (i > s) out.push_back({line, static_cast<int>(s - line_begin), static_cast<int>(i - s), type});
            ++line;
            line_begin = i + 1;
            s = i + 1;
        }
        if (e > s) out.push_back({line, static_cast<int>(s - line_begin), static_cast<int>(e - s), type});
    };
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') {
            ++line;
            line_begin = ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) e = text.size();
            add(i, e, Comment);
            i = e;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            size_t e = text.find("*/", i + 2);
            e = e == std::string::npos ? text.size() : e + 2;
            add(i, e, Comment);
            i = e;
        } else if (c == '"' || c == '\'') {
            size_t e = i + 1;
            while (e < text.size() && text[e] != c && text[e] != '\n') e += text[e] == '\\' ? 2 : 1;
            e = std::min(e + 1, text.size());
            add(i, e, String);
            i = e;
        } else if (c >= '0' && c <= '9') {
            size_t e = i;
            while (e < text.size() && (is_id_char(text[e]) || text[e] == '.')) ++e;
            add(i, e, Number);
            i = e;
        } else if (is_id_char(c)) {
            size_t e = i;
            while (e < text.size() && is_id_char(text[e])) ++e;
            const std::string w = text.substr(i, e - i);
            int type = -1;
            if (keywords().count(w)) type = Keyword;
            else if (sppc::builtin_cpp_name(w)) type = TypeName;
            else if (classes.count(w)) type = ClassName;
            else if (methods.count(w)) type = Method;
            else if (properties.count(w)) type = Property;
            if (type >= 0) add(i, e, type);
            i = e;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace spp::ls
