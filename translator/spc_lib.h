// spc_lib: the translator as a library. The input and the output are strings.
//
// translate() does not write a file and does not read a script or a slot file.
// libclang reads the headers that the script imports, from the include dirs.
// The function is thread safe when each thread uses its own call.
#pragma once

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
    std::string diagnostics;  // the errors, one line each: "name:line: error: text"
};

TranslateResult translate(const TranslateOptions& options);

}  // namespace spc
