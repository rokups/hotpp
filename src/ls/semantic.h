// The script facts that the server makes without clangd: semantic tokens and text helpers.
#pragma once

#include "spp/sppc_lib.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace spp::ls
{

// The token types of the semantic token legend, in the order of their index.
const std::vector<std::string>& semantic_token_types();

// One token on one line. "line" is 0-based. "col" and "len" are bytes.
struct SemanticToken
{
    int line = 0, col = 0, len = 0, type = 0;
};

// The tokens of a script: keywords, types, classes, methods, properties, numbers, strings and
// comments. The names come from the facts of analyze(). A token never spans two lines.
std::vector<SemanticToken> script_tokens(const std::string& text, const sppc::AnalyzeResult& facts);

// The keywords of the script, and the names of its builtin types (with the aliases).
const std::vector<std::string>& script_keywords();
const std::vector<std::string>& builtin_type_names();

// A name that the script declares, for a completion list.
struct ScopeName
{
    std::string name;
    std::string type; // the declared type as written, or empty
    std::string detail; // "local", "parameter" or "module variable"
};

// The script names that a completion at one place can use. The scan reads the tokens only: it
// works on a script with syntax errors.
struct ScriptScope
{
    std::vector<ScopeName> locals; // locals and parameters in scope
    std::vector<ScopeName> module_vars; // the variables at module level
    std::vector<std::string> enums; // the script enums
    std::map<std::string, std::vector<std::string>> enum_members; // the enumerators of each enum
    std::vector<std::string> classes, structs; // the script classes and structs
};

// Scans "text". The locals are the ones in scope at the byte offset "at". The word that starts at
// "typed_begin" is the word that the user types: it is not a declaration.
ScriptScope scan_scope(const std::string& text, size_t at, size_t typed_begin);

// The byte offset of the start of each line.
std::vector<size_t> line_starts(const std::string& text);
// The text of a 0-based line without the line break. An empty view for a line that does not exist.
std::string_view line_text(const std::string& text, const std::vector<size_t>& starts, int line);
// Converts a column in UTF-16 code units to a byte column on one line, and back.
int utf16_to_byte(std::string_view line, int col16);
int byte_to_utf16(std::string_view line, int col);

inline bool is_id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

} // namespace spp::ls
