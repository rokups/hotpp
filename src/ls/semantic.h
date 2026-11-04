// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The script facts that the server makes without clangd: semantic tokens and text helpers.
#pragma once

#include "hot/hotc_lib.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hot::ls
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
std::vector<SemanticToken> script_tokens(const std::string& text, const hotc::AnalyzeResult& facts);

// A parameter-name hint: "label" (the name and ":") goes before the byte offset "off".
struct ParamHint
{
    size_t off = 0;
    std::string label;
};

// The hints for the arguments of the calls of script methods and constructors. The callee is a
// member in the facts of analyze(), found by its name. A call with the name of more than one
// member gets a hint only for an argument where all the members that fit have the same
// parameter name. An argument that is the plain name of its parameter gets no hint.
std::vector<ParamHint> script_param_hints(const std::string& text, const hotc::AnalyzeResult& facts);

// A call whose name is not a method or a constructor of the facts: a native function, method or
// constructor, or a free function of the script. The arguments are the text between the brackets,
// split at the commas of depth 0. A declaration (a word or "~" before the name) is not a call.
struct CallArg
{
    size_t begin = 0; // the byte offset of the first token of the argument
    std::string plain; // the name when the argument is one identifier, else empty
    bool empty = false; // no tokens: "f(a,,b)" or a trailing comma
};

struct CallSite
{
    size_t name_off = 0, close_off = 0; // the byte offsets of the name and of the ")"
    std::string name;
    std::vector<CallArg> args;
};

std::vector<CallSite> script_unknown_calls(const std::string& text, const hotc::AnalyzeResult& facts);

// A use of a name in a script: an identifier that is not a keyword. The name in the declaration
// of a class or of a member is not a use. The name in a destructor ("~Name") is a declaration too. "call" is set when "(" follows the name. The scan reads
// the tokens only. It does not know the type of an object, so the same name in two classes (or
// a local with the name of a method) is the same name. The references, the code lens and the
// call hierarchy use this scan.
struct NameUse
{
    size_t off = 0; // the byte offset of the name
    std::string name;
    bool call = false;
};

// The uses of all the names, in source order.
std::vector<NameUse> script_name_uses(const std::string& text, const hotc::AnalyzeResult& facts);

// The keywords of the script, and the names of its builtin types (with the aliases).
const std::vector<std::string>& script_keywords();
// The global helper functions that every script has: "enumerate" and "range". They are not keywords.
const std::vector<std::string>& script_helpers();
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
    std::vector<std::string> interfaces; // the script interfaces
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

} // namespace hot::ls
