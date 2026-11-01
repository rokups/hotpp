// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The HOT_HIDDEN macro. Helpers on the script call path must not create debugger frames or variables.
#pragma once

// A small helper that the generated code calls. It inlines at the call site and has no debug
// info of its own, so "step into" on a script line does not enter it.
// HOT_EXPORT keeps a symbol visible outside its module. The AOT libraries build with
// -fvisibility=hidden. A type whose RTTI must match across the host and a script module needs it.
// On Windows a module is a DLL. The runtime exports the symbol. A module build
// (HOT_MODULE_BUILD defined) imports it.
#if defined(_WIN32) && defined(HOT_MODULE_BUILD)
    #define HOT_EXPORT __declspec(dllimport)
#elif defined(_WIN32)
    #define HOT_EXPORT __declspec(dllexport)
#elif defined(__clang__) || defined(__GNUC__)
    #define HOT_EXPORT [[gnu::visibility("default")]]
#else
    #define HOT_EXPORT
#endif

// HOT_MODULE_EXPORT marks a symbol that a script module gives to the host, for example
// script_register. The module defines it, so it is always an export, also under HOT_MODULE_BUILD.
// How a module DLL reaches the host symbols (the HOT_EXPORT symbols of the runtime):
// - The host executable exports them. CMake ENABLE_EXPORTS on the host target makes the
//   executable write an import library (for example libhost.dll.a or host.lib).
// - The module links that import library (ClangFilesOptions::host_import_library). Then the
//   loader binds each dllimport symbol of the module to the executable.
// - A MinGW (GNU ld) link of the host with ENABLE_EXPORTS exports all symbols, not only the
//   dllexport ones. An MSVC link exports only the dllexport symbols.
#if defined(_WIN32)
    #define HOT_MODULE_EXPORT __declspec(dllexport)
#elif defined(__clang__) || defined(__GNUC__)
    #define HOT_MODULE_EXPORT [[gnu::visibility("default")]]
#else
    #define HOT_MODULE_EXPORT
#endif

// Only clang knows the nodebug attribute. GCC ignores it and warns.
#if defined(__clang__)
    #define HOT_HIDDEN [[gnu::always_inline, gnu::nodebug]] inline
#elif defined(__GNUC__)
    #define HOT_HIDDEN [[gnu::always_inline]] inline
#elif defined(_MSC_VER)
    #define HOT_HIDDEN __forceinline
#else
    #define HOT_HIDDEN inline
#endif

// A helper that cannot be always_inline (large, out-of-line or address taken). It has no debug
// info, so the debugger steps over it. Put it before the return type or on the declaration.
#if defined(__clang__)
    #define HOT_NODEBUG [[gnu::nodebug]]
#else
    #define HOT_NODEBUG
#endif

// A cold helper of a fast path. It stays out of line, so the fast path keeps few registers and
// no stack slots. It has no debug info. Put it before the return type.
#if defined(__clang__)
    #define HOT_NOINLINE [[gnu::noinline, gnu::nodebug]]
#elif defined(__GNUC__)
    #define HOT_NOINLINE [[gnu::noinline]]
#elif defined(_MSC_VER)
    #define HOT_NOINLINE __declspec(noinline)
#else
    #define HOT_NOINLINE
#endif

// An attribute list for a local variable in generated code. The variable can be unused. It has
// no debug info, so the debugger does not show it.
#if defined(__clang__)
    #define HOT_NODEBUG_LOCAL [[maybe_unused, gnu::nodebug]]
#else
    #define HOT_NODEBUG_LOCAL [[maybe_unused]]
#endif
