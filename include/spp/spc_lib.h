// spc_lib: the translator as a library. The input and the output are strings.
//
// translate() does not write a file and does not read a script or a slot file.
// libclang reads the headers that the script imports, from the include dirs.
// The function is thread safe when each thread uses its own call.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace spc {

struct TranslateOptions {
    std::string script_text;   // the content of the script
    std::string script_name;   // the name for "#line" and for the diagnostics
    std::string output_name;   // the name of the generated .cpp for "#line" (default: script_name + ".cpp")
    int generation = 0;        // 1 or more
    std::string slots_in;      // the content of the slot file (empty: no earlier generation)
    std::vector<std::string> include_dirs;  // for the imports of the script
    std::string resource_dir;  // the Clang resource directory (empty: the built-in default)
};

struct TranslateResult {
    bool ok = false;
    std::string cpp;          // the generated C++ text (valid when ok)
    std::string slots_out;    // the new slot file content (valid when ok)
    std::string diagnostics;  // the errors, one line each: "name:line:col: error: text"
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

// Uses a temporary session: each call parses the headers.
TranslateResult translate(const TranslateOptions& options);

// Uses the cache of the session.
TranslateResult translate(const TranslateOptions& options, TranslatorSession& session);

}  // namespace spc
