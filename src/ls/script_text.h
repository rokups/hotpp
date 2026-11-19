// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Pure helpers for the text and the facts of one script. They read no Server state. The server
// and the workspace index (workspace_index.h) both use them.
#pragma once

#include "hot/hotc_lib.h"
#include "ref_resolver.h"
#include "symbol_index.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace hot::ls
{

// A text with its line table. Positions are 0-based lines and byte columns.
struct Text
{
    std::string s;
    std::vector<size_t> starts;
    Text();
    explicit Text(std::string t);
    std::string_view line(int l) const;
    size_t offset(int l, int c) const;
    std::pair<int, int> pos(size_t off) const;
    // HOT-65: the unit text of a markup document (a ".rml" file). The unit text has a prefix in front of
    // line 0 and has no markup. "shift0" is the byte length of the prefix. "client" is the raw text that the
    // editor shows. The columns of the unit text are byte columns of the raw text, with "shift0" more on line 0.
    // The server converts the LSP columns on the raw text (Server::from_lsp and Server::to_lsp). A text
    // that is not a unit text has shift0 0 and no client.
    int shift0 = 0;
    std::shared_ptr<const Text> client;
};

struct Pos
{
    int line = 0, col = 0; // 0-based line, byte column
    bool operator<(const Pos& o) const { return std::tie(line, col) < std::tie(o.line, o.col); }
    bool operator==(const Pos& o) const { return line == o.line && col == o.col; }
};

// Replaces the text of the string literals, the character literals and the comments of a line by
// spaces. "in_block" is true inside a block comment that goes on to the next line.
std::string blank_non_code(std::string_view l, bool& in_block);

// HOT-51: true when the class facts of "analyzed" (the text that "facts" come from) still hold for "text".
// This is the case when the two texts differ only inside the body of one method, one constructor or one
// destructor, in one line, and the changed bytes are plain: words, spaces and the brackets "( ) [ ]" and
// the operator signs, but no brace, ";", quote, comment sign, "#" or newline. Then the classes, their bases and
// their member names are the same in both texts, and the line count is the same. The completion reads only
// these facts.
bool class_facts_hold(const Text& analyzed, const std::string& text, const hotc::AnalyzeResult& facts);

// The columns of the whole-word matches of "w" in a line.
std::vector<int> whole_word_cols(std::string_view l, const std::string& w);

// The position of the name of the class "c" in the script text "t".
Pos class_name_pos_in(const Text& t, const hotc::AnalyzedClass& c);

// The places of the class name "name": the semantic tokens of the facts that mark it.
std::set<Pos> class_places_in(const Text& t, const hotc::AnalyzeResult& facts, const std::string& name);

// True when each code word "word" in "text" is an exact resolved ref of the table. A word that
// the table does not claim (an unresolved name, a native type, a raw body word) gives false.
bool table_lacks_word_in(const FileRefs& table, const std::string& text, const std::string& word);

// Every word of the text, and the code words that have one or more occurrences which are not an
// exact resolved ref (the same rule as table_lacks_word_in).
void collect_words(const FileRefs& table, const std::string& text, std::set<std::string>& words,
    std::set<std::string>& unclaimed);

// The script imports of "text": the ref file id of each "using "x.hot";" that the search finds.
// "options" has the file system of the search (null: the disk only). "path" is the script path.
std::vector<std::string> script_imports_in(
    const hotc::TranslateOptions& options, const std::string& path, const std::string& text);

// The table of the resolved names of the analyzed script "path".
FileRefs table_of_facts(const std::string& path, const hotc::AnalyzeResult& facts, std::vector<std::string> imports);

// The shard (symbols and table) of the analyzed script.
IndexShard shard_of_facts(
    const std::string& path, const Text& text, const hotc::AnalyzeResult& facts, std::vector<std::string> imports);

} // namespace hot::ls
