---
layout: default
title: JIT, clang files and AOT
nav_order: 32
group: Embedding
---

## In-process JIT

**Links:** `hot_jit_backend`. It brings `hot_runtime`, `hotc_lib`, `clang-cpp` and `LLVM`. Set `ENABLE_EXPORTS`. The target exists only when the build has the JIT backend (`HOT_ENABLE_JIT` is `ON`, or `AUTO` and CMake finds LLVM and Clang).

```cpp
hot::JitOptions options;
options.compiler = "/usr/bin/clang++";             // the driver name; an absolute path is best
options.include_dirs = {HOT_INCLUDE_DIR, "engine"};
options.resource_dir = RESOURCE_DIR;                // <compiler> -print-resource-dir
hot::JitBackend backend(std::move(options));        // declare before the loader
hot::Loader loader;
hot::LoadResult built = backend.load({"player.hot", text}, 1);
```

**Run time:** the backend translates in memory, compiles in memory with the Clang library and loads the code with LLVM ORC. No process starts. The backend writes no file, except the precompiled header of `make_pch`. Each `load` makes a new `JITDylib`. The slot order stays in memory.

**Requires:** the LLVM and Clang development packages at build time, and their shared libraries at run time. No compiler process. Pass the same flags as the host (for example a sanitizer) in `extra_flags`.

**Failure:** `LoadResult::error` has the compiler diagnostics, or a text such as `cannot make the JIT: ...` or `script_register not found or not linkable: ...`.

**Extra API:** `last_timings()` gives the time of the phases in ms. `make_pch(header_path, out_pch, error)` makes a precompiled header in the process. The PCH keeps the path of the header. `debugger_support()` and `debugger_error()` tell if the debugger plugin runs.

**Sample:** [jit-backend](https://github.com/rokups/hotpp/tree/main/samples/jit-backend).

## Explicit Clang files backend

**Links:** `hot_clang_backend`. It brings `hot_runtime` and `hotc_lib`. Set `ENABLE_EXPORTS`.

```cpp
hot::ClangFilesOptions options;
options.compiler = "clang++";
options.include_dirs = {HOT_INCLUDE_DIR, "engine"};
options.extra_flags = {"-fsanitize=address"};
options.work_dir = "build/hot_work";
options.remove_work_dir_at_exit = true;
hot::ClangFilesBackend backend(std::move(options));   // declare before the loader
hot::Loader loader;
hot::LoadResult built = backend.load({"player.hot", text}, 1);
```

**Run time:** the backend translates in memory, writes a `.cpp` in `work_dir`, runs the compiler (without a shell) to make a `.so`, and loads it. Each call makes a new `.so`. The compile command is `<compiler> -std=<standard> -fPIC -shared -g -Werror=return-type`, then `-resource-dir`, `extra_flags`, the `-I` folders and the format flags (`compile_command` in `include/hot/clang_files_backend.h`).

**Requires:** the compiler at run time. A work folder that the program can write.

**Failure:** `LoadResult::error` has the diagnostics and `the compile of <script> failed (exit code N)`, or `cannot write <path>`.

**Extra API:** `build(source, generation, out_so, error)` builds a `.so` without loading it (`hotc` uses it). `slots_file()` gives the slot file path. `compile_commands_json` and `run_command` are helpers.

**Sample:** [clang-files-backend](https://github.com/rokups/hotpp/tree/main/samples/clang-files-backend).

## AOT

**Links:** `hot::aot`, through `hot_add_host(... MODE AOT ...)`. `hot_aot` links `hot_runtime_core` only. It has no loader. No `ENABLE_EXPORTS`.

An AOT build has no loader, no reload and no Clang. `hotc --aot` (the AOT mode) translates generation 1 of each script to C++. The code becomes part of the game.

```
hotc --aot player.hot boss.hot --out-dir gen -I include -I tests/engine
```

```cpp
#include "hot/aot.h"

hot::AotRegistry registry(hot::aot_table);   // the main thread; it lives as long as the objects
Actor* boss = registry.create_as<Actor>("Boss");
world.Add(boss);
```

**Run time:** the constructor of `AotRegistry` calls the register function of each script. `has`, `class_names`, `record`, `create`, `create_as`, `thunk`, `class_count` and `script_count` read the registry.

**Requires:** no compiler and no loader at run time. Script names and class names must be unique.

**Limits:** a plain AOT program has generation 1 only. The `onReload` hook and the `convert_` hooks never run. A type error in a script shows when the C++ compiler builds the generated file.

**Failure:** the constructor throws `std::runtime_error` when two scripts have a class of the same name (`the class ... is in two AOT scripts (...)`) or when the initializer of a module variable throws. `create` throws `std::runtime_error` (`no AOT script class ...`) for an unknown class.

**Sample:** [aot](https://github.com/rokups/hotpp/tree/main/samples/aot).

### Optimized AOT

Optimized AOT is a mode of AOT for a shipped build that never overrides or reloads a script. A call from the host into a script is then as fast as a native virtual call. Add `OPTIMIZED` to the CMake call:

```cmake
hot_add_host(TARGET game MODE AOT OPTIMIZED SCRIPTS player.hot boss.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
```

Without CMake, add `--aot-optimized` to `hotc --aot`. The flag needs `--aot`.

**What it does:** in plain AOT, a call from the host into a script goes through a table that a run-time override can change. Optimized AOT code has no table. The host call goes to the script function directly. Your host code, your script code and the `AotRegistry` calls do not change. The script errors, the error handler and the output are the same as in plain AOT.

A `hot::CommitWindow` does not wait for calls into optimized AOT code. Do not change script state from the host while another thread runs optimized script code.

**When to use:** use it for the shipped build, when no developer or mod author replaces a script at run time. Keep plain AOT (or `AOT_RELOAD`) for the build that needs overrides. Both builds can use the same scripts. The `OPTIMIZED` keyword is the only difference.

**What you lose:** the code cannot be overridden or reloaded. `hot_add_host(... MODE AOT_RELOAD OPTIMIZED ...)` is a configure error. `OPTIMIZED` without `AOT` is a configure error. `Loader::request_reload`, `Loader::revert_unit` and `Loader::load_unit` fail with `BadModule` when the loader holds optimized code. A program has one AOT library, so it cannot link a plain and an optimized library together.

**Failure:** a `Loader` that takes the optimized table with `load_aot` works for the classes, but it refuses to override, reload or revert a script. The error text is:

```
the loader holds optimized AOT code (MODE AOT OPTIMIZED): it cannot override or reload a script. Build the scripts without OPTIMIZED
```

Build the scripts without `OPTIMIZED` to fix it.

**Speed:** a host to script call costs about 1.8 ns, against 3.5 ns in plain AOT. A native to script virtual call and a script to script virtual call get cheaper too. `new` of a script object is about 7 ns cheaper. See [Benchmarks](benchmarks.html#optimized-aot-aot-opt) for the table and the build that gave the numbers.

**Sample:** [aot](https://github.com/rokups/hotpp/tree/main/samples/aot). The targets `sample_aot_hot_opt` and `sample_aot_cpp_opt` build the same scripts as optimized AOT code.

## AOT with run-time overrides

**Links:** the AOT script library and `hot::backend`, through `hot_add_host(... MODE AOT_RELOAD ...)`. `ENABLE_EXPORTS` is on. Do not link `hot::aot`.

JIT, Clang files and AOT can all be in one host. AOT is the start state. The user switches per script at run time: a script runs its compiled-in code, runs a run-time override, or goes back to the compiled-in code.

**When to use:** the shipped build is AOT, but a developer or a mod author must replace one script without a rebuild. The program carries a loader and a compiler for this. A program that never overrides a script uses plain AOT.

**Call sequence:**

```cpp
auto backend = hot::make_default_backend(options);   // the same includes and flags as the AOT build
hot::Loader loader;                                  // one Loader in the process
backend->seed_slots(hot::aot_table.slots);           // the override keeps the AOT slot numbers
loader.load_aot(hot::aot_table);                     // start: each AOT script is one unit

// Override one script from source.
hot::LoadResult built = backend->load({path, text, {}}, hot::active_generation + 1);
built.module->unit = "wolf";                         // the unit is the name of the AOT script
loader.request_reload(std::move(*built.module));     // validate
loader.commit_pending();                             // at a safe point of the host
loader.frame_boundary();

// Go back to the compiled-in code.
loader.revert_unit("wolf", *backend);                // stage the AOT code again
loader.commit_pending();
loader.frame_boundary();
```

**What migrates:** a live object of the unit keeps the fields that have the same name and type in both layouts. A new field gets its default. A field that the new layout lacks is dropped. This holds for the override and for the revert. Closures of the old code stop (the stale call is counted and logged). Other units do not change.

**Limits:**

- Optimized AOT code (`MODE AOT OPTIMIZED`) cannot be the start state. The loader refuses an override of it (see [Optimized AOT](#optimized-aot)).
- The override needs a compiler at run time. It must use the same includes and flags as the AOT build. Else the layout or the slot numbers can differ.
- Call `seed_slots` before the first `load`. Without it, a backend numbers the methods from zero, and they can differ from the AOT numbers.
- One `Loader` for each process. `load_aot` registers the AOT classes in it. A second `Loader` in the process starts with the AOT classes again after the first one is gone.
- Never link `hot::aot` and `hot::backend` (or `hot::runtime`) in one program. `MODE AOT_RELOAD` links the second one only.
- The AOT library compiles with hidden symbols. This stops the override library from binding to the AOT code.
- `revert_unit` fails (`BadModule`) when the unit has no AOT module. It returns `Busy` while another reload waits for its commit.
- The AOT thunks carry the signature key of each method. The revert sets these keys again. After a revert, a delegate on the thunk of an override signature detaches: a call logs once and does not run the AOT function. Take the thunk of the AOT signature again to call the method.
- `revert_unit` needs the backend of the override. The backend learns the signatures of an override only when the loader accepts the override. The commit of the revert calls `Backend::revert_slots` with the AOT slot lines of the unit. So a later override of the unit sees the AOT signatures. The backend must live until the commit.
- `HOT_SCRIPT_DIR` is a development define. The override files stay out of `SCRIPTS`.

**Failure:** `load_aot` returns an error when a class is in two units. A failed `load_aot` leaves the `Loader` empty.

**Sample:** [aot-reload](https://github.com/rokups/hotpp/tree/main/samples/aot-reload). The program starts with AOT, overrides `wolf`, and reverts. One object keeps its state in all three phases.
