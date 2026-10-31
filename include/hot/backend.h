// The backend interface: it turns script source into a loadable Module.
//
// The Loader does not know how a Module is made. A backend can run an external
// compiler (ClangFilesBackend) or compile in the process (JitBackend).
// CMake selects the default backend with the cache option HOT_BACKEND.
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

// The backend that HOT_BACKEND selected at build time.
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
};
std::unique_ptr<Backend> make_default_backend(const DefaultBackendOptions& options);

} // namespace hot
