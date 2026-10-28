// A loadable generation: the register function and the owner of the code memory.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace spp {

class Registry;

struct LoadResult;

// The code of a generation module stays valid while a copy of "keep_alive" exists.
// For a shared library, the keep-alive object calls dlclose when the last copy goes.
//
// The core module (design 4.9, M8). "make_core" loads a second, independent instance of the
// same code. The loader calls it once, when the generation adds a class or a thunk. The
// loader keeps the core for the whole session. The core gives the class records, the
// factories, the shell constructors, the vtables and the thunks. So the generation itself
// can unload later. Without "make_core" the generation holds these parts and does not
// unload while they are in use.
struct Module {
    using RegisterFn = void (*)(Registry&);
    RegisterFn register_fn = nullptr;
    std::shared_ptr<void> keep_alive;
    std::string origin;  // a name for the messages, for example the path of the library
    std::function<LoadResult()> make_core;  // empty: the module cannot make a core instance
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
