// The backend interface: it turns script source into a loadable Module.
//
// The Loader does not know how a Module is made. A backend can run an external
// compiler (ClangFilesBackend) or compile in the process (JitBackend).
// CMake selects the default backend with the cache option HOT_BACKEND.
// A library that the tool sppc made ahead of time needs no backend: call
// module_from_library (module.h) and give the Module to the Loader.
#pragma once

#include "hot/module.h"

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
};

// The backend that HOT_BACKEND selected at build time.
std::unique_ptr<Backend> make_default_backend();

// Additions to the build-time defaults of make_default_backend.
struct DefaultBackendOptions
{
    std::vector<std::string> include_dirs; // after HOT_DEFAULT_INCLUDES
    std::vector<std::string> extra_flags;  // after HOT_DEFAULT_CXXFLAGS
};
std::unique_ptr<Backend> make_default_backend(const DefaultBackendOptions& options);

} // namespace hot
