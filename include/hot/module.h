// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A loadable generation: the register function and the owner of the code memory.
#pragma once

#include "hot/runtime/reload_result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hot
{

class Registry;

struct LoadResult;

// The code of a generation module stays valid while a copy of "keep_alive" exists.
// For a shared library, the keep-alive object calls dlclose when the last copy goes. On Windows
// it calls FreeLibrary.
//
// The core module (design 4.9, M8). "make_core" loads a second, independent instance of the
// same code. The loader calls it once, when the generation adds a class or a thunk. The
// loader keeps the core for the whole session. The core gives the class records, the
// factories, the shell constructors, the vtables and the thunks. So the generation itself
// can unload later. Without "make_core" the generation holds these parts and does not
// unload while they are in use.
struct Module
{
    using RegisterFn = void (*)(Registry&);
    RegisterFn register_fn = nullptr;
    std::shared_ptr<void> keep_alive;
    std::string origin; // a name for the messages, for example the path of the library
    std::function<LoadResult()> make_core; // empty: the module cannot make a core instance
    // The Loader calls it when the module is live: at the end of a successful load, and at the end
    // of a successful commit of a reload. A backend uses it to write the slots file only for a
    // live module. A rejected module, and a reload that does not commit, never call it.
    std::function<void()> on_accept;
    // The unit of the module, for example the stem of a script. The empty unit is the default:
    // a reload of it removes every class that the new module lacks. A reload of a named unit
    // removes only the missing classes of this unit, and it keeps the module block of each other
    // unit. A class that arrives in another unit than its first unit makes the module bad.
    std::string unit;
    // The entry point "main" of the script (the symbol "hot_main"). It is null when the module has
    // no "main". The pointer stays valid while the module library stays loaded. An AOT module
    // leaves it null. The function does not catch an exception of the script.
    using EntryMainFn = int32_t (*)(int32_t, const char* const*);
    EntryMainFn entry_main = nullptr;
    // The interface hash of the module (hotc::TranslateResult::iface_hash). Zero: not known.
    uint64_t iface_hash = 0;
    // The direct script imports of the module as unit name and interface hash.
    std::vector<std::pair<std::string, uint64_t>> imports;
};

// The file suffix of a module library on this platform. macOS uses ".so" too: it is the CMake
// suffix of a MODULE library there (CMAKE_SHARED_MODULE_SUFFIX), and dlopen takes any name.
inline constexpr const char* module_suffix()
{
#ifdef _WIN32
    return ".dll";
#else
    return ".so";
#endif
}

// The result of a build or a load: a Module, or an error text with the diagnostics.
struct LoadResult
{
    std::optional<Module> module;
    std::string error;
    // Why the build failed. A backend failure is CompileError. SlotsRemoved means that the loader
    // found fewer slots in the new record. This is a safety net. The translator does not produce it.
    ReloadReason reason = ReloadReason::None;
    // The files that the build read, other than the script: the native headers and the imported
    // scripts. The host watches them. A change of one of them needs a full reload. A backend fills
    // the list also when the build fails.
    std::vector<std::string> dependencies;
    // The interface hash and the import hashes of the translation. A backend copies them to
    // "module" after a successful build (Module::iface_hash, Module::imports).
    uint64_t iface_hash = 0;
    std::vector<std::pair<std::string, uint64_t>> import_hashes;
    explicit operator bool() const { return module.has_value(); }
};

// Builds a Module from an existing shared library (dlopen and dlsym, or LoadLibraryA and
// GetProcAddress on Windows).
LoadResult module_from_library(const std::string& path);

} // namespace hot
