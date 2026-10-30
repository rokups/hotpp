// hot-ls: a stdio Language Server (LSP 3.17) for S++ scripts (design 10.1, M12 to M14).
//
// The server is a proxy. It owns the text of each .hot document. It runs hotc::analyze() on the
// text and gets the generated C++ and the source map. It gives the generated C++ to a stock
// clangd child process as a virtual file "<script>.cpp" with the compile command of the backend
// (hot::compile_command). It maps the positions of each request and each answer between the
// script and the generated C++. The diag rewriter changes generated names to script names.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace hot::ls
{

struct ServerOptions
{
    std::string clangd = "clangd"; // the clangd binary. Empty: no clangd.
    std::vector<std::string> include_dirs; // for the script imports and for the compile
    std::string resource_dir; // the Clang resource dir (empty: the default)
    std::vector<std::string> extra_flags; // added to the compile command
    // The directory of a compile_commands.json from "hotc --compile-commands". clangd gets it
    // (--compile-commands-dir) and runs its background index on the generated files. The
    // server also reads the -I, -isystem and -D flags of the first entry.
    std::string compile_commands_dir;
    // The directory of the saved symbol index (S28). Empty: ".cache/hot/index" in the first
    // workspace folder.
    std::string index_dir;
    // The directory of the host files (hosts.h). Empty: ".cache/hot/hosts" in each workspace folder.
    std::string hosts_dir;
    std::string clangd_log; // the stderr of clangd. Empty: /dev/null.
};

// Runs the server on two file descriptors until "exit" or the end of the input.
// Returns 0 after a "shutdown" request, else 1 (the LSP rule for "exit").
int run_server(int in_fd, int out_fd, const ServerOptions& options);

// The script place of a 0-based line of a generated C++ text, from its "#line" directives.
// "file" is the name in the directive. "line" is 0-based. Gives no value when no script
// directive comes before the line. A line in text that the translator writes by itself (after
// a directive that names the .cpp file) gets the line of the last script directive.
struct ScriptLine
{
    std::string file;
    int line = 0;
};
std::optional<ScriptLine> line_directive_target(const std::string& cpp, int line);

// Reads the command line of hot-ls (without argv[0]). Gives false and a message on an error.
bool parse_arguments(const std::vector<std::string>& args, ServerOptions& out, std::string& error);

// The text of the usage message.
const char* usage_text();

} // namespace hot::ls
