// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hotc_lib: the translator as a library. The input and the output are strings.
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

#include "hot/vfs.h"

namespace hotc
{

struct TranslateOptions
{
    std::string script_text; // the content of the script
    std::string script_name; // the name for "#line" and for the diagnostics
    // The unit name of the script (doc/module-level.md, "Unit names"). Empty: the stem of script_name.
    // The imports get their unit names from it, and AOT code names its symbols with it.
    std::string unit_name;
    std::string output_name; // the name of the generated .cpp for "#line" (default: script_name + ".cpp")
    int generation = 0; // 1 or more
    std::string slots_in; // the content of the slot file (empty: no earlier generation)
    std::vector<std::string> include_dirs; // for the imports of the script
    std::string resource_dir; // the Clang resource directory (empty: the built-in default)
    // The target triple and the sysroot of the parse (-target, --sysroot). Empty: the built-in
    // default of the build (HOT_HOTC_TARGET, HOT_SCRIPT_SYSROOT), else the default of libclang.
    // A MinGW triple also adds the libstdc++ headers of the sysroot (hot::mingw_target_flags).
    std::string target;
    std::string sysroot;
    // The C++ standard for the parse of the native headers, for example "c++17". Empty: the
    // standard of the host (HOT_CXX_STANDARD in hot/config.h).
    std::string std_version;
    // The macro flags of the parse of the native headers ("-DNAME", "-DNAME=value", "-UNAME").
    // A backend gives the "-D" and "-U" flags of its compile, so the parse sees the same macros.
    std::vector<std::string> defines;
    std::vector<std::pair<std::string, std::string>> preludes; // name, text
    // The texts of the imported scripts ("using "parent.hot";"), as name and text. The name is
    // the path in the "using" line. A name that is not here is read from the directory of the
    // script, then from the include directories.
    std::vector<std::pair<std::string, std::string>> modules;
    // The borrowed-storage rule of design 7.10.4 and 7.10.6: a field, a return value, a
    // collection element or a lambda capture cannot hold a native "T*", a "T&" or a "ref" of a
    // type that is not a handle. True (the default): a violation is an error. False: it is a
    // warning (hotc --no-strict-borrowed).
    bool strict_borrowed = true;
    // The nullability analyzer (doc/type-design.md, "Nullability"). False (the default): a finding
    // is an error. True: it is a warning (hotc --null-warnings). The analyzer runs on ".hot"
    // scripts only.
    bool null_warnings = false;
    // The function for string interpolation "${expr}" (DECISIONS item 10): "std" lowers to
    // std::format and adds <format>. "fmt" lowers to fmt::format and adds <fmt/format.h>. The
    // user supplies the fmt header. Any other value is an error.
    std::string format_lib = "std";
    // The view type of "T[]" (hotc --view): "hot" lowers to hot::span<T> (C++17 or newer). "std"
    // lowers to std::span<T> and needs C++20. Any other value is an error.
    std::string view_type = "hot";
    // The file system of the host for the imports ("using "x.hot";" and "using "x.h";"). For each
    // directory the translator reads this file system first and then the disk. Null: the disk only.
    // The file system must be thread safe.
    std::shared_ptr<hot::FileSystem> fs;
    // AOT code (hotc --aot). Each thunk of a script function carries the signature key of its
    // slot, also the thunk of the first signature. Loader::revert_unit restores the keys from it.
    bool aot = false;
    // Optimized AOT code (hotc --aot-optimized). It needs aot. The generated file defines
    // HOT_AOT_OPTIMIZED. The library has no slots, so no override and no reload is possible.
    bool aot_optimized = false;
};

// The scripts of a build: pairs of the name that "#line" uses and the script text.
using ScriptTexts = std::vector<std::pair<std::string, std::string>>;

// A position in a text: a 1-based line and a 1-based byte column.
struct SourcePos
{
    int line = 0, col = 0;
    bool operator==(const SourcePos& o) const { return line == o.line && col == o.col; }
};

// One range of generated C++ that comes from one range of the script. The offsets are bytes.
// When the two lengths are equal, the text is the same in both places and a position maps
// byte by byte. When they differ (for example "int" and "int32_t"), any position in the
// range maps to the start of the other range.
//
// The "file" is 0 for the main script. For an imported script ("using "x.hot";") it is the number
// of the file in SourceMap::files() (the first file is 1). The script offsets are then offsets in
// the text of that file.
struct SourceSegment
{
    uint32_t script_begin = 0, script_len = 0;
    uint32_t gen_begin = 0, gen_len = 0;
    uint32_t file = 0;
};

// A place in the main script or in an imported script. The "file" has the same meaning as in
// SourceSegment: 0 is the main script.
struct SourceLocation
{
    uint32_t file = 0;
    SourcePos pos;
};

// An imported script that the source map knows: the name that "#line" uses and the text.
struct SourceFile
{
    std::string name, text;
};

// The map between the script text and the generated C++ text (design 10.1, M10). The map covers
// the bodies of the methods, the constructors, the destructors and the property accessors. It
// also covers the names in the head of a script function (the function name and the parameter
// names) and the field names in the state struct.
// Text that the translator writes by itself (for example "hot::deref(" or the class shell)
// has no segment: the functions return no value for it.
class SourceMap
{
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

    const std::vector<SourceSegment>& segments() const { return segments_; } // in generated order
    bool empty() const { return segments_.empty(); }

    // The instantiations of the script generics: the C++ name and the script name, for example
    // "Box_G_int" and "Box<int>". rewrite_diagnostic() shows the script name in a message.
    using GenericNames = std::vector<std::pair<std::string, std::string>>;
    void set_generic_names(GenericNames names) { generic_names_ = std::move(names); }
    const GenericNames& generic_names() const { return generic_names_; }

private:
    GenericNames generic_names_;
    std::vector<SourceSegment> segments_;
    std::vector<SourceFile> files_;
    std::vector<uint32_t> by_script_; // indexes into segments_, sorted by file and script_begin
    // The byte offset of each line start: for the main script and for each imported script.
    std::vector<std::vector<uint32_t>> script_lines_;
    std::vector<uint32_t> gen_lines_;
};

struct TranslateResult
{
    bool ok = false;
    std::string cpp; // the generated C++ text (valid when ok)
    std::string slots_out; // the new slot file content (valid when ok)
    // The signature lines of the slot file (design 5.3): "sig Cls::fn Sig", one for each script
    // function. A backend writes them after "slots_out" and gives both back in "slots_in". A
    // reload that changes a signature then gets a new thunk with a signature key. After the sig
    // lines come the lines "field Cls::name type", one for each field. The next translation
    // reads them to warn about a field rename. A reader of the slot text skips these lines.
    std::string sigs_out;
    std::string diagnostics; // the errors, one line each: "name:line:col: error: text"
    // Always false. A removed function keeps a defunct slot. The field stays for API compatibility.
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
    // The native headers that the parse read from TranslateOptions::fs, as path and text. The
    // headers that these include, and that the host file system holds, are here too.
    std::vector<std::pair<std::string, std::string>> virtual_files;
    // The files that the translation read: the native headers and the imported scripts. The system
    // headers (the standard library, for example) are not here. A build
    // system or a host uses them to know when to translate again. There is no duplicate. The
    // order is stable. The list is valid when ok is true.
    std::vector<std::string> dependencies;
    // The interface of the module (the script): the text of the declarations that importers
    // see. The names with "private" are not in it. A body change does not change it.
    std::string iface;
    // The FNV-1a hash of "iface". It is valid when ok is true.
    uint64_t iface_hash = 0;
    // The script imports as unit name and interface hash: the direct imports and their imports
    // (the importer copies their classes and structs). Each unit is in the list one time. The unit name of an import follows
    // the rule in doc/module-level.md. The hash is the "iface_hash" of the imported script.
    std::vector<std::pair<std::string, uint64_t>> import_hashes;
    // The unit name of the script: TranslateOptions::unit_name, or the stem of the script name.
    std::string unit;
    // AOT code only (TranslateOptions::aot): the C++ text of "<stem>.iface.h" (the stem of the script file). It holds the shell
    // and the state struct of each public class, as copies of the text in "cpp". The importers
    // include it. The TU in "cpp" does not include it, so each TU has one definition of a class.
    std::string iface_header;
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
struct AnalyzedMember
{
    std::string kind; // "field", "method", "constructor", "destructor" or "property"
    std::string name;
    // The begin is the start of the name. The end is after the closing brace of a body, after
    // the initializer of a field or a property, or else after the name.
    SourcePos begin, end;
    // The previous name from [alias("...")]. Only a field has it. It is empty with no alias.
    std::string alias;
};

struct AnalyzedClass
{
    std::string name, base;
    // True for a script interface. Its members are methods and its base is empty.
    bool is_interface = false;
    SourcePos begin;
    std::vector<AnalyzedMember> members; // in source order
};

struct AnalyzeError
{
    SourcePos pos;
    uint32_t offset = 0; // the byte offset in the script
    std::string message;
    bool note = false; // a note of the error before it, not an error
};

struct AnalyzeResult
{
    bool ok = false; // no syntax error, and the translation is ok
    std::vector<AnalyzedClass> classes; // the classes that the parser read
    std::vector<AnalyzeError> errors; // the syntax errors, in source order
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
class TranslatorSession
{
public:
    TranslatorSession();
    ~TranslatorSession();
    TranslatorSession(const TranslatorSession&) = delete;
    TranslatorSession& operator=(const TranslatorSession&) = delete;

    int parse_count() const; // the number of libclang parses until now
    void clear(); // drops the cache

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

// Changes each C++ name of a generic instantiation in "text" to its script name: "Box_G_int"
// becomes "Box<int>". Only a full name changes: "Box_G_int2" and "Box_G_int__Get" stay.
std::string show_generic_names(std::string text, const SourceMap::GenericNames& names);

// Changes the generated names in one text to script names, as rewrite_diagnostic() does for a
// message: "Player__heal" -> "Player.heal", "int32_t" -> "int", "std::string" -> "std.string".
// A language server uses it for hover text and completion labels.
std::string rewrite_generated_names(const std::string& text);

// Rewrites one Clang message (no location, no source line) as rewrite_diagnostic() does. The
// common mistakes get a plain message. A language server uses it for the clangd diagnostics.
std::string rewrite_clang_message(const std::string& message);

// Uses a temporary session: each call parses the headers.
TranslateResult translate(const TranslateOptions& options);

// The names of the built-in attributes of a stub, a class definition and a module attribute list,
// for example "native", "weak_handle", "weak_source" and "cpp_overrides". A script attribute tag is not in the list.
const std::vector<std::string>& class_attribute_names();

// The libclang arguments for the target of the parse: "-target", "--sysroot=" and, for a MinGW
// triple, "-nostdinc++" and the "-isystem" folders of libstdc++. An empty target or sysroot uses
// the default of the build (HOT_HOTC_TARGET, HOT_SCRIPT_SYSROOT). The translator uses the same
// arguments. A caller that runs libclang directly (for example a test) adds them too.
std::vector<std::string> target_parse_args(const std::string& target, const std::string& sysroot);

// The error text when the target and the sysroot of the parse (with the same defaults as
// target_parse_args) cannot work: a MinGW target with a sysroot that has no C++ standard library
// headers (hot::mingw_sysroot_error). Empty when they can work.
std::string target_parse_error(const std::string& target, const std::string& sysroot);

// Uses the cache of the session.
TranslateResult translate(const TranslateOptions& options, TranslatorSession& session);

// Reads "text" with the tolerant parser and returns the facts. The text replaces
// options.script_text. The function does not change the result of translate(): a script
// with a syntax error still gives ok = false and no C++ there.
AnalyzeResult analyze(const std::string& text, const TranslateOptions& options);
AnalyzeResult analyze(const std::string& text, const TranslateOptions& options, TranslatorSession& session);

// Checks a ".cpp" script (see doc/cpp-scripts.md) and returns its errors. The function runs the
// ".cpp" front end and all the checks of translate(). It writes no file. "path" is the script
// name: it must end in ".cpp" and it replaces options.script_name. The text replaces
// options.script_text.
//   - ok:          the script translates with no error.
//   - errors:      the errors that have a place in "path", in source order. An error with no
//                  script line gets the place 1:1. The "offset" is the byte offset of the place.
//   - diagnostics: all the error lines of the translator, as in TranslateResult.
// The fields classes, completion_points, source_map and cpp stay empty: the editor gets the
// other features for a ".cpp" file from another server.
AnalyzeResult analyze_cpp(const std::string& text, const std::string& path, const TranslateOptions& options);
AnalyzeResult analyze_cpp(
    const std::string& text, const std::string& path, const TranslateOptions& options, TranslatorSession& session);

} // namespace hotc
