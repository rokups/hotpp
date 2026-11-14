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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hot/iface_compat.h"
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
    // The module exports its generation as the symbol "hot_module_generation" (module_from_library
    // reads it). Only the tool hotc sets it, for a module with an explicit "--gen N". The backends
    // leave it false: then the text has no generation and a later generation hits the object cache.
    // AOT code ignores it.
    bool export_generation = false;
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
    // The table of the resolved names (TranslateResult::refs, AnalyzeResult::refs). False (the
    // default): the translator makes no table. A language server sets it for its analyze calls.
    bool resolve_refs = false;
};

// The scripts of a build: pairs of the name that "#line" uses and the script text.
using ScriptTexts = std::vector<std::pair<std::string, std::string>>;

// A position in a text: a 1-based line and a 1-based byte column.
struct SourcePos
{
    int line = 0, col = 0;
    bool operator==(const SourcePos& o) const { return line == o.line && col == o.col; }
};

// The kind of the symbol that a resolved name refers to.
enum class RefKind
{
    Local, // a local variable, a foreach variable, a catch variable or an "is T name" binding
    Param, // a parameter of a function or of a lambda
    Field, // a field of a script class
    Property, // a property of a script class
    Method, // a method of a script class or of a script interface
    StaticMethod, // a static method of a script class
    StaticField, // a static field of a script class
    Const, // a class constant or a module constant
    ModuleVar, // a module variable
    Function, // a free function
    Class, // a script class or a script struct
    Interface, // a script interface
    Enum, // a script enum
    EnumValue, // a value of a script enum
};

// One name in the main script and the declaration that the translator resolved it to. The
// translator makes the table only when TranslateOptions::resolve_refs is true.
//
// The "decl" is the identity of the declaration. Two refs with the same "decl" name the same
// symbol. The forms are:
//   - a local or a parameter: "@" and the byte offset of its declaration name ("@120"). Two
//     locals with the same name in different scopes thus have different ids.
//   - a member of a class or an interface (also a static member and a class constant):
//     "Class.member". The member is in the class that declares it. An instance method has the
//     id of its root declaration: the highest declaration in the script base classes, or the
//     method of a script interface. An override and its root thus have one id.
//   - a class, an interface or an enum: "Name". An enum value: "Enum.Value".
//   - a free function, a module variable or a module constant: "Name".
// "decl_file" is the normal path of the script that declares the symbol (hotc::ref_file_id). The
// main script also has its path here. "id" is "decl_file", "#" and "decl". The id is stable over
// the files: the ref in an importer and the declaration in the imported script have the same id.
// A name of the native code has no ref.
struct ResolvedRef
{
    uint32_t offset = 0; // the byte offset of the name in the main script
    uint32_t length = 0; // the byte length of the name
    SourcePos pos; // the line and the column of "offset"
    RefKind kind = RefKind::Local;
    bool is_decl = false; // true at the declaration itself
    std::string decl;
    std::string decl_file;
    std::string id; // decl_file + "#" + decl
};

// The file part of a ResolvedRef::id: the absolute, lexically normal path with "/" separators.
// It does not read the file system. Thus a symbolic link gives a different path.
std::string ref_file_id(const std::string& path);

// A name that the translator cannot resolve. It is one of these:
//   - a bare name that is not a local and matches no script symbol (for example a native function);
//   - a member name after "." with a receiver of no known script type (for example a native value).
// The name is perhaps native. A language server can ask clangd about it.
struct UnresolvedName
{
    uint32_t offset = 0, length = 0; // the bytes of the name in the main script
    SourcePos pos; // the line and the column of "offset"
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
    // For each entry of by_script_: the largest script end of the entries of the same file up to it.
    std::vector<uint32_t> max_end_;
    // The byte offset of each line start: for the main script and for each imported script.
    std::vector<std::vector<uint32_t>> script_lines_;
    std::vector<uint32_t> gen_lines_;
};

// One search for an imported script (hotc::find_import): the arguments, the file that the search
// found and the hash of its text (hotc::text_hash). A translation cache repeats the search.
struct ImportLookup
{
    std::string path, from_dir, from_unit_dir;
    std::string file;
    uint64_t text_hash = 0;
    // The import view of the text (hotc::import_view): no value when the translation did not parse
    // the file as an import. "array_tpl" is the array template of the parse. "view_masked" has
    // the masked byte ranges of the text as begin and end pairs.
    std::optional<uint64_t> view_hash;
    std::string array_tpl;
    std::vector<uint32_t> view_masked;
};

// The import view of an imported script: all the parts of its text that the output of an
// importer can use. The bodies of the class methods, constructors, destructors, accessors and
// free functions are masked: an importer gets no copy of them. Each part that keeps a position
// of the import in the importer output also has its position in the view. A translation cache
// can thus use an entry after a body edit of an import when the view hash stays the same.
struct ImportView
{
    uint64_t hash = 0;
    std::vector<uint32_t> masked; // the masked byte ranges: begin and end pairs, in text order
};

// The view of "text", parsed with the array template "array_tpl" of the importer. No value when
// the parse fails. The diagnostics of the parse are not kept.
std::optional<ImportView> import_view(const std::string& text, const std::string& array_tpl);
std::optional<uint64_t> import_view_hash(const std::string& text, const std::string& array_tpl);

// The declarations of an imported script that a language server offers in completion. The parse is
// tolerant and nothing is translated. A private class, enum, function, constant or variable is not
// in the facts. The constants and static fields of a class are members of the class (is_static).
struct ImportMember
{
    std::string kind; // "field", "method", "property", "constructor"
    std::string name;
    std::string type; // the type of a field or a property, the return type of a method
    std::string signature; // "(int a, float b)" for a method or a constructor, else empty
    bool is_static = false, is_const = false;
};

struct ImportClass
{
    std::string name, base;
    bool is_interface = false, is_struct = false;
    std::vector<ImportMember> members; // the public members, in source order
};

struct ImportFunction
{
    std::string name, type, signature;
};

struct ImportVar
{
    std::string name, type;
    bool is_const = false;
};

struct ImportEnum
{
    std::string name;
    std::vector<std::string> enumerators;
};

struct ImportFacts
{
    std::vector<ImportClass> classes;
    std::vector<ImportFunction> functions;
    std::vector<ImportVar> vars; // the module constants and the module variables
    std::vector<ImportEnum> enums;
    std::vector<std::string> imports; // the paths of the "using" lines of the script
};

// The facts of the script "text", parsed with the array template "array_tpl" of the importer.
ImportFacts analyze_imports(const std::string& text, const std::string& array_tpl);

// Where the time of one translate call goes, in nanoseconds of std::chrono::steady_clock. The
// numbers are for the measurement only. A cache hit gives zeros. They are not in any cache key.
struct TranslateTiming
{
    int64_t parse_ns = 0;      // the libclang parse (zero when the session cache hits)
    int64_t parse_wait_ns = 0; // the wait for the lock of the entry
    int64_t parses = 0;        // the number of parses in this call
    int64_t front_ns = 0;      // the time before the session lookup
    int64_t lower_ns = 0;      // from the session lookup to the first check
    int64_t check_ns = 0;      // from the first check to the start of the emit
    int64_t emit_ns = 0;       // from the start of the emit to the end
};

// A virtual method that a script class can override. The types have the script spelling.
struct VirtualMethod
{
    std::string name, owner, ret_script;
    std::string signature; // "(float dt)": the types and the names of the parameters
    std::vector<std::string> param_types, param_names;
    bool is_const = false, pure = false, native = false;
};

struct TranslateResult
{
    bool ok = false;
    // The public and protected virtual methods of each native base of the script, by the name of the
    // base as the script writes it. The language server reads them for "override" completion.
    std::map<std::string, std::vector<VirtualMethod>> base_virtuals;
    std::string cpp; // the generated C++ text (valid when ok)
    std::string slots_out; // the new slot file content (valid when ok)
    // The signature lines of the slot file (design 5.3): "sig Cls::fn Sig", one for each script
    // function. A backend writes them after "slots_out" and gives both back in "slots_in". A
    // reload that changes a signature then gets a new thunk with a signature key. After the sig
    // lines come the lines "field Cls::name type", one for each field. The next translation
    // reads them to warn about a field rename. A reader of the slot text skips these lines.
    std::string sigs_out;
    // The identity key of each lambda of the script (and its alias keys), in the order of the
    // translation. See src/translator/lambda_key.h.
    std::vector<std::string> lambda_keys;
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
    // The shadow paths: the paths that a header search tried before it found the header. These
    // files did not exist at the parse. A new file at one of these paths hides the header that
    // the parse used. There is no duplicate. The list is valid when ok is true.
    std::vector<std::string> shadow_paths;
    // The start time of the earliest libclang parse that gave facts to this translate, in
    // nanoseconds of std::filesystem::file_time_type::clock. A session entry can come from an
    // earlier translate, so this time can be before the start of this translate. 0: not known.
    int64_t parse_start_ns = 0;
    // The resolved names of the main script, in the order of the offsets. The table is empty
    // unless TranslateOptions::resolve_refs is true. It is valid also when ok is false, if the
    // translation got to the resolution of the names.
    std::vector<ResolvedRef> refs;
    // The member names that the table does not resolve, in the order of the offsets. Empty
    // unless TranslateOptions::resolve_refs is true.
    std::vector<UnresolvedName> unresolved;
    // The raw bodies of a ".cpp" script that the table does not read: the begin and the end
    // offsets in the main script. Empty unless TranslateOptions::resolve_refs is true.
    std::vector<std::pair<uint32_t, uint32_t>> raw_ranges;
    // The interface of the module (the script): the text of the declarations that importers
    // see. The names with "private" are not in it. A body change does not change it.
    std::string iface;
    // The FNV-1a hash of "iface". It is valid when ok is true.
    uint64_t iface_hash = 0;
    // The script imports as unit name and interface hash: the direct imports and their imports
    // (the importer copies their classes and structs). Each unit is in the list one time. The unit name of an import follows
    // the rule in doc/module-level.md. The hash is the "iface_hash" of the imported script.
    std::vector<std::pair<std::string, uint64_t>> import_hashes;
    // The fine data of "iface" (hot::IfaceInfo): the shape, the member entries and the names.
    // iface_info.hash is iface_hash. Valid when ok is true.
    hot::IfaceInfo iface_info;
    // The names that the script uses and the imports that it needs in full (hot::UseInfo).
    hot::UseInfo use_info;
    // The unit name of the script: TranslateOptions::unit_name, or the stem of the script name.
    std::string unit;
    // AOT code only (TranslateOptions::aot): the C++ text of "<stem>.iface.h" (the stem of the script file). It holds the shell
    // and the state struct of each public class, as copies of the text in "cpp". The importers
    // include it. The TU in "cpp" does not include it, so each TU has one definition of a class.
    std::string iface_header;
    // The searches for the imported scripts, in the order of the translation. Valid when ok.
    std::vector<ImportLookup> import_lookups;
    // The classes of the script and of its imports: the names and the key names (the names in
    // the slot file). The slot lines of these classes can change the output (carry_slots).
    std::vector<std::string> slot_classes, slot_keys;
    // The time of this call. It is not part of the result: the translate cache does not keep it.
    TranslateTiming timing;
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
    // The static members, in source order: kind "const" (a class constant), "static field" or
    // "static method". The name is the script name, without the "Class__" prefix. They are not
    // in "members": they have no state and no reload key.
    std::vector<AnalyzedMember> statics;
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
    std::map<std::string, std::vector<VirtualMethod>> base_virtuals; // see TranslateResult
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
    // The resolved names (see TranslateResult::refs). Empty unless TranslateOptions::resolve_refs is
    // true. A member with a syntax error is left out.
    std::vector<ResolvedRef> refs;
    std::vector<UnresolvedName> unresolved; // see TranslateResult::unresolved
    std::vector<std::pair<uint32_t, uint32_t>> raw_ranges; // see TranslateResult::raw_ranges
};

// The cache of a translator session. A backend holds one for its life.
//
// libclang needs about 400 ms to parse the engine headers. The session keeps the
// result of that parse: the virtual methods of each native base class. The key is the
// include flags, the current directory and the imports. The session also keeps the
// path, the change time and the size of each file that the parse included. A call
// checks them again. If one differs, the session parses again. A session is thread
// safe. Calls with the same key wait for each other during a parse. Calls with different keys
// parse at the same time. Parallel translate calls are permitted. clear() is safe during a
// translate call: a parse in progress keeps its entry.
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

// A text that changes when the translator changes: a hash of the translator sources and of
// its public headers. The build makes it (cmake/HotTranslatorBuildId.cmake). A cache of the
// translate output has it in its key.
const char* translator_build_id();

// Finds an imported script ("using "x.hot";") as translate() does: the name in options.modules,
// else "from_dir" and the directory of the script, then the include directories. In each
// directory options.fs comes first, then the disk. "file" gets the name for "#line" and "unit"
// the unit name. It returns false when no file has the name.
bool find_import(const TranslateOptions& options, const std::string& path, const std::string& from_dir,
    const std::string& from_unit_dir, std::string& text, std::string& file, std::string& unit);

// True when a file is at a shadow path (TranslateResult::shadow_paths). The test is the same as
// the header search of the parse: "fs" (can be null) with the normal spellings, then the disk.
// With "disk" false, the test skips the disk (the caller knows that the folder is not there).
bool shadow_exists(hot::FileSystem* fs, const std::string& path, bool disk = true);

// The 64-bit hash of a text that ImportLookup::text_hash holds.
uint64_t text_hash(const std::string& text);

// The parts of a slot text that translate() copies to its output without a change, for the
// classes of a script (TranslateResult::slot_classes and slot_keys) and a generation:
//   - slots_out starts with "manifest". The lines of the new generation follow.
//   - sigs_out is "sigs", the own "sig" lines, "fields", and then the own "field" lines.
// "read" has the lines that can change the output: the lines of the classes and of the keys.
// A translation cache compares "relative", and then makes the output from the parts.
// "relative" is "read" with the generation of each manifest line relative to "generation":
// "<" (older), "=" (this generation) or ">" (later). The translation uses a generation of the slot
// text only in a compare with its own generation. Thus two generations with the same "relative"
// give the same C++ text. The new manifest lines of slots_out have the number of the generation.
struct SlotCarry
{
    std::string manifest, sigs, fields, read, relative;
};
SlotCarry carry_slots(const std::string& slots_in, const std::vector<std::string>& classes,
    const std::vector<std::string>& keys, int generation);

// Reads "text" with the tolerant parser and returns the facts. The text replaces
// options.script_text. The function does not change the result of translate(): a script
// with a syntax error still gives ok = false and no C++ there.
AnalyzeResult analyze(const std::string& text, const TranslateOptions& options);
AnalyzeResult analyze(const std::string& text, const TranslateOptions& options, TranslatorSession& session);

// The virtual methods that the script class "class_name" can override: the virtual, abstract and
// override methods of its script parents and the virtual methods of its native bases. A method that
// the class or a nearer parent overrides, a static method and a private method are not in the list.
// The text can have errors. The class can be partly written.
std::vector<VirtualMethod> virtual_methods(const std::string& text, const TranslateOptions& options,
    TranslatorSession& session, const std::string& class_name);

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

// Registers the LLVM targets of libclang one time in the process. clang_createIndex registers
// all targets with no lock, and a parallel first registration of one target makes a loop in
// the target list. Each translate calls it. A host that runs the Clang library on other threads
// (the JIT) calls it before its first parallel work. It is thread safe.
void register_llvm_targets();

} // namespace hotc
