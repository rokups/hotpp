// JitBackend: translate in memory, compile in memory with the Clang library,
// load the code with LLVM ORC. No file is written and no process starts.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backend.h"
#include "spc_lib.h"

namespace script {

struct JitOptions {
    std::string compiler = "clang++";       // the driver name; an absolute path finds the toolchain best
    std::vector<std::string> include_dirs;  // for the script imports and for the compile
    std::string resource_dir;               // the Clang resource dir (empty: the default)
    // The translator cache. Empty: the backend makes its own. Share one to keep the
    // cache between backends.
    std::shared_ptr<spc::TranslatorSession> session;
    std::vector<std::string> extra_flags;   // added to the compile command
    std::string pch_path;                   // optional: a precompiled header for -include-pch
    // The debug flag. "-g" gives full debug info, but Clang leaks about 270 KB of
    // debug metadata in each compile that fails. Line tables do not leak.
    // The empty string gives no debug info.
    std::string debug_flag = "-gline-tables-only";
};

struct JitState;  // the LLJIT of the session

// The time of the phases of the last successful or failed load, in milliseconds.
struct JitTimings {
    double translate_ms = 0;  // spc::translate (libclang parses the headers)
    double compile_ms = 0;    // Clang: parse, Sema, IR generation, IR optimization
    double jit_ms = 0;        // ORC: code generation, link, static initializers, lookup
};

class JitBackend : public Backend {
public:
    explicit JitBackend(JitOptions options);
    ~JitBackend() override;

    // Each call makes a new JITDylib, so a reload of the same generation is possible.
    // The slot order of the fn methods stays in memory between the calls.
    LoadResult load(const ScriptSource& source, uint32_t generation) override;
    const char* name() const override { return "JIT"; }

    JitTimings last_timings() const { return timings_; }

    // Makes a precompiled header from the header file at "header_path", in the
    // process. The file "out_pch" is the only file that the backend writes, and
    // only here. The header must have the includes that the generated code starts
    // with. The PCH keeps the path of the header: do not move or change the header.
    bool make_pch(const std::string& header_path, const std::string& out_pch, std::string& error);

private:
    std::shared_ptr<JitState> impl_;  // a Module keeps a copy: the JIT must outlive the code
    JitOptions opt_;
    std::mutex mutex_;  // one build at a time: the slot text is shared
    std::string slots_;
    unsigned sequence_ = 0;
    JitTimings timings_;
};

}  // namespace script
