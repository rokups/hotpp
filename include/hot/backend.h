// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The backend interface: it turns script source into a loadable Module.
//
// The Loader does not know how a Module is made. A backend can run an external
// compiler (ClangFilesBackend) or compile in the process (JitBackend).
// The CMake options HOT_ENABLE_CLANG_FILES and HOT_ENABLE_JIT decide which backends the build has.
// The program chooses one at run time (make_default_backend).
// A library that the tool hotc made ahead of time needs no backend: call
// module_from_library (module.h) and give the Module to the Loader.
#pragma once

#include "hot/module.h"
#include "hot/vfs.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hot
{

struct ScriptSource
{
    std::string name; // the script name or path; the diagnostics and "#line" use it
    std::string text; // the content of the script
    // The imported scripts ("using "parent.hot";"), as the path in the "using" line and the text.
    // One generation holds them all (design 4.10). A path that is not here is read from disk.
    std::vector<std::pair<std::string, std::string>> modules;
};

// Reload: the new generation continues the slot numbering of the earlier generations.
// Fresh: the load ignores the slot state of the earlier generations (the backend does not read
// the slots file, and the slot numbering starts empty). It builds a new library with a new name,
// so it never overwrites a library that the process still maps. A full reload uses it
// (the host calls Loader::reset first).
enum class LoadMode
{
    Reload,
    Fresh
};

class Backend
{
public:
    virtual ~Backend() = default;
    // Makes the Module of one script for one generation. On failure the result has
    // an error text with the compiler diagnostics and a "reason". It does not touch the live
    // generation. It does not write the slot state: the Module has an "on_accept" callback that
    // writes it, and the Loader calls the callback when the module is live.
    // Threads: load can run on a worker thread while the old code runs on other threads. Run one
    // load at a time for each backend. Do not run Loader::commit_pending or Loader::load during a
    // load: they call "on_accept", and load reads the slot state. The library of the script opens
    // on the thread of the call, so its static initializers run on that thread.
    virtual LoadResult load(const ScriptSource& source, uint32_t generation, LoadMode mode = LoadMode::Reload) = 0;
    virtual const char* name() const = 0;

    // Gives the backend the slot and signature manifest of the AOT scripts (AotTable::slots), so
    // the next generation keeps the slot numbers of the AOT code. Call it before the first load.
    // The rule: the backend keeps its slot state only when the state has every line of the text
    // (see slots_cover). Such a state comes from a later generation of the same scripts. Else the
    // backend takes the text, because the state is empty or stale (for example a file of an
    // earlier run). The default does nothing.
    virtual void seed_slots(const std::string& text) { (void)text; }

    // Gives the signature lines of the AOT code back to the slot state. Loader::revert_unit calls
    // it at the commit, with the AOT slot lines of the classes of the reverted unit. The backend
    // learns an override only in on_accept, and a revert has no such callback of the backend. So
    // without this call the state still has the signatures of the override, and a later override
    // that has the same signatures emits no key for them (see slots_revert). The default does nothing.
    virtual void revert_slots(const std::string& text) { (void)text; }

    // Replaces the preludes (name and text) for the next loads. A host calls it when a prelude
    // file changes, and then loads its scripts again. Do not call it during a load. Return false
    // when the backend has no preludes: the default does nothing.
    virtual bool set_preludes(std::vector<std::pair<std::string, std::string>> preludes)
    {
        (void)preludes;
        return false;
    }
};

// Replaces each line "sig Cls::fn Sig" of "state" with the line of "aot" for the same function.
// Other lines of "state" stay, so the slot numbers of the methods that an override added stay.
inline std::string slots_revert(const std::string& state, const std::string& aot)
{
    auto name_of = [](const std::string& line)
    {
        const size_t sp = line.find(' ', 4);
        return line.rfind("sig ", 0) == 0 && sp != std::string::npos ? line.substr(4, sp - 4) : std::string();
    };
    std::vector<std::pair<std::string, std::string>> aot_sigs; // function, line
    for (size_t pos = 0; pos < aot.size();)
    {
        size_t eol = aot.find('\n', pos);
        if (eol == std::string::npos)
            eol = aot.size();
        const std::string line = aot.substr(pos, eol - pos);
        pos = eol + 1;
        if (std::string n = name_of(line); !n.empty())
            aot_sigs.emplace_back(std::move(n), line);
    }
    std::string out;
    for (size_t pos = 0; pos < state.size();)
    {
        size_t eol = state.find('\n', pos);
        const bool last = eol == std::string::npos;
        if (last)
            eol = state.size();
        std::string line = state.substr(pos, eol - pos);
        pos = eol + 1;
        if (const std::string n = name_of(line); !n.empty())
            for (const auto& [fn, aot_line] : aot_sigs)
                if (fn == n)
                {
                    line = aot_line;
                    break;
                }
        out += line;
        if (!last)
            out += '\n';
    }
    return out;
}

// True when every non-empty line of "have" is a line of "text". The backends use it in seed_slots.
inline bool slots_cover(const std::string& text, const std::string& have)
{
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        lines.push_back(text.substr(pos, eol - pos));
        pos = eol + 1;
    }
    for (size_t pos = 0; pos < have.size();)
    {
        size_t eol = have.find('\n', pos);
        if (eol == std::string::npos)
            eol = have.size();
        const std::string line = have.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && std::find(lines.begin(), lines.end(), line) == lines.end())
            return false;
    }
    return true;
}

// The backends that make_default_backend can return.
//   Auto:       the environment variable HOT_BACKEND ("jit" or "files") when it is set. Else the
//               clang files backend when the build has it, else the JIT backend.
//   ClangFiles: the clang files backend (CMake option HOT_ENABLE_CLANG_FILES).
//   Jit:        the JIT backend (CMake option HOT_ENABLE_JIT).
enum class BackendKind
{
    Auto,
    ClangFiles,
    Jit
};

// True when the build has the backend. Auto is true when the build has at least one backend.
bool backend_available(BackendKind kind);

// The backend that the run-time choice selects (BackendKind::Auto, see above).
// It throws std::runtime_error when the backend is not in the build or the variable is wrong.
std::unique_ptr<Backend> make_default_backend();

// Additions to the build-time defaults of make_default_backend.
struct DefaultBackendOptions
{
    std::vector<std::string> include_dirs; // after HOT_DEFAULT_INCLUDES
    std::vector<std::string> extra_flags;  // after HOT_DEFAULT_CXXFLAGS
    // The file system of the host for the script imports and the "using "x.h";" headers. Null: the
    // disk only. For each directory the backend asks this file system first and then the disk.
    // It must be thread safe (hot/vfs.h).
    std::shared_ptr<hot::FileSystem> fs;
    // The preludes of the host (stub-only scripts), as name and text. The translator reads them
    // before each script, in this order (hotc::TranslateOptions::preludes).
    std::vector<std::pair<std::string, std::string>> preludes;
    // The backend to make. An explicit value wins over the environment variable HOT_BACKEND.
    BackendKind kind = BackendKind::Auto;
    // The C++ standard of the scripts, for example "c++20" for "async" scripts. Empty: the
    // standard of the host (hot::cxx_std_name()).
    std::string std_version;
};
std::unique_ptr<Backend> make_default_backend(const DefaultBackendOptions& options);

} // namespace hot
