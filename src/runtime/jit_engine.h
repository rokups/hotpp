// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The JIT engine: the only code that uses the LLVM and Clang C++ API. The interface is C, so no
// C++ object and no allocation goes across it. On MSVC the engine is a DLL with the static
// runtime (/MT), because the LLVM release has only /MT libraries. The host, the runtime and the
// script modules use the DLL runtime (/MD), so they share one heap. On other targets the engine
// is a static library with the same interface.
//
// Rules of the interface:
// - A "char**" output gets a string that the engine allocates. hot_jit_string_free releases it.
//   A null output pointer is not written.
// - The engine does not keep a pointer to an input after the call.
// - An object that the engine makes is released only with the engine function for it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(HOT_JIT_ENGINE_SHARED)
    #ifdef HOT_JIT_ENGINE_BUILD
        #define HOT_JIT_ENGINE_API __declspec(dllexport)
    #else
        #define HOT_JIT_ENGINE_API __declspec(dllimport)
    #endif
#else
    #define HOT_JIT_ENGINE_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

// The IR of one compiled translation unit.
typedef struct hot_jit_ir hot_jit_ir;
// One LLJIT with a search of the process symbols.
typedef struct hot_jit_engine hot_jit_engine;
// One JITDylib with the code of one IR. Its static initializers ran.
typedef struct hot_jit_dylib hot_jit_dylib;

HOT_JIT_ENGINE_API void hot_jit_string_free(char* text);

// Compiles the C++ file "args[n_args - 1]" to IR. "args" is the command line of the driver,
// "args[0]" is the compiler. The files ("paths", "texts") go into a memory file system. Their
// paths must be absolute and normal. Returns 1 and sets "*out_ir", else returns 0.
// "*out_diagnostics" gets the compiler messages in both cases.
HOT_JIT_ENGINE_API int hot_jit_compile(const char* const* args, size_t n_args, const char* const* paths,
    const char* const* texts, size_t n_files, hot_jit_ir** out_ir, char** out_diagnostics);

// Writes the precompiled header of the header "args[n_args - 1]" to "out_pch". Returns 1 on
// success, else 0. "*out_diagnostics" gets the compiler messages.
HOT_JIT_ENGINE_API int hot_jit_make_pch(
    const char* const* args, size_t n_args, const char* out_pch, char** out_diagnostics);

// A copy of the IR in the same context. The copy and "ir" must not be used in parallel.
HOT_JIT_ENGINE_API hot_jit_ir* hot_jit_ir_clone(const hot_jit_ir* ir);
HOT_JIT_ENGINE_API void hot_jit_ir_free(hot_jit_ir* ir);

// Makes a JIT. With "debugger_support" the debugger sees the JIT code (GDB JIT interface). A
// failure of the debugger support is not fatal: "*out_debugger_error" gets the reason. Returns
// null and sets "*out_error" when the JIT cannot start.
HOT_JIT_ENGINE_API hot_jit_engine* hot_jit_engine_create(
    int debugger_support, int* out_debugger_on, char** out_error, char** out_debugger_error);
// Call this only after hot_jit_remove of each dylib of the engine.
HOT_JIT_ENGINE_API void hot_jit_engine_free(hot_jit_engine* engine);

// Adds "ir" to a new JITDylib "id" and runs the static initializers. The call takes "ir" also
// on failure. The absolute symbols ("abs_names", "abs_values") are defined in the dylib first.
// Returns null and sets "*out_error" on failure.
HOT_JIT_ENGINE_API hot_jit_dylib* hot_jit_add(hot_jit_engine* engine, const char* id, hot_jit_ir* ir,
    const char* const* abs_names, const uint64_t* abs_values, size_t n_abs, char** out_error);

// The address of the symbol "name" in the dylib. Null and "*out_error" when it is not there.
HOT_JIT_ENGINE_API void* hot_jit_lookup(hot_jit_dylib* dylib, const char* name, char** out_error);

// Runs the static destructors of the dylib and removes its code.
HOT_JIT_ENGINE_API void hot_jit_remove(hot_jit_dylib* dylib);

#ifdef __cplusplus
}
#endif
