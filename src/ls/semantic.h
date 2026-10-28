// The script facts that the server makes without clangd: semantic tokens and text helpers.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "spp/sppc_lib.h"

namespace spp::ls {

// The token types of the semantic token legend, in the order of their index.
const std::vector<std::string>& semantic_token_types();

// One token on one line. "line" is 0-based. "col" and "len" are bytes.
struct SemanticToken {
    int line = 0, col = 0, len = 0, type = 0;
};

// The tokens of a script: keywords, types, classes, methods, properties, numbers, strings and
// comments. The names come from the facts of analyze(). A token never spans two lines.
std::vector<SemanticToken> script_tokens(const std::string& text, const sppc::AnalyzeResult& facts);

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

}  // namespace spp::ls
