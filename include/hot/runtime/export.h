// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The export macros of Hot++. Only these macros export a symbol. The build does not export all
// symbols (no WINDOWS_EXPORT_ALL_SYMBOLS, no --export-all-symbols).
#pragma once

// HOT_EXPORT marks a symbol of the host that a script module uses. Examples: a runtime function
// that the generated code calls, a native class of the host that a script derives from or calls.
// - Windows (MSVC and MinGW): a module is a DLL. The host exports the symbol (dllexport). A module
//   build (HOT_MODULE_BUILD defined) imports it (dllimport). Mark a whole class, so the vtable
//   and the type info come from the host. Mark a variable too: a module cannot import data
//   without dllimport.
// - ELF and Mach-O: default visibility. The AOT libraries build with -fvisibility=hidden. A type
//   whose RTTI must match across the host and a script module needs it.
// How a module DLL reaches the HOT_EXPORT symbols:
// - CMake ENABLE_EXPORTS on the host target makes the executable write an import library (for
//   example libhost.dll.a or host.lib). hot_export_host copies it next to the executable.
// - The module links that import library (ClangFilesOptions::host_import_library). Then the
//   loader binds each dllimport symbol of the module to the executable.
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
#if defined(_WIN32)
    #define HOT_MODULE_EXPORT __declspec(dllexport)
#elif defined(__clang__) || defined(__GNUC__)
    #define HOT_MODULE_EXPORT [[gnu::visibility("default")]]
#else
    #define HOT_MODULE_EXPORT
#endif

// HOT_EXPORT_STD_BEGIN and HOT_EXPORT_STD_END go around the HOT_EXPORT classes that have a
// standard library base or member, for example a std::string field. MSVC then writes C4251
// and C4275: the std type has no dll export. The host and each script module use the same
// DLL C++ runtime (/MD, -fms-runtime-lib=dll), so the std types are the same in both. Thus
// the two warnings do not apply, and these macros stop them only between BEGIN and END.
#if defined(_MSC_VER)
    #define HOT_EXPORT_STD_BEGIN __pragma(warning(push)) __pragma(warning(disable : 4251 4275))
    #define HOT_EXPORT_STD_END __pragma(warning(pop))
#else
    #define HOT_EXPORT_STD_BEGIN
    #define HOT_EXPORT_STD_END
#endif
