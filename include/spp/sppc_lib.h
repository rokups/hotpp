// sppc_lib: the translator as a library. The input and the output are strings.
//
// translate() does not write a file and does not read a script or a slot file.
// libclang reads the headers that the script imports, from the include dirs.
// The function is thread safe when each thread uses its own call.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sppc {

struct TranslateOptions {
    std::string script_text;   // the content of the script
    std::string script_name;   // the name for "#line" and for the diagnostics
    std::string output_name;   // the name of the generated .cpp for "#line" (default: script_name + ".cpp")
    int generation = 0;        // 1 or more
    std::string slots_in;      // the content of the slot file (empty: no earlier generation)
    std::vector<std::string> include_dirs;  // for the imports of the script
    std::string resource_dir;  // the Clang resource directory (empty: the built-in default)
    std::vector<std::pair<std::string, std::string>> preludes;  // name, text
    // The texts of the imported scripts ("using "parent.spp";"), as name and text. The name is
    // the path in the "using" line. A name that is not here is read from the directory of the
    // script, then from the include directories.
    std::vector<std::pair<std::string, std::string>> modules;
    // The borrowed-storage rule of design 7.10.4 and 7.10.6: a field, a return value, a
    // collection element or a lambda capture cannot hold a native "T*", a "T&" or a "ref" of a
    // type that is not a handle. True (the default): a violation is an error. False: it is a
    // warning (sppc --no-strict-borrowed).
    bool strict_borrowed = true;
    // The function for string interpolation "${expr}" (DECISIONS item 10): "std" lowers to
    // std::format and adds <format>. "fmt" lowers to fmt::format and adds <fmt/format.h>. The
    // user supplies the fmt header. Any other value is an error.
    std::string format_lib = "std";
};

// The scripts of a build: pairs of the name that "#line" uses and the script text.
using ScriptTexts = std::vector<std::pair<std::string, std::string>>;

// A position in a text: a 1-based line and a 1-based byte column.
struct SourcePos {
    int line = 0, col = 0;
    bool operator==(const SourcePos& o) const { return line == o.line && col == o.col; }
};

// One range of generated C++ that comes from one range of the script. The offsets are bytes.
// When the two lengths are equal, the text is the same in both places and a position maps
// byte by byte. When they differ (for example "int" and "int32_t"), any position in the
// range maps to the start of the other range.
//
// The "file" is 0 for the main script. For an imported script ("using "x.spp";") it is the number
// of the file in SourceMap::files() (the first file is 1). The script offsets are then offsets in
// the text of that file.
struct SourceSegment {
    uint32_t script_begin = 0, script_len = 0;
    uint32_t gen_begin = 0, gen_len = 0;
    uint32_t file = 0;
};

// A place in the main script or in an imported script. The "file" has the same meaning as in
// SourceSegment: 0 is the main script.
struct SourceLocation {
    uint32_t file = 0;
    SourcePos pos;
};

// An imported script that the source map knows: the name that "#line" uses and the text.
struct SourceFile {
    std::string name, text;
};

// The map between the script text and the generated C++ text (design 10.1, M10). The map covers
// the bodies of the methods, the constructors, the destructors and the property accessors. It
// also covers the names in the head of a script function (the function name and the parameter
// names) and the field names in the state struct.
// Text that the translator writes by itself (for example "spp::deref(" or the class shell)
// has no segment: the functions return no value for it.
class SourceMap {
public:
    // The sorted tables are built here. Call it after the last add() and add_file().
    void build(std::string_view script, std::string_view generated);
    void add(const SourceSegment& s) { segments_.push_back(s); }
    // Adds an imported script. Returns its file number (1 or more). Call it before build().
    uint32_t add_file(std::string name, std::string text)
    {
        files_.push_back({std::move(name), std::move(text)});
        return static_cast<uint32_t>(files_.size());
    }

    // A position in the generated text gives the position in the main script, or no value. A
    // position that comes from an imported script gives no value (use locate()).
    std::optional<SourcePos> map_to_script(int gen_line, int gen_col) const;
    // A position in the generated text gives the file and the position, or no value. The file
    // is 0 for the main script, else the number in files().
    std::optional<SourceLocation> locate(int gen_line, int gen_col) const;
    // A position in the main script gives the position in the generated text, or no value.
    std::optional<SourcePos> map_to_generated(int script_line, int script_col) const;
    // The same for a file: 0 is the main script.
    std::optional<SourcePos> map_to_generated(uint32_t file, int script_line, int script_col) const;

    // The imported scripts. The file number n is files()[n - 1].
    const std::vector<SourceFile>& files() const { return files_; }

    const std::vector<SourceSegment>& segments() const { return segments_; }  // in generated order
    bool empty() const { return segments_.empty(); }

private:
    std::vector<SourceSegment> segments_;
    std::vector<SourceFile> files_;
    std::vector<uint32_t> by_script_;  // indexes into segments_, sorted by file and script_begin
    // The byte offset of each line start: for the main script and for each imported script.
    std::vector<std::vector<uint32_t>> script_lines_;
    std::vector<uint32_t> gen_lines_;
};

struct TranslateResult {
    bool ok = false;
    std::string cpp;          // the generated C++ text (valid when ok)
    std::string slots_out;    // the new slot file content (valid when ok)
    // The signature lines of the slot file (design 5.3): "sig Cls::fn Sig", one for each script
    // function. A backend writes them after "slots_out" and gives both back in "slots_in". A
    // reload that changes a signature then gets a new thunk with a signature key.
    std::string sigs_out;
    std::string diagnostics;  // the errors, one line each: "name:line:col: error: text"
    // True when the translation failed because a function of an earlier generation is removed
    // ("the slot count must not shrink"). Hot reload cannot apply this change: only a full
    // reload can. A backend maps it to ReloadReason::SlotsRemoved.
    bool slots_removed = false;
    // The warnings, one line each: "name:line:col: warning: text". The translation can be ok
    // with warnings. The text is empty when there is no warning.
    std::string warnings;
    // The map between the generated C++ and the script. It is valid when ok.
    SourceMap source_map;
    // The imported scripts that the translator read, as name and text. The name is the one that
    // the diagnostics and "#line" use. It is valid also when ok is false: a diagnostic
    // rewriter needs the text of an imported script to show its source line.
    ScriptTexts imports;
};

// The main script and the imported scripts of a translation, as input for the rewriters.
inline ScriptTexts script_texts(const std::string& name, const std::string& text, const TranslateResult& r)
{
    ScriptTexts all{{name, text}};
    all.insert(all.end(), r.imports.begin(), r.imports.end());
    return all;
}

// The facts of a script that has errors (design 10.1, M10). A language server calls analyze().
//
// The parser is tolerant. A broken statement or member is skipped to the next ";" or "}" and the
// parse goes on, so one error does not hide the others. The facts hold what the parser read.
struct AnalyzedMember {
    std::string kind;  // "field", "method", "constructor", "destructor" or "property"
    std::string name;
    // The begin is the start of the name. The end is after the closing brace of a body, after
    // the initializer of a field or a property, or else after the name.
    SourcePos begin, end;
};

struct AnalyzedClass {
    std::string name, base;
    SourcePos begin;
    std::vector<AnalyzedMember> members;  // in source order
};

struct AnalyzeError {
    SourcePos pos;
    uint32_t offset = 0;  // the byte offset in the script
    std::string message;
};

struct AnalyzeResult {
    bool ok = false;  // no syntax error, and the translation is ok
    std::vector<AnalyzedClass> classes;  // the classes that the parser read
    std::vector<AnalyzeError> errors;    // the syntax errors, in source order
    // A "." with no member name after it ("player." at the end of a line). The position is
    // right after the ".". A completion request there lists the members of the object.
    std::vector<SourcePos> completion_points;
    // The translation errors of a script with no syntax error, as in TranslateResult.
    std::string diagnostics;
    // The map for the valid parts. A member with a syntax error is left out of the translation.
    // The map is empty when the rest does not translate (for example a type error).
    SourceMap source_map;
    // The generated C++ of the valid parts. The map belongs to this text. It is empty when the
    // map is empty. A language server gives this text to clangd.
    std::string cpp;
};

// The cache of a translator session. A backend holds one for its life.
//
// libclang needs about 400 ms to parse the engine headers. The session keeps the
// result of that parse: the virtual methods of each native base class. The key is the
// include flags, the current directory and the imports. The session also keeps the
// path, the change time and the size of each file that the parse included. A call
// checks them again. If one differs, the session parses again. A session is thread
// safe. The calls that use the same session wait for each other during a parse.
class TranslatorSession {
public:
    TranslatorSession();
    ~TranslatorSession();
    TranslatorSession(const TranslatorSession&) = delete;
    TranslatorSession& operator=(const TranslatorSession&) = delete;

    int parse_count() const;  // the number of libclang parses until now
    void clear();             // drops the cache

    struct Impl;

private:
    friend TranslateResult translate(const TranslateOptions&, TranslatorSession&);
    std::unique_ptr<Impl> impl_;
};


// Rewrites the text that Clang prints for the generated C++ so that it speaks script names
// (design 4.2, experiment F2). The function is pure: it has no state and no I/O.
//   - A generated name becomes a script name: "Player_Shell" -> "Player", "Player__heal" ->
//     "Player.heal", "int32_t" -> "int", "std::string" -> "std.string".
//   - The argument counts of a script method do not count the hidden "self" argument.
//   - A common mistake gets a plain message (the table is in diag_rewrite.cpp).
//   - A source line from the generated code is not shown. If "scripts" has the text of the
//     script, the line of the script replaces it. The caret line is dropped.
//   - The map also holds the imported scripts (SourceMap::files()). A location in an imported
//     script gets its own file, line, column and source line.
//   - The column of a script location is 1, unless the caller gives "map". Then the rewriter
//     finds the generated line of the location (from the "#line" directives in the first text
//     of "generated"), maps the generated column with the source map, and prints the script
//     column. A column that the map cannot give stays 1.
//   - A note that points into the generated file gets the location of its error. An error that
//     points into the generated file gets the last script line before it, if "generated" has the
//     text of that file (pairs of the "#line" name of the file and its text). A note from a
//     system header is hidden, and one line says how many.
// A line that is not part of a Clang diagnostic stays as it is. Pass the raw text on when the
// user asks for it (--raw-diagnostics).
std::string rewrite_diagnostic(const std::string& text, const ScriptTexts& scripts = {},
                               const ScriptTexts& generated = {}, const SourceMap* map = nullptr);

// Gives the text of the translator diagnostics (TranslateResult::diagnostics or ::warnings) the
// same final form as rewrite_diagnostic(): the header line "script:line:col: level: message",
// then the script line in the gutter form ("   11 | code"), and at the end the count line
// ("1 error generated."). A line that is not a diagnostic of a script in "scripts" stays as it
// is. The message and the position do not change: the translator already speaks script names
// and gives the real script column. Pass the raw text on when the user asks for it
// (--raw-diagnostics).
std::string rewrite_translator_diagnostic(const std::string& text, const ScriptTexts& scripts);

// Changes the generated names in one text to script names, as rewrite_diagnostic() does for a
// message: "Player__heal" -> "Player.heal", "int32_t" -> "int", "std::string" -> "std.string".
// A language server uses it for hover text and completion labels.
std::string rewrite_generated_names(const std::string& text);

// Rewrites one Clang message (no location, no source line) as rewrite_diagnostic() does. The
// common mistakes get a plain message. A language server uses it for the clangd diagnostics.
std::string rewrite_clang_message(const std::string& message);

// Uses a temporary session: each call parses the headers.
TranslateResult translate(const TranslateOptions& options);

// Uses the cache of the session.
TranslateResult translate(const TranslateOptions& options, TranslatorSession& session);

// Reads "text" with the tolerant parser and returns the facts. The text replaces
// options.script_text. The function does not change the result of translate(): a script
// with a syntax error still gives ok = false and no C++ there.
AnalyzeResult analyze(const std::string& text, const TranslateOptions& options);
AnalyzeResult analyze(const std::string& text, const TranslateOptions& options, TranslatorSession& session);

}  // namespace sppc
