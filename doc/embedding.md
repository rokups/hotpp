---
layout: default
title: Embedding
nav_order: 13
---

The host loads scripts with a `Loader`. A development build compiles them. An AOT build links them.

This page has six hosting situations. Each situation has a sample project. The page [CMake](cmake.html) has the build side: the targets, the options, `hot_add_host` and `hot_add_scripts`. A release build needs the section "Portable binaries" there.

## Which situation fits?

Answer these questions in order. Stop at the first answer that fits.

1. Must the shipped program have no compiler, no libclang and no loader? Then the scripts are fixed at build time. Use **AOT**.
2. Do the scripts change after the program is built, but you build them ahead of time (with the build or with `hotc`)? Use a **precompiled library**.
3. Does the program read `.hot` text at run time and compile it? Then ask the next two questions.
4. Must a live object keep its field values while its code changes? Use **hot reload**.
5. Do you want to avoid a compiler process, and can you link LLVM and Clang? Use the **in-process JIT**.
6. Do you want to set the compiler, the include folders, the flags and the work folder in your own code? Use the **explicit Clang files backend** (or the explicit JIT backend).
7. Else use **compile at run time** with `make_default_backend`. The build option `HOT_BACKEND` picks the backend.

| Situation | Sample |
|---|---|
| AOT | [aot](https://github.com/rokups/hotpp/tree/main/samples/aot) |
| Precompiled library | [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library) |
| Compile at run time | [runtime-compile](https://github.com/rokups/hotpp/tree/main/samples/runtime-compile) |
| Hot reload | [hot-reload](https://github.com/rokups/hotpp/tree/main/samples/hot-reload) |
| Explicit Clang files backend | [clang-files-backend](https://github.com/rokups/hotpp/tree/main/samples/clang-files-backend) |
| In-process JIT | [jit-backend](https://github.com/rokups/hotpp/tree/main/samples/jit-backend) |

The sample [imgui](https://github.com/rokups/hotpp/tree/main/samples/imgui) combines hot reload with a Dear ImGui window. Copy your host from `samples/`. The folder `tests/engine` is a test host for the Hot++ tests. It is not a model for a user project.

## Hosting situations

Pick the row that matches your program. Each sample is a small project that builds in the Hot++ tree and as a separate project.

| Situation | Links | Key API | Sample |
|---|---|---|---|
| Compile at run time | `hot_backend` | `make_default_backend` | [runtime-compile](https://github.com/rokups/hotpp/tree/main/samples/runtime-compile) |
| Precompiled library | `hot_runtime` | `module_from_library` | [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library) |
| Hot reload | `hot_backend` | `request_reload`, `commit_pending`, `frame_boundary` | [hot-reload](https://github.com/rokups/hotpp/tree/main/samples/hot-reload) |
| In-process JIT | `hot_jit_backend` (needs `HOT_BACKEND=JIT`) | `JitBackend` | [jit-backend](https://github.com/rokups/hotpp/tree/main/samples/jit-backend) |
| Explicit Clang files backend | `hot_clang_backend` | `ClangFilesBackend` | [clang-files-backend](https://github.com/rokups/hotpp/tree/main/samples/clang-files-backend) |
| AOT | `hot::aot` (through `hot_add_host` with `MODE AOT`) | `AotRegistry` | [aot](https://github.com/rokups/hotpp/tree/main/samples/aot) |

### Comparison

| Situation | Compiler at run time | Hot reload | Cost at start | Link targets | Notes |
|---|---|---|---|---|---|
| AOT | No | No | None. `AotRegistry` registers the classes. | `hot_aot` through the AOT library | No loader, no Clang, no `ENABLE_EXPORTS`. |
| Precompiled library | No | Yes, when you load a newer generation that you built ahead | `dlopen` of the library | `hot_runtime` | No libclang at run time. `ENABLE_EXPORTS`. A writable temp folder (see "Core copies"). |
| Compile at run time | `CLANG_FILES`: yes, the compiler of the build. `JIT`: no process, but Clang libraries. | Yes | One translate and one compile for each load. | `hot_backend` | The backend is the one that `HOT_BACKEND` selects. `ENABLE_EXPORTS`. |
| Hot reload | Same as compile at run time | Yes | Same as compile at run time | `hot_backend` | The host calls `request_reload`, `commit_pending` and `frame_boundary`. |
| In-process JIT | No process. The program links LLVM and Clang. | Yes | No process start. About 140 ms less than `CLANG_FILES` for one load at `-O1` (see below the table). | `hot_jit_backend` | `HOT_BACKEND=JIT`. `ENABLE_EXPORTS`. |
| Explicit Clang files backend | Yes. `ClangFilesOptions::compiler`. | Yes | One compiler process for each load. | `hot_clang_backend` | You set every option. `ENABLE_EXPORTS`. |

The numbers come from the benchmark in `README.md` (section "JIT backend"). The author measured them on one machine. A translate with a new `TranslatorSession` takes about 406 ms, because libclang parses the headers. A translate with the same session takes about 1.3 ms. A warm load with `CLANG_FILES` at `-O1` and no precompiled header takes about 1.1 s. The JIT is about 140 ms (15%) faster at the same `-O1`. The test headers are small. Measure again with your own headers.

Linux is the tested platform. Windows and macOS are untested. `src/runtime/module.cpp` has a Windows branch (`LoadLibraryA`). It is marked as not built on MSVC. `src/runtime/default_backend.cpp` includes `unistd.h`, so `make_default_backend` is POSIX only.

## Rules for all situations

### Ownership and lifetime

- The `Loader` owns every `Module`. A module owns its code memory (`keep_alive`): for a `.so` the last copy calls `dlclose`.
- Declare the `Loader` before the script objects. The loader is then destroyed after them. The objects must die before the loader unloads the code. All samples do this.
- Declare the backend before the loader. The samples do this too.
- `Loader::find(name)` returns a `const ClassEntry*` or `nullptr`. `entry->create()` makes an object and returns an `Instance`. `Instance::native` is a pointer to the native base class (for example `Creature`). The caller owns the object. Delete it through the native pointer (the native class needs a virtual destructor). The samples use `std::unique_ptr<Creature>`.
- With AOT, `AotRegistry::create(name)` returns the `Instance` and `create_as<Native>(name)` returns the native pointer. The caller owns the object, or gives it to the engine (for example `World::Add`).
- The pointers from `find()`, `module_state()` and `thunk()` and every function pointer, closure and handle that came from a script module become invalid after `Loader::reset`.
- A retired generation unloads in `frame_boundary()` when its pin count is zero, at least one frame boundary passed, and no code of the module is in use. A live state field that can hold code (`kMayHoldCode`, for example a `std::shared_ptr`) blocks every unload.
- A JIT `Module` keeps the JIT alive, so its code stays valid. For `ClangFilesBackend` the module keeps the loaded library alive.

### Threads

These rules come from the comments in `include/hot/runtime/loader.h`.

- `load`, `frame_boundary`, `stats`, `find` and `thunk` run on the main thread only. Worker threads do not call them.
- `request_reload` and `reload_pending` can run on any thread, also while workers run script code.
- In the default mode (`StopTheWorld`), `commit_pending` runs on the main thread at the frame boundary, when no worker is in script code.
- In `EntryCounters` mode (`Loader::set_sync_mode`), `commit_pending` can run on one thread while other threads call script code. It blocks new script entries and waits for the running ones. It fails when the calling thread is in script code. The mode is global for the process. Change it only when no thread is in script code.
- `Loader::reset` runs on the main thread when no worker runs script code. Never call it from script code.
- `AotRegistry` is made and destroyed on the main thread.
- `ClangFilesBackend` builds one script at a time (it has a mutex for the shared slot file).

### Symbol export

A host that loads a module must export its symbols. The modules resolve the host functions and the runtime functions from the executable.

- Set `ENABLE_EXPORTS` on the executable (`-rdynamic` on Linux). Every sample except `aot` does this.
- Link a static library of your own as a whole archive when scripts call functions that the host code does not use. `samples/imgui/CMakeLists.txt` uses `$<LINK_LIBRARY:WHOLE_ARCHIVE,imgui_sdl3>` for this.
- The executable must link the runtime objects that the script code uses (see the JIT section of `README.md`). The samples link the Hot++ libraries without a whole-archive link.
- An AOT program does not load a module, so it does not export.

### C++17 baseline

The generated code, `include/hot` and the runtime library need C++17 only. A C++17 host works. The standard of the scripts is the standard of the host (17, 20 or 23). A standard below 20 selects fmt for `${}`. See [CMake](cmake.html) for the table.

### Core copies

`module_from_library` makes a second, independent instance of the library for the loader (the "core"). It copies the file to the temp folder as `hot-core-<pid>-<n>-<name>` and removes the copy after `dlclose`. If the temp folder is not available, it uses the folder of the library. If the copy fails (`cannot copy <path> for the core module: ...`), the generation stays loaded and the loader prints `[script] WARNING: no core module for ...` to `stderr`.

## Compile at run time

**Links:** `hot_backend`. It brings `hot_runtime` and the backend that `HOT_BACKEND` selects. Set `ENABLE_EXPORTS`.

**Run time:** the backend turns script text into a `Module`. `CLANG_FILES` is the default. It runs an external `clang++` and loads a `.so`. `JIT` compiles in memory with the Clang library and LLVM ORC. Select one with the CMake option `HOT_BACKEND`.

```
cmake --preset linux-clang-files
cmake --preset linux-clang-jit
```

```cpp
auto backend = hot::make_default_backend();   // declare before the loader
hot::Loader loader;                           // declare before the objects
hot::LoadResult module = backend->load({"player.hot", text}, 1);
if (!module) { /* report module.error and module.reason */ }
hot::ReloadResult loaded = loader.load(std::move(*module.module));
if (!loaded) { /* report loaded.error */ }
const hot::ClassEntry* entry = loader.find("Player");
std::unique_ptr<Actor> player(static_cast<Actor*>(entry->create().native));
```

`make_default_backend` takes the compiler, the include folders and the flags from the build. The folders are the include folder of Hot++ and, with `HOT_BUILD_TESTS` ON, `tests/engine`. The flags are `CMAKE_CXX_FLAGS` at the time of `add_subdirectory`. The `CLANG_FILES` work folder is `<build>/tmp/hot_work/p<pid>`, and the backend removes it at exit unless `HOT_KEEP_TEMP` keeps it. A host adds its own folders and flags with `hot::DefaultBackendOptions` (`include/hot/backend.h`):

```cpp
hot::DefaultBackendOptions options;
options.include_dirs = {"engine"};   // after the build-time include folders
options.extra_flags = {"-DGAME=1"};  // after the build-time flags
auto backend = hot::make_default_backend(options);
```

`make_default_backend()` without an argument is the same as a call with empty options. Both backends use the options.

**Requires:** with `CLANG_FILES`, the compiler of the build must exist on the machine that runs the program. With `JIT`, the program needs the LLVM and Clang libraries and no compiler process.

**Failure:** `LoadResult` has `error` (the compiler diagnostics with script names) and `reason` (`CompileError`, or `SlotsRemoved` when a class has fewer slots). `Loader::load` returns a `ReloadResult` with `Failed` and the reason `BadModule` when the module has no `script_register`, or when the loader already has a generation.

**Sample:** [runtime-compile](https://github.com/rokups/hotpp/tree/main/samples/runtime-compile).

## Precompiled library

**Links:** `hot_runtime`. No backend and no libclang. Set `ENABLE_EXPORTS`.

`hotc` builds a script into a `.so` ahead of time. Any build can load it. In CMake use `hot_add_host` with `MODE PRECOMPILED`, or `hot_add_scripts` for more control. On the command line:

```
hotc player_v1.hot --gen 1 -I include -I tests/engine --out-dir out/
```

```cpp
hot::Loader loader;
hot::LoadResult module = hot::module_from_library("out/player_v1.g1.so");
if (!module || !loader.load(std::move(*module.module))) { /* report */ }
```

**Run time:** `module_from_library` calls `dlopen` and looks up `script_register`. Later generations go through `request_reload`, as in the hot reload case. Build them with `--gen 2` and the same output folder, because the generations share a slot file.

**Requires:** no compiler at run time. The host must export the functions that the script calls (`ENABLE_EXPORTS`). The library must use the same standard, the same flags (for example a sanitizer) and the same format library as the host.

**Failure:** `LoadResult::error` has the text of `dlerror` (for example when a symbol is missing), or `symbol script_register not found in <path>`. `reason` stays `None`.

**Sample:** [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library).

## Reload

**Links:** `hot_backend` (or any backend), or `hot_runtime` with prebuilt generations.

Ask for a reload at any time. Commit it at the frame boundary.

```cpp
// next: a LoadResult of generation 2, from a backend
hot::ReloadResult r = loader.request_reload(std::move(next));   // checks only
// later, at the frame boundary of the main thread:
loader.commit_pending();
loader.frame_boundary();                                        // unloads old generations
```

`request_reload` has two overloads. One takes a `Module`. The other takes a `LoadResult`: a failed result becomes a `ReloadResult` without a call of the validation. A backend failure with the reason `SlotsRemoved` becomes `FullReloadRequired`. Any other failure becomes `Failed`.

`request_reload` returns a `hot::ReloadResult` (`include/hot/runtime/reload_result.h`). It has a `status`, a `reason`, an `error` text and the `classes` involved. The status is one of these:

| Status | Meaning |
|---|---|
| `Applied` | The new generation is active. |
| `Pending` | The module is acceptable. `commit_pending` applies it. |
| `FullReloadRequired` | Hot reload cannot apply the change. The host does a full reload. |
| `Failed` | The module is not acceptable. The live generation continues. |

The result converts to `true` for `Applied` and `Pending`. The reason is one of `None`, `NativeBaseChanged`, `InterfaceBasesChanged`, `ScriptParentChanged`, `ShellLayoutChanged`, `SlotsRemoved`, `CompileError`, `BadModule`, `GenerationNotNewer`, `Busy` and `MigrationFailed`. `to_string` gives the name.

`commit_pending` returns `Applied` when it committed. With nothing pending it returns `Failed` with the reason `None` and an empty error. It returns `Busy` when the commit cannot run now, and `MigrationFailed` when the state does not migrate. If the runtime cannot go on (for example a native move constructor throws during a migration), it calls the handler from `set_restart_handler` and aborts when there is none. When the handler returns, the commit fails, the old generation stays active and `restart_requested()` is true.

**Run time:** the object keeps its field values. The new code runs through the same pointer. The retired generation unloads in `frame_boundary`.

**Requires:** a backend at run time (or prebuilt generations). Each generation number must be higher than the active one, else the reason is `GenerationNotNewer`.

**Failure:** read `status`, `reason` and `error`. For `Failed`, the old generation goes on. For `FullReloadRequired`, do a full reload (below).

**Sample:** [hot-reload](https://github.com/rokups/hotpp/tree/main/samples/hot-reload). See also [Hot reload](hot-reload.html) for the changes that a reload accepts.

### Full reload

For `FullReloadRequired`, the host does these steps:

1. Save the host state.
2. Drop all script objects, handles, closures and thunk pointers.
3. Call `loader.reset(error)` on the same `Loader`.
4. Call `backend->load(source, 1, hot::LoadMode::Fresh)`.
5. Call `loader.load(module)`.
6. Restore the host state.

The host does the serialization. The script has no hook for it. `reset` returns `false` and changes nothing when objects remain. The error is, for example, `2 live objects remain: Player x1, Weapon x1`. It also fails when a reload is pending or a generation is pinned. The check for a call from script code is exact only in EntryCounters mode. The host must not call `reset` from script code. After `reset`, pointers from `find()`, `module_state()` and `thunk()` are invalid. The runtime does not see a change in a native header: the host starts a full reload itself. `tests/engine/Harness.cpp` (`full_reload`) shows the full reload sequence. It is a test host: for a user project, start from `samples/hot-reload`.

## In-process JIT

**Links:** `hot_jit_backend`. It brings `hot_runtime`, `hotc_lib`, `clang-cpp` and `LLVM`. Set `ENABLE_EXPORTS`. The target exists only when `HOT_BACKEND` is `JIT`.

```cpp
hot::JitOptions options;
options.compiler = "/usr/bin/clang++";             // the driver name; an absolute path is best
options.include_dirs = {HOT_INCLUDE_DIR, "engine"};
options.resource_dir = RESOURCE_DIR;                // <compiler> -print-resource-dir
hot::JitBackend backend(std::move(options));        // declare before the loader
hot::Loader loader;
hot::LoadResult built = backend.load({"player.hot", text}, 1);
```

**Run time:** the backend translates in memory, compiles in memory with the Clang library and loads the code with LLVM ORC. No process starts. The backend writes no file, except the precompiled header of `make_pch`. Each `load` makes a new JITDylib. The slot order stays in memory.

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

**Failure:** the constructor throws `std::runtime_error` when two scripts have a class of the same name (`the class ... is in two AOT scripts (...)`) or when the initializer of a module variable throws. `create` throws `std::runtime_error` (`no AOT script class ...`) for an unknown class.

**Sample:** [aot](https://github.com/rokups/hotpp/tree/main/samples/aot).

## The backend interface

`include/hot/backend.h`, `include/hot/module.h`.

| Type | Role |
|---|---|
| `Backend` | The abstract class. `load(source, generation, mode)` makes the `Module` of one script for one generation. `name()` gives `CLANG_FILES` or `JIT`. |
| `ScriptSource` | The input of `load`. |
| `LoadMode` | `Reload` (default) continues the slot numbering. `Fresh` ignores the earlier slot state and builds a library with a new name. A full reload uses `Fresh` after `Loader::reset`. |
| `LoadResult` | A `Module` (`module`), or an error. |
| `Module` | A loadable generation. |
| `Loader` | Registers modules, reloads, unloads. |
| `ClassEntry` | `record` (the class record) and `create` (a factory that returns an `Instance`). |

Fields of `ScriptSource`:

| Field | Meaning |
|---|---|
| `name` | The script name or path. The diagnostics and the `#line` directives use it. The output name uses its file stem. |
| `text` | The content of the script. |
| `modules` | The imported scripts (`using "parent.hot";`) as pairs of the path in the `using` line and the text. One generation holds them all. A path that is not here is read from disk. |

Fields of `LoadResult`:

| Field | Meaning |
|---|---|
| `module` | `std::optional<Module>`. `operator bool` is true when it has a value. |
| `error` | The diagnostics or the error text. Empty on success. |
| `reason` | A `ReloadReason`. A backend failure is `CompileError`. `SlotsRemoved` means that a class has fewer slots: only a full reload applies the change. Default `None`. |

Fields of `Module`:

| Field | Meaning |
|---|---|
| `register_fn` | `void (*)(Registry&)`. The `script_register` function. |
| `keep_alive` | `shared_ptr<void>`. The code stays valid while a copy exists. |
| `origin` | A name for messages, for example the library path. |
| `make_core` | A function that makes a second instance of the code (the core). Empty: the module cannot make one. |
| `on_accept` | The loader calls it when the module is live. A backend uses it to write the slot file only for a live module. |

`Backend::load` does not touch the live generation. On failure the result has an `error` and a `reason`.

`hot::module_from_library(path)` builds a `Module` from a shared library (`dlopen` and `dlsym`). The loader calls `Loader::load` for the first module, `request_reload` and `commit_pending` for the next ones, and `reset` for a full reload. `Loader` also has `stats()`, `stats_text()`, `set_sync_mode`, `sync_mode`, `reload_pending`, `find`, `module_state` and `thunk`.

### Options structs

`DefaultBackendOptions` (`include/hot/backend.h`):

| Field | Default | Meaning |
|---|---|---|
| `include_dirs` | empty | Added after the build-time include folders (`HOT_DEFAULT_INCLUDES`). |
| `extra_flags` | empty | Added after the build-time flags (`HOT_DEFAULT_CXXFLAGS`). |

`ClangFilesOptions` (`include/hot/clang_files_backend.h`):

| Field | Default | Meaning |
|---|---|---|
| `compiler` | `"clang++"` | The compiler command. |
| `include_dirs` | empty | For the script imports and for the compile. |
| `resource_dir` | empty | The Clang resource folder. Empty: the default of the compiler. |
| `std_version` | empty | For example `"c++17"`. Empty: the standard of the host (`hot::cxx_std_name()`). |
| `session` | empty | The translator cache. Empty: the backend makes its own. Share one to keep the cache between backends. |
| `extra_flags` | empty | Added to the compile command. |
| `work_dir` | `"hot_work"` | The `.cpp` and `.so` files go here. |
| `slots_file` | empty | Empty: `<work_dir>/slots.txt`. |
| `remove_work_dir_at_exit` | `false` | `true`: the destructor removes `work_dir`. Give a folder that no other backend uses. |
| `strict_borrowed` | `true` | `false`: a borrowed-storage finding is a warning. |
| `raw_diagnostics` | `false` | `true`: the error text is the raw compiler output. `false`: it uses script names. |

`JitOptions` (`include/hot/jit_backend.h`):

| Field | Default | Meaning |
|---|---|---|
| `compiler` | `"clang++"` | The driver name. An absolute path finds the toolchain best. |
| `include_dirs` | empty | For the script imports and for the compile. |
| `resource_dir` | empty | The Clang resource folder. |
| `std_version` | empty | As in `ClangFilesOptions`. |
| `session` | empty | The translator cache. |
| `extra_flags` | empty | Added to the compile command. |
| `pch_path` | empty | An optional precompiled header for `-include-pch`. |
| `strict_borrowed` | `true` | As in `ClangFilesOptions`. |
| `raw_diagnostics` | `false` | As in `ClangFilesOptions`. |
| `debug_flag` | `"-gline-tables-only"` | The debug flag. `"-g"` gives full debug info, but Clang leaks about 270 KB for each failed compile. Empty: no debug info. |
| `debugger_support` | `true` | Register the JIT code with the debugger (GDB JIT interface). |

`DefaultBackendOptions` sets only `include_dirs` and `extra_flags`. The default backend sets the compiler, the resource folder and (for `CLANG_FILES`) the work folder from the build.

## CMake

The page [CMake](cmake.html) describes the targets, the options and the functions `hot_add_host`, `hot_add_scripts` and `hot_script_host`. A complete project is the sample [aot](https://github.com/rokups/hotpp/tree/main/samples/aot).

## Format library for `${}`

The build chooses the format library for string interpolation. The page [CMake](cmake.html) has the rules and the table of `HOT_FMT_PROVIDER`.
