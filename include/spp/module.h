// A loadable generation: the register function and the owner of the code memory.
#pragma once

#include <memory>
#include <optional>
#include <string>

namespace spp {

class Registry;

// The code of a generation module stays valid while a copy of "keep_alive" exists.
// For a shared library, the keep-alive object calls dlclose when the last copy goes.
struct Module {
    using RegisterFn = void (*)(Registry&);
    RegisterFn register_fn = nullptr;
    std::shared_ptr<void> keep_alive;
    std::string origin;  // a name for the messages, for example the path of the library
};

// The result of a build or a load: a Module, or an error text with the diagnostics.
struct LoadResult {
    std::optional<Module> module;
    std::string error;
    explicit operator bool() const { return module.has_value(); }
};

// Builds a Module from an existing shared library (dlopen and dlsym).
LoadResult module_from_library(const std::string& path);

}  // namespace spp
