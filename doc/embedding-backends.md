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

**Run time:** the backend translates in memory, compiles in memory with the Clang library and loads the code with LLVM ORC. No process starts. The backend writes no file, except the precompiled header of `make_pch` and the caches of `cache_dir`. Each `load` makes a new `JITDylib`. The slot order stays in memory.

**Caches:** with `cache_dir` the backend keeps the translate output in `<cache_dir>/tr` and the machine code of each load in `<cache_dir>/jit/obj` (an `ObjectCache`, Linux only). A load with an object hit does no Clang compile and no code generation: ORC links the cached object and runs its static initializers. The core instance gets the same object a second time. The object key has the translated text, the compile arguments (also the PCH path), the host file system files, the shadow paths and the engine id (`hot_jit_target_id`: the cache format number, the Clang and LLVM versions, the triple, the host CPU, its features and the code generation level). The generation and the name of the `JITDylib` are not in the key. The manifest has the stamps of the files that the compile read on disk, also the system headers and the PCH. Each cached file has a hash of the object. A file with a bad hash, or an object that does not link, is removed and the backend compiles again. `HOT_CACHE=0` turns off both caches. `HOT_TRANSLATE_CACHE=0` turns off only the translate cache. `HOT_CACHE_MAX_MB` sets the size limit of the objects (`JitOptions::cache_max_bytes`, default 4 GiB). `CacheStats` has the object hits and misses in `hits` and `misses`. `compiles()` counts the Clang compiles. `HOT_TIMING` shows `object cache hit` or `object cache miss`. The cache holds machine code that the process runs: keep the directory private to the user, as the cache of the clang files backend. After a crash that can come from a cached object, delete the cache directory or set `HOT_CACHE=0`.

**Requires:** the LLVM and Clang development packages at build time, and their shared libraries at run time. No compiler process. Pass the same flags as the host (for example a sanitizer) in `extra_flags`.

**Failure:** `LoadResult::error` has the compiler diagnostics, or a text such as `cannot make the JIT: ...` or `script_register not found or not linkable: ...`.

**Extra API:** `last_timings()` gives the time of the phases in ms. `make_pch(header_path, out_pch, error)` makes a precompiled header in the process. The PCH keeps the path of the header. `debugger_support()` and `debugger_error()` tell if the debugger plugin runs.

**Sample:** [jit-backend](https://github.com/rokups/hotpp/tree/master/samples/jit-backend).

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

**Run time:** the backend translates in memory, writes a `.cpp` in `work_dir` (or in `generated_dir`, with a stable name), compiles it (without a shell) to a `.o`, links the `.o` to a `.so`, and loads it. Each call makes a new `.so`. The compile command is `<compiler> -std=<standard> -fPIC -c -g -Werror=return-type` (no `-g` when `ClangFilesOptions::debug_info` is false), then `-resource-dir`, `extra_flags`, the `-I` folders, the format flags and `-MD -MF <depfile>` (`compile_object_command` in `include/hot/clang_files_backend.h`). The link command is `<compiler> -shared -fPIC -g`, then the target link flags, `extra_flags`, the `.o` and the host import library (`link_module_command`).

**Object cache:** the object entry has a manifest with the stamp (path, time, size) of each header and of the `.o`. A changed header is a miss. The compiler id has the compiler path, version and stamp, and the variables that clang reads (`CPATH`, `CPLUS_INCLUDE_PATH`, `C_INCLUDE_PATH`, `OBJC_INCLUDE_PATH`, `SDKROOT`, `MACOSX_DEPLOYMENT_TARGET`, `CCC_OVERRIDE_OPTIONS`, `LIBRARY_PATH`, `COMPILER_PATH`, the MSVC variables `INCLUDE`, `EXTERNAL_INCLUDE`, `LIB`, `VCToolsInstallDir`, `VCINSTALLDIR`, `WindowsSdkDir`, `WindowsSDKVersion`, and `IPHONEOS_DEPLOYMENT_TARGET`, `TVOS_DEPLOYMENT_TARGET`, `WATCHOS_DEPLOYMENT_TARGET`, `XROS_DEPLOYMENT_TARGET`). A header with a time after the start of the translate minus 2 seconds can change during the compile. Then the backend writes no entry, and the next build compiles again. A `.o` that does not match the stamp in its manifest (the file of another process) is a miss. The key also has a hash of the shadow paths of the translate. A new header that shadows a header of a later `-I` folder changes the header search, so the key is different.

**Module cache:** with `cache_dir` the backend also keeps the linked `.so` in `<cache_dir>/obj/<hex>.so`, with a small manifest `<hex>.json` that has the time and the size. The key has the object key, the link command, the compiler id and the stamp (path, time, size) of `host_import_library`. The module key has no header data, so the backend reads the module entry only after an object hit, and stores it only with an object entry. A hit copies the cached file to the work path and runs no compiler process. The backend never loads the cache file. A cached `.so` that does not load is removed, and the backend links it once more. The environment variable `HOT_LINK_CACHE=0` turns only this level off. `HOT_CACHE=0` turns off all the caches. `HOT_TIMING` shows `link cached` for a hit. `CacheStats` has `module_hits` and `module_misses`.

**Translate cache:** with `cache_dir` the backend also keeps the translate output in `<cache_dir>/tr/` (`hot/translate_cache.h`). The key has the translator build id, the translate options and the compiler id. The generation is not in the key, unless the text has the generation number (`export_generation`). The manifest has the imported scripts, the header stamps, the shadow paths, the generation of the entry and the digest of the slot lines of the classes. The digest has each generation of a slot manifest line relative to the generation of the translate: older, this generation or later. The translator uses these numbers only in a compare with its generation. Thus an entry hits at a later generation when the relative slot lines are the same, for example a revert to an earlier text. The hit gives the new manifest lines of the unit the number of the lookup generation. A different slot history is a miss, for example a restart without the history of the entry. A shadow path is a path that a header search tried before it found the header. A new file at a shadow path is a miss. The manifest keeps the shadow paths by folder, and folders with the same names share one name list. A lookup tests a folder on the disk once. When the folder is not there, it tests its names only in the host file system. The 2-second rule uses the start of the parse. A translate can use the parse of an earlier translate in the same process. Then the earlier start applies. An imported script is valid when the search finds the same file, and the text has the same hash or the same import view (`hotc::import_view`). The view is the text without the bodies of the class methods, constructors, destructors, accessors and free functions. It also has the position of each item that keeps its place in the importer output, for example a struct, an enum or a constant. Thus a body edit of an imported class keeps the entry of the importer. An importer warning can have a place in an import, for example a field of an imported class. An entry with such a warning gets no hit of this kind, because a body edit can move that place. The hit has the current text of the import, and its source map segments move with the edited bodies. The environment variable `HOT_TRANSLATE_VERIFY=1` checks each hit of this kind and each hit at another generation: the backend translates again and compares the results. A difference is an error that names the first different line. The tests of the cache and of the reloads use this check. The environment variable `HOT_TRANSLATE_CACHE=0` turns only this level off. `CacheStats` has `translate_hits` and `translate_misses`.

**Parallel compiles:** the environment variable `HOT_JOBS` sets how many compiles run at the same time. Unset, invalid or `0` gives the number of hardware threads, capped at 16 (`hot::kDefaultMaxJobs`, minimum 1). A cold JIT start of a large project (87 units with full engine headers) peaked at about 7.7 GiB at 16 jobs and 9.4 GiB at 32 jobs. Lower `HOT_JOBS` on a computer with little RAM. A valid value of `HOT_JOBS` (1 to 4096) always wins, also above 16. `HOT_JOBS=1` is serial. The launcher runs `min(HOT_JOBS, pending units)` compiles at the same time. No thread starts for one unit. `build_jobs()` in `include/hot/backend.h` reads the value.

A batch has three phases:

1. Translate and import discovery run one at a time (`Backend::begin_load`).
2. Compile and link run in parallel (`PendingLoad::compile`).
3. The library opens in name order (`PendingLoad::finish`).

Thread contract for embedders: `begin_load`, `begin_load_many` and `finish` run one at a time, on one thread. `compile` runs on any thread, and many calls can run together. `begin_load_many` takes a list of sources and gives one `PendingLoad` for each, in order. `ClangFilesBackend` translates them on `build_jobs()` threads and returns when all are done. The default calls `begin_load` in a loop. The unit names of one call must differ. The backend does not check the import order, so an import and its importer can be in one call. A host `hot::FileSystem` must permit parallel read calls. Do not call `Loader::commit_pending`, `seed_slots`, `revert_slots` or `set_preludes` between the first `begin_load` and the last `finish`. A `PendingLoad` does not outlive its backend.

A batch stays serial when the host is Windows. A host file system (`opt.fs`) does not make a batch serial: each job writes its mirror files to its own directory `<work_dir>/vfs.job/<process id>.<job number>`, and the job removes it after the compile.

The JIT backend also loads in three phases. `begin_load_many` translates on `build_jobs()` threads and finds the cached objects. `compile` runs the Clang compile to IR, or reads a cached object, with no lock. `finish` adds the code to ORC and runs the static initializers on the thread of the call. So call `finish` on the thread that must run the static initializers. ORC uses one thread. Do not call `make_pch` between the first `begin_load` and the last `finish`. Do not put `-mllvm` in `JitOptions::extra_flags`: these options are global. A host that uses the Clang or LLVM library on its own threads makes the `JitBackend` before it starts these threads: the constructor registers the LLVM targets. On Windows the JIT keeps the serial default.

Effect on the PCH: a cold batch starts all jobs before the PCH build ends. So the first cold batch uses the PCH less. The PCH build can use one more core than `HOT_JOBS`.

Failure rule: after a compile error the other jobs finish and cache their objects. The launcher reports the first failed unit in name order. The old code stays live.

The compiler of the dev build comes from `HOT_CXX` (see `doc/cmake-sdk.md`).

**Precompiled header (PCH):** with `cache_dir` the backend can compile a unit with a PCH of its native headers. The PCH header has the `#include` lines of the start of the translated `.cpp` (the lines before the first line of code), in the same order. So the unit sees the same declarations and macros with and without the PCH. The include lines in the `.cpp` then find their include guards. The PCH header has no comment and no `#line` line: these name the script and the generation. So the identity of the PCH is the include set only, and all the units with the same include set share one PCH. The `.cpp` keeps its `#line` lines, so a diagnostic of the unit names the script line. A compile with the PCH that fails runs again without the PCH, so its diagnostics come from a compile without the PCH. The first miss of an include set starts the PCH build in a thread (its own compile has no PCH) (the compile command without `-c`, with `-fpch-instantiate-templates -x c++-header`). The compile, the link and the load of that miss do not wait for the PCH build. The next build call or the destructor waits for it, or `wait_for_pch()`. One PCH build runs at a time for each backend. The misses after that compile with `-include-pch`. The PCH is an entry of a second object cache in `<cache_dir>/pch/obj/`, with half the size limit. The trim of that cache runs when the backend starts and after each new PCH. The key has the include text, the normalized flags, the compiler id and the hash of the shadow paths. A new header earlier in the include path changes the shadow paths, so the old PCH does not hide it. The manifest has the header stamps, and the 2-second rule applies to the PCH build. A changed header makes the PCH stale: the next miss builds it again. An object of a compile with the PCH also has the headers of the PCH in its manifest. The object key does not change with the PCH: the PCH has only the inputs of the key. When the compile with the PCH fails, the backend compiles again without it. When that compile works, the PCH entry goes and the PCH is off for one day for that include set (`<hex>.bad`). The marker files are in `<cache_dir>/pch/`: `<hex>.seen` (the last PCH build start), `<hex>.h` (the PCH header) and `<hex>.bad`. The PCH header has a fixed old time (2020-01-01), so two writers make files with the same stamp. The trim removes the markers of a PCH that is not in the cache: a `.seen` or `.bad` file older than 7 days, and a `.h` file without a `.seen` file. The PCH is off for a Windows host, a MinGW or MSVC target, and a translate with mirror files of the host file system (`fs`). It needs a driver whose `--version` names clang. The environment variable `HOT_PCH=0` turns it off. The default backend sets `debug_info` to false, so a run time compile has no `-g`. The environment variable `HOT_DEBUG_INFO=1` turns `-g` on. `HOT_TIMING` prints the time of a PCH build and shows `pch` for a compile with the PCH. The compile database has no `-include-pch`.

**Shared startup PCH:** `begin_load_many` starts one PCH build for the whole batch, before the compile jobs. The PCH header has the union of the `#include` lines of all units, without duplicates, in first-seen order. The unit name of the build is `shared`. The files `<cache_dir>/pch/shared.inc` (the union) and `shared.shadow` (the union of the shadow paths) describe it. A miss of a unit that has no exact PCH uses the shared PCH when each include line of the unit is in `shared.inc`, the order does not matter, and each shadow path of the unit is in `shared.shadow`. Else the unit builds an exact PCH as before, and the new include lines go to `shared.extra`. The next start adds these lines to its union. A build that runs blocks a new shared build. A failed shared build writes the `.bad` marker of the shared key only. `HOT_PCH=0` and mirror files turn it off.

**Compile database:** set `ClangFilesOptions::compile_commands_dir` (`DefaultBackendOptions::compile_commands_dir` for `make_default_backend`). Then the backend writes `<dir>/compile_commands.json` for IDE plugins (`clangd`, `hot-ls`). The environment variable `HOT_COMPILE_COMMANDS_DIR` overrides the option. An empty value turns the database off. The backend needs a `generated_dir` (with `cache_dir` it is `<cache_dir>/src`). Each entry has `file`, the stable generated `.cpp`, and the compile command without `-MD -MF`. Its `output` is the object file in the cache, and `directory` is the cache dir. The backend updates the entry after each good build. It writes the file through a temp file and a rename, and only when the text changes. Before each write it reads the file again and merges it: the entries of other processes stay, and the entries of this backend win. An entry whose `file` does not exist goes. A damaged file is ignored.

**Requires:** the compiler at run time. A work folder that the program can write.

**Failure:** `LoadResult::error` has the diagnostics and `the compile of <script> failed (exit code N)`, or `cannot write <path>`.

**Extra API:** `build(source, generation, out_so, error)` builds a `.so` without loading it (`hotc` uses it). `slots_file()` gives the slot file path. The file keeps the defunct slots of removed functions. `compile_commands_json` and `run_command` are helpers.

**Sample:** [clang-files-backend](https://github.com/rokups/hotpp/tree/master/samples/clang-files-backend).

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

**Sample:** [aot](https://github.com/rokups/hotpp/tree/master/samples/aot).

### Optimized AOT

Optimized AOT is a mode of AOT for a shipped build that never overrides or reloads a script. A call from the host into a script is then as fast as a native virtual call. Add `OPTIMIZED` to the CMake call:

```cmake
hot_add_host(TARGET game MODE AOT OPTIMIZED SCRIPTS player.hot boss.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
```

Without CMake, add `--aot-optimized` to `hotc --aot`. The flag needs `--aot`.

**What it does:** in plain AOT, a call from the host into a script goes through a table that a run-time override can change. Optimized AOT code has no table. The host call goes to the script function directly. Your host code, your script code and the `AotRegistry` calls do not change. The script errors, the error handler and the output are the same as in plain AOT.

The fields of an optimized script object are members of the object. The object has no state block and is not in the list of live objects. `hot::field_ptr` returns null for it. The operators `is` and `as` and `hot::shell_of` work as in the other modes.

A `hot::CommitWindow` does not wait for calls into optimized AOT code. Do not change script state from the host while another thread runs optimized script code.

**When to use:** use it for the shipped build, when no developer or mod author replaces a script at run time. Keep plain AOT (or `AOT_RELOAD`) for the build that needs overrides. Both builds can use the same scripts. The `OPTIMIZED` keyword is the only difference.

**What you lose:** the code cannot be overridden or reloaded. `hot_add_host(... MODE AOT_RELOAD OPTIMIZED ...)` is a configure error. `OPTIMIZED` without `AOT` is a configure error. `Loader::request_reload`, `Loader::revert_unit` and `Loader::load_unit` fail with `BadModule` when the loader holds optimized code. A program has one AOT library, so it cannot link a plain and an optimized library together.

Optimized objects have no field table and no object tracking. This has four effects:

- `hot::field_ptr` and the field reflection (`hot::find_field`) return null for an optimized object.
- `Loader::reset` fails, because the loader does not know the live objects. Exit the program instead.
- The live object counts in `Loader::stats()` are 0 for optimized objects.
- The `onReload` hook and the `convert_` hooks never run (as in plain AOT).

**Script classes in `.cpp` files:** in optimized AOT, the field initializers of a parent class run when the parent part of the object is built. A virtual call through `this` in a field initializer then reaches the function of the parent, not the override of the child. Do not call a virtual function from a field initializer.

**Failure:** a `Loader` that takes the optimized table with `load_aot` works for the classes, but it refuses to override, reload or revert a script. The error text is:

```
the loader holds optimized AOT code (MODE AOT OPTIMIZED): it cannot override or reload a script. Build the scripts without OPTIMIZED
```

Build the scripts without `OPTIMIZED` to fix it.

**Speed:** a host to script call costs about 1.5 ns, against 3.5 ns in plain AOT. A native to script virtual call and a script to script virtual call get cheaper too. `new` of a script object is about 25 ns cheaper for a `.hot` class and about 30 ns cheaper for a `.cpp` class. See [Benchmarks](benchmarks.html#optimized-aot-aot-opt) for the table and the build that gave the numbers.

**Sample:** [aot](https://github.com/rokups/hotpp/tree/master/samples/aot). The targets `sample_aot_hot_opt` and `sample_aot_cpp_opt` build the same scripts as optimized AOT code.

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

**Sample:** [aot-reload](https://github.com/rokups/hotpp/tree/master/samples/aot-reload). The program starts with AOT, overrides `wolf`, and reverts. One object keeps its state in all three phases.
