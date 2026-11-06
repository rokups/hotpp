// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// ClangFilesBackend: translate in memory, write a .cpp, run an external
// compiler to make a .so, then dlopen it.
#pragma once

#include "hot/backend.h"
#include "hot/command_line.h"
#include "hot/hotc_lib.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
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
    std::shared_ptr<hotc::TranslatorSession> session;
    std::vector<std::string> extra_flags; // added to the compile command
    std::string work_dir = "hot_work"; // the .cpp and .so files go here
    std::string slots_file; // empty: <work_dir>/slots.txt
    // True: the destructor of the backend removes work_dir (remove_scratch). The owner must give
    // a directory that no other backend uses.
    bool remove_work_dir_at_exit = false;
    bool strict_borrowed = true; // false: a borrowed-storage finding is a warning
    bool null_warnings = false; // true: a nullability finding is a warning (hotc --null-warnings)
    // True: the error text is the raw output of the compiler. False (the default): it speaks
    // script names (hotc::rewrite_diagnostic).
    bool raw_diagnostics = false;
    // The file system of the host for the script imports and the "using "x.h";" headers. Null: the
    // disk only. For each directory the backend asks this file system first and then the disk.
    // It must be thread safe (hot/vfs.h).
    std::shared_ptr<hot::FileSystem> fs;
    // The preludes of the host (stub-only scripts), as name and text. The translator reads them
    // before each script, in this order (hotc::TranslateOptions::preludes).
    std::vector<std::pair<std::string, std::string>> preludes;
    // The import library of the host executable (Windows). Non-empty: the link of each module DLL
    // adds it, so the module reaches the HOT_EXPORT symbols of the host (hot/runtime/hidden.h).
    std::string host_import_library;
    // The target triple of the modules ("--target="). Empty: the default target of the compiler.
    // A MinGW triple adds the libstdc++ headers of the sysroot (hot/command_line.h).
    std::string target_triple;
    std::string sysroot; // the sysroot of the modules ("--sysroot="); empty: none
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
    // Writes the text to the slots file when the file does not exist yet or when the text covers
    // the content of the file (backend.h: slots_cover). A failed write does nothing.
    void seed_slots(const std::string& text) override;
    // Puts the signature lines of the AOT code back in the slots file (Backend::revert_slots).
    void revert_slots(const std::string& text) override;
    // Replaces the preludes for the next loads (Backend::set_preludes).
    bool set_preludes(std::vector<std::pair<std::string, std::string>> preludes) override
    {
        opt_.preludes = std::move(preludes);
        return true;
    }

    // Translates and compiles one script into "out_so". It writes the .cpp
    // next to the .so. It does not load the library. The slot file changes only
    // when the compile succeeds. On failure, "error" has the diagnostics. "dependencies" (when not
    // null) gets the files that the translation read (TranslateResult::dependencies), also on failure.
    bool build(const ScriptSource& source, uint32_t generation, const std::string& out_so, std::string& error,
        std::vector<std::string>* dependencies = nullptr);

private:
    bool build_impl(const ScriptSource& source, uint32_t generation, LoadMode mode, const std::string& out_so,
        std::string& error, std::string* deferred_slots, std::vector<std::string>* dependencies,
        LoadResult* hashes = nullptr);

    ClangFilesOptions opt_;
    std::mutex mutex_; // one build at a time: the slot file is shared
    std::atomic<unsigned> sequence_{0};
    std::atomic<unsigned> epoch_{0}; // counts the fresh loads; it is in the name of the library
};

// The compile command of the backend for one generated .cpp and its .so. The backend and
// the compile database (hotc --compile-commands) both use it, so the flags are the same.
// With a host file system ("options.fs"), the files of the host go to "<work_dir>/vfs/<path>".
// The command then adds "-I<work_dir>/vfs/<dir>" before each "-I<dir>", and one for
// "script_dir" (the directory of the script; empty: none).
std::vector<std::string> compile_command(const ClangFilesOptions& options, const std::string& cpp_path,
    const std::string& out_so, const std::string& script_dir = std::string());

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
