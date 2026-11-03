// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// JitBackend: translate in memory, compile in memory with the Clang library,
// load the code with LLVM ORC. No file is written and no process starts.
#pragma once

#include "hot/backend.h"
#include "hot/hotc_lib.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hot
{

struct JitOptions
{
    std::string compiler = "clang++"; // the driver name; an absolute path finds the toolchain best
    std::vector<std::string> include_dirs; // for the script imports and for the compile
    std::string resource_dir; // the Clang resource dir (empty: the default)
    // The C++ standard of the compile and of the import parse, for example "c++17". Empty: the
    // standard of the host (hot::cxx_std_name() in hot/config.h).
    std::string std_version;
    // The translator cache. Empty: the backend makes its own. Share one to keep the
    // cache between backends.
    std::shared_ptr<hotc::TranslatorSession> session;
    std::vector<std::string> extra_flags; // added to the compile command
    std::string pch_path; // optional: a precompiled header for -include-pch
    bool strict_borrowed = true; // false: a borrowed-storage finding is a warning
    bool null_warnings = false; // true: a nullability finding is a warning (hotc --null-warnings)
    // True: the error text is the raw output of the compiler. False (the default): it speaks
    // script names (hotc::rewrite_diagnostic).
    bool raw_diagnostics = false;
    // The debug flag. "-g" gives full debug info, but Clang leaks about 270 KB of
    // debug metadata in each compile that fails. Line tables do not leak.
    // The empty string gives no debug info.
    std::string debug_flag = "-gline-tables-only";
    // True: register the JIT code with the debugger (GDB JIT interface, design 4.10). Needs
    // a debug flag to be useful: the line table names the .hot files.
    bool debugger_support = true;
    // The file system of the host for the script imports and the "using "x.h";" headers. Null: the
    // disk only. For each directory the backend asks this file system first and then the disk.
    // It must be thread safe (hot/vfs.h).
    std::shared_ptr<hot::FileSystem> fs;
    // The preludes of the host (stub-only scripts), as name and text. The translator reads them
    // before each script, in this order (hotc::TranslateOptions::preludes).
    std::vector<std::pair<std::string, std::string>> preludes;
};

struct JitState; // the LLJIT of the session

// The time of the phases of the last successful or failed load, in milliseconds.
struct JitTimings
{
    double translate_ms = 0; // hotc::translate (libclang parses the headers)
    double compile_ms = 0; // Clang: parse, Sema, IR generation, IR optimization
    double jit_ms = 0; // ORC: code generation, link, static initializers, lookup
};

class JitBackend : public Backend
{
public:
    explicit JitBackend(JitOptions options);
    ~JitBackend() override;

    // Each call makes a new JITDylib, so a reload of the same generation is possible.
    // The slot order of the fn methods stays in memory between the calls. It changes only when
    // the module is live (the "on_accept" callback of the Module). Fresh mode: see
    // backend.h.
    LoadResult load(const ScriptSource& source, uint32_t generation, LoadMode mode = LoadMode::Reload) override;
    const char* name() const override { return "JIT"; }
    // Sets the slot text in memory when it is empty or when the text covers it (backend.h).
    void seed_slots(const std::string& text) override;
    // Puts the signature lines of the AOT code back in the slot text (Backend::revert_slots).
    void revert_slots(const std::string& text) override;
    // Replaces the preludes for the next loads (Backend::set_preludes).
    bool set_preludes(std::vector<std::pair<std::string, std::string>> preludes) override
    {
        opt_.preludes = std::move(preludes);
        return true;
    }

    JitTimings last_timings() const { return timings_; }

    // True after the first load when the debugger plugin is on. When the plugin could not start,
    // debugger_error() has the reason. Both are empty or false before the first load.
    bool debugger_support() const { return debugger_support_; }
    const std::string& debugger_error() const { return debugger_error_; }

    // Makes a precompiled header from the header file at "header_path", in the
    // process. The file "out_pch" is the only file that the backend writes, and
    // only here. The header must have the includes that the generated code starts
    // with. The PCH keeps the path of the header: do not move or change the header.
    bool make_pch(const std::string& header_path, const std::string& out_pch, std::string& error);

private:
    std::shared_ptr<JitState> impl_; // a Module keeps a copy: the JIT must outlive the code
    JitOptions opt_;
    std::mutex mutex_; // one build at a time: the slot text is shared
    // The slot text. A Module keeps a copy of the pointer, so the callback is safe after the backend.
    struct SlotState
    {
        std::mutex mutex;
        std::string text;
    };
    std::shared_ptr<SlotState> slots_ = std::make_shared<SlotState>();
    unsigned sequence_ = 0;
    unsigned epoch_ = 0; // counts the fresh loads; it is in the name of the JITDylib
    JitTimings timings_;
    bool debugger_support_ = false;
    std::string debugger_error_;
};

} // namespace hot
