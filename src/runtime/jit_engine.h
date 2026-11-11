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
// "*out_diagnostics" gets the compiler messages in both cases. On success "*out_deps" gets the
// files that the compile read (also the system headers and the files of the memory file
// system), one path in each line.
HOT_JIT_ENGINE_API int hot_jit_compile(const char* const* args, size_t n_args, const char* const* paths,
    const char* const* texts, size_t n_files, hot_jit_ir** out_ir, char** out_diagnostics, char** out_deps);

// The text that identifies the machine code of the engine: the format number of a captured
// object, the Clang and LLVM versions, the target triple, the host CPU, its features and the
// code generation level. An object of hot_jit_dylib_object is valid for an engine with the same
// text. Release it with hot_jit_string_free.
HOT_JIT_ENGINE_API char* hot_jit_target_id(void);

// Writes the precompiled header of the header "args[n_args - 1]" to "out_pch". Returns 1 on
// success, else 0. "*out_diagnostics" gets the compiler messages.
HOT_JIT_ENGINE_API int hot_jit_make_pch(
    const char* const* args, size_t n_args, const char* out_pch, char** out_diagnostics);

// A copy of the IR in the same context. The copy and "ir" must not be used in parallel.
HOT_JIT_ENGINE_API hot_jit_ir* hot_jit_ir_clone(const hot_jit_ir* ir);
HOT_JIT_ENGINE_API void hot_jit_ir_free(hot_jit_ir* ir);

// For the tests: compiles a copy of "ir" (a COFF x86_64 target) to an object and loads it with the
// memory manager of the Windows JIT. A hook records the unwind tables. The text gives the counts
// on the first line and one error on each other line. Release it with hot_jit_string_free.
HOT_JIT_ENGINE_API char* hot_jit_coff_unwind_check(const hot_jit_ir* ir);

// For the tests: the IR text of a copy of "ir" after the link preparation of hot_jit_add. For a
// COFF target, the preparation makes the data declarations dllimport. Release the text with
// hot_jit_string_free.
HOT_JIT_ENGINE_API char* hot_jit_ir_link_text(const hot_jit_ir* ir);

// Makes a JIT. With "debugger_support" the debugger sees the JIT code (GDB JIT interface). A
// failure of the debugger support is not fatal: "*out_debugger_error" gets the reason. Returns
// null and sets "*out_error" when the JIT cannot start. Windows: "host_runtime" has one address in
// each C runtime DLL of the host (jit::host_runtime_addresses). The JIT code takes a name from
// these DLLs first and only then from the other loaded modules. Another C runtime DLL gives no
// name (jit_search_order.h). Other platforms ignore the list.
HOT_JIT_ENGINE_API hot_jit_engine* hot_jit_engine_create(int debugger_support, const void* const* host_runtime,
    size_t n_host_runtime, int* out_debugger_on, char** out_error, char** out_debugger_error);
// Call this only after hot_jit_remove of each dylib of the engine.
HOT_JIT_ENGINE_API void hot_jit_engine_free(hot_jit_engine* engine);

// Adds "ir" to a new JITDylib "id" and runs the static initializers. The call takes "ir" also
// on failure. The absolute symbols ("abs_names", "abs_values") are defined in the dylib first.
// Returns null and sets "*out_error" on failure.
HOT_JIT_ENGINE_API hot_jit_dylib* hot_jit_add(hot_jit_engine* engine, const char* id, hot_jit_ir* ir,
    const char* const* abs_names, const uint64_t* abs_values, size_t n_abs, char** out_error);

// Adds the relocatable object "data" (an object of hot_jit_dylib_object, also from another engine
// or process) to a new JITDylib "id" and runs its static initializers. The object must come from
// the same compiler and target. The absolute symbols are defined first, as in hot_jit_add. Returns
// null and sets "*out_error" on failure. After a failure no static initializer of the object
// ran: the generic platform of LLJIT links the object (and returns a link error) before it calls
// the first initializer. So the caller can compile the source and load it again.
HOT_JIT_ENGINE_API hot_jit_dylib* hot_jit_add_object(hot_jit_engine* engine, const char* id, const void* data,
    size_t size, const char* const* abs_names, const uint64_t* abs_values, size_t n_abs, char** out_error);

// With "on", each later hot_jit_add of the engine keeps a copy of the object that ORC makes from
// the IR. The object refers to the host symbols and the absolute symbols by name.
HOT_JIT_ENGINE_API void hot_jit_capture_objects(hot_jit_engine* engine, int on);

// The object of a dylib of hot_jit_add, when the engine captures the objects. Returns 1 and sets
// "*out_data" and "*out_size". The bytes stay valid until hot_jit_remove. Returns 0 when there is
// no object, for example for a dylib of hot_jit_add_object.
HOT_JIT_ENGINE_API int hot_jit_dylib_object(const hot_jit_dylib* dylib, const void** out_data, size_t* out_size);

// Releases the copy of the object of the dylib. Then hot_jit_dylib_object returns 0. The code
// of the dylib stays.
HOT_JIT_ENGINE_API void hot_jit_dylib_release_object(hot_jit_dylib* dylib);

// The address of the symbol "name" in the dylib. Null and "*out_error" when it is not there.
HOT_JIT_ENGINE_API void* hot_jit_lookup(hot_jit_dylib* dylib, const char* name, char** out_error);

// The time of the last hot_jit_add of the dylib, in milliseconds: the module add, and the
// static initializers (ORC makes the machine code and links it there).
HOT_JIT_ENGINE_API void hot_jit_add_timing(const hot_jit_dylib* dylib, double* add_ms, double* init_ms);

// Runs the static destructors of the dylib and removes its code.
HOT_JIT_ENGINE_API void hot_jit_remove(hot_jit_dylib* dylib);

#ifdef __cplusplus
}
#endif
