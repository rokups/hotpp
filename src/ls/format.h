// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The formatter of a script. It works on a token stream that keeps the comments and the literals.
// It never parses. It keeps the line breaks of the author and fixes the indent and the spacing.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hot::ls
{

struct FormatOptions
{
    int indent_width = 4; // the spaces of one indent level (ignored with use_tabs)
    bool use_tabs = false; // one tab for each level
};

// The result of a format with the map of the lines. "line_map" has one entry for each line of the
// input. The entry is the line of the output, or -1 for a line that the format removes (a blank
// line of a run). A range format uses the map.
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
