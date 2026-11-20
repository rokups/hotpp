// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The formatter of a script. It works on a token stream that keeps the comments and the literals.
// It never parses. It keeps the line breaks of the author and fixes the indent and the spacing.
// Only a brace style other than Keep moves the "{" of a block (and "else", "catch", "finally").
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hot::ls
{

// The place of the "{" of a block: as the author put it, at the end of the head line, or on the
// next line. A "{" moves only after ")", ">" or a word, outside "(" and "[", when its block has
// more than one line. A "{" after "=>", "=", ",", ":" or another bracket stays.
enum class BraceStyle
{
    Keep,
    SameLine,
    NextLine
};

struct FormatOptions
{
    int indent_width = 4; // the spaces of one indent level (ignored with use_tabs)
    bool use_tabs = false; // one tab for each level
    BraceStyle brace = BraceStyle::Keep;
    // A brace moves only when all the input lines of the move are in this range (-1: to the end).
    // A range format sets them, so the lines outside the range keep their line breaks.
    int first_line = 0;
    int last_line = -1;
};

// The result of a format with the map of the lines. "line_map" has one entry for each line of the
// input. The entry is the line of the output, or -1 for a line that the format removes (a blank
// line of a run, or a line that a moved "{" joins to the line before). A range format uses the map.
struct FormatResult
{
    std::string text;
    std::vector<int> line_map;
};

FormatResult format_hot_map(std::string_view text, const FormatOptions& options = {});

// The tokens of the text, as the formatter reads them: the exact text of each word, operator,
// literal and comment. The format changes white space only, so the list is the same before and
// after a format. The tests use it.
std::vector<std::string> format_tokens(std::string_view text);

// The formatted text. The function is idempotent and it keeps all the tokens of the input.
std::string format_hot(std::string_view text, const FormatOptions& options = {});

} // namespace hot::ls
