// ClangFilesBackend: translate in memory, write a .cpp, run an external
// compiler to make a .so, then dlopen it.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "spp/backend.h"
#include "spp/spc_lib.h"

namespace script {

struct ClangFilesOptions {
    std::string compiler = "clang++";         // the compiler command
    std::vector<std::string> include_dirs;    // for the script imports and for the compile
    std::string resource_dir;                 // the Clang resource dir (empty: the default)
    // The translator cache. Empty: the backend makes its own. Share one to keep the
    // cache between backends.
    std::shared_ptr<spc::TranslatorSession> session;
    std::vector<std::string> extra_flags;     // added to the compile command
    std::string work_dir = "spp_work";        // the .cpp and .so files go here
    std::string slots_file;                   // empty: <work_dir>/slots.txt
};

class ClangFilesBackend : public Backend {
public:
    explicit ClangFilesBackend(ClangFilesOptions options);

    // Makes a new .so for each call, so a reload of the same generation is possible.
    LoadResult load(const ScriptSource& source, uint32_t generation) override;
    const char* name() const override { return "CLANG_FILES"; }

    // Translates and compiles one script into "out_so". It writes the .cpp
    // next to the .so. It does not load the library. The slot file changes only
    // when the compile succeeds. On failure, "error" has the diagnostics.
    bool build(const ScriptSource& source, uint32_t generation, const std::string& out_so,
               std::string& error);

private:
    ClangFilesOptions opt_;
    std::mutex mutex_;  // one build at a time: the slot file is shared
    std::atomic<unsigned> sequence_{0};
};

// Runs a command without a shell. "output" gets stdout and stderr together.
// Returns the exit code, or -1 when the command did not run.
int run_command(const std::vector<std::string>& argv, std::string& output);

}  // namespace script
