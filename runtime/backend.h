// The backend interface: it turns script source into a loadable Module.
//
// The Loader does not know how a Module is made. A backend can run an external
// compiler (ClangFilesBackend) or compile in the process (JIT, not implemented yet).
// CMake selects the default backend with the cache option SPP_BACKEND.
// A library that the tool sppc made ahead of time needs no backend: call
// module_from_library (module.h) and give the Module to the Loader.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "module.h"

namespace script {

struct ScriptSource {
    std::string name;  // the script name or path; the diagnostics and "#line" use it
    std::string text;  // the content of the script
};

class Backend {
public:
    virtual ~Backend() = default;
    // Makes the Module of one script for one generation. On failure the result has
    // an error text with the compiler diagnostics. It does not touch the live generation.
    virtual LoadResult load(const ScriptSource& source, uint32_t generation) = 0;
    virtual const char* name() const = 0;
};

// The backend that SPP_BACKEND selected at build time.
std::unique_ptr<Backend> make_default_backend();

}  // namespace script
