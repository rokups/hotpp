// sppc_lib: the translator as a library. The input and the output are strings.
//
// translate() does not write a file and does not read a script or a slot file.
// libclang reads the headers that the script imports, from the include dirs.
// The function is thread safe when each thread uses its own call.
#pragma once

#include <memory>
#include <string>
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

struct TranslateResult {
    bool ok = false;
    std::string cpp;          // the generated C++ text (valid when ok)
    std::string slots_out;    // the new slot file content (valid when ok)
    std::string diagnostics;  // the errors, one line each: "name:line:col: error: text"
    // The warnings, one line each: "name:line:col: warning: text". The translation can be ok
    // with warnings. The text is empty when there is no warning.
    std::string warnings;
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

// The scripts of a build: pairs of the name that "#line" uses and the script text.
using ScriptTexts = std::vector<std::pair<std::string, std::string>>;

// Rewrites the text that Clang prints for the generated C++ so that it speaks script names
// (design 4.2, experiment F2). The function is pure: it has no state and no I/O.
//   - A generated name becomes a script name: "Player_Shell" -> "Player", "Player__heal" ->
//     "Player.heal", "int32_t" -> "int", "std::string" -> "std.string".
//   - The argument counts of a script method do not count the hidden "self" argument.
//   - A common mistake gets a plain message (the table is in diag_rewrite.cpp).
//   - A source line from the generated code is not shown. If "scripts" has the text of the
//     script, the line of the script replaces it. The caret line is dropped.
//   - The column of a script location is 1: the generated column has no meaning in the script.
//   - A note that points into the generated file gets the location of its error. An error that
//     points into the generated file gets the last script line before it, if "generated" has the
//     text of that file (pairs of the "#line" name of the file and its text). A note from a
//     system header is hidden, and one line says how many.
// A line that is not part of a Clang diagnostic stays as it is. Pass the raw text on when the
// user asks for it (--raw-diagnostics).
std::string rewrite_diagnostic(const std::string& text, const ScriptTexts& scripts = {},
                               const ScriptTexts& generated = {});

// Uses a temporary session: each call parses the headers.
TranslateResult translate(const TranslateOptions& options);

// Uses the cache of the session.
TranslateResult translate(const TranslateOptions& options, TranslatorSession& session);

}  // namespace sppc
