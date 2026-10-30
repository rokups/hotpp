// ClangFilesBackend: translate in memory, write a .cpp, run an external
// compiler to make a .so, then dlopen it.
#pragma once

#include "hot/backend.h"
#include "hot/sppc_lib.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hot
{

struct ClangFilesOptions
{
    std::string compiler = "clang++"; // the compiler command
    std::vector<std::string> include_dirs; // for the script imports and for the compile
    std::string resource_dir; // the Clang resource dir (empty: the default)
    // The C++ standard of the compile and of the import parse, for example "c++17". Empty: the
    // standard of the host (hot::cxx_std_name() in hot/config.h).
    std::string std_version;
    // The translator cache. Empty: the backend makes its own. Share one to keep the
    // cache between backends.
    std::shared_ptr<sppc::TranslatorSession> session;
    std::vector<std::string> extra_flags; // added to the compile command
    std::string work_dir = "hot_work"; // the .cpp and .so files go here
    std::string slots_file; // empty: <work_dir>/slots.txt
    // True: the destructor of the backend removes work_dir (remove_scratch). The owner must give
    // a directory that no other backend uses.
    bool remove_work_dir_at_exit = false;
    bool strict_borrowed = true; // false: a borrowed-storage finding is a warning
    // True: the error text is the raw output of the compiler. False (the default): it speaks
    // script names (sppc::rewrite_diagnostic).
    bool raw_diagnostics = false;
};

class ClangFilesBackend : public Backend
{
public:
    explicit ClangFilesBackend(ClangFilesOptions options);
    ~ClangFilesBackend() override;

    // Makes a new .so for each call, so a reload of the same generation is possible.
    // The slots file does not change here. The Module has an "on_accept" callback that writes
    // it, and the Loader calls it when the module is live. A rejected module leaves the file
    // as it is. The load fails when the slots file is not writable. Fresh mode: see backend.h.
    LoadResult load(const ScriptSource& source, uint32_t generation, LoadMode mode = LoadMode::Reload) override;
    const char* name() const override { return "CLANG_FILES"; }
    // The path of the slots file. The file changes only when the Loader accepts a module.
    const std::string& slots_file() const { return opt_.slots_file; }

    // Translates and compiles one script into "out_so". It writes the .cpp
    // next to the .so. It does not load the library. The slot file changes only
    // when the compile succeeds. On failure, "error" has the diagnostics.
    bool build(const ScriptSource& source, uint32_t generation, const std::string& out_so, std::string& error);

private:
    bool build_impl(const ScriptSource& source, uint32_t generation, LoadMode mode, const std::string& out_so,
        std::string& error, std::string* deferred_slots, bool* slots_removed);

    ClangFilesOptions opt_;
    std::mutex mutex_; // one build at a time: the slot file is shared
    std::atomic<unsigned> sequence_{0};
    std::atomic<unsigned> epoch_{0}; // counts the fresh loads; it is in the name of the library
};

// The compile command of the backend for one generated .cpp and its .so. The backend and
// the compile database (sppc --compile-commands) both use it, so the flags are the same.
std::vector<std::string> compile_command(
    const ClangFilesOptions& options, const std::string& cpp_path, const std::string& out_so);

// One entry of a compile database (the format of clangd and CMake's compile_commands.json).
struct CompileCommandEntry
{
    std::string directory; // the working directory of the command
    std::string file; // the source file
    std::vector<std::string> arguments;
    std::string output;
};

// The text of compile_commands.json. One entry takes 6 lines. The strings are escaped.
std::string compile_commands_json(const std::vector<CompileCommandEntry>& entries);

// Runs a command without a shell. "output" gets stdout and stderr together.
// Returns the exit code, or -1 when the command did not run.
int run_command(const std::vector<std::string>& argv, std::string& output);

} // namespace hot
