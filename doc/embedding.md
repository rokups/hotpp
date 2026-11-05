---
layout: default
title: Embedding
nav_order: 28
group: Embedding
---

The host loads scripts with a `Loader`. A development build compiles them. An AOT build links them.

This guide has seven hosting situations. This page helps you choose one. Each situation has a sample project. The page [CMake](cmake.html) has the build side: the targets, the options, `hot_add_host` and `hot_add_scripts`. A release build needs the section [Portable binaries](cmake-sdk.html#portable-binaries) there.

## Which situation fits?

Answer these questions in order. Stop at the first answer that fits. The diagram shows the questions. The numbered list below has the same steps in text.

<pre class="mermaid">
flowchart TD
Q1{"Must the shipped program have no compiler and no loader?"}
Q1 -->|Yes| AOT["AOT"]
Q1 -->|No| Q1b{"Do you want an AOT start with per-script overrides?"}
Q1b -->|Yes| AOTR["AOT with run-time overrides"]
Q1b -->|No| Q2{"Do the scripts change after the build, but you build them ahead of time?"}
Q2 -->|Yes| PRE["Precompiled library"]
Q2 -->|No| Q3{"Does the program read .hot text at run time and compile it?"}
Q3 -->|No| BACK["Check the first two answers again"]
Q3 -->|Yes| Q4{"Must a live object keep its field values while its code changes?"}
Q4 -->|Yes| HR["Hot reload"]
Q4 -->|No| Q5{"Can you link LLVM and Clang, and do you want no compiler process?"}
Q5 -->|Yes| JIT["In-process JIT"]
Q5 -->|No| Q6{"Do you want to set the compiler, the flags and the folders in your own code?"}
Q6 -->|Yes| EXP["Explicit Clang files backend"]
Q6 -->|No| RT["Compile at run time"]
</pre>

1. Must the shipped program have no compiler, no `libclang` and no loader? Then the scripts are fixed at build time. Use **AOT**. If the shipped program may carry a loader and a compiler, and you want an AOT start with per-script overrides, use **AOT with run-time overrides**.
2. Do the scripts change after the program is built, but you build them ahead of time (with the build or with `hotc`)? Use a **precompiled library**.
3. Does the program read `.hot` text at run time and compile it? Then ask the next two questions.
4. Must a live object keep its field values while its code changes? Use **hot reload**.
5. Do you want to avoid a compiler process, and can you link LLVM and Clang? Use the **in-process JIT**.
6. Do you want to set the compiler, the include folders, the flags and the work folder in your own code? Use the **explicit Clang files backend** (or the explicit JIT backend).
7. Else use **compile at run time** with `make_default_backend`. The program picks the backend at run time (see [Compile at run time](embedding-runtime.html#compile-at-run-time)).

| Situation | Sample |
|---|---|
| AOT | [aot](https://github.com/rokups/hotpp/tree/master/samples/aot) |
| AOT with run-time overrides | [aot-reload](https://github.com/rokups/hotpp/tree/master/samples/aot-reload) |
| Precompiled library | [precompiled-library](https://github.com/rokups/hotpp/tree/master/samples/precompiled-library) |
| Compile at run time | [runtime-compile](https://github.com/rokups/hotpp/tree/master/samples/runtime-compile) |
| Hot reload | [hot-reload](https://github.com/rokups/hotpp/tree/master/samples/hot-reload) |
| Explicit Clang files backend | [clang-files-backend](https://github.com/rokups/hotpp/tree/master/samples/clang-files-backend) |
| In-process JIT | [jit-backend](https://github.com/rokups/hotpp/tree/master/samples/jit-backend) |

The sample [imgui](https://github.com/rokups/hotpp/tree/master/samples/imgui) combines hot reload with a Dear ImGui window. Copy your host from `samples/`. The folder `tests/engine` is a test host for the Hot++ tests. It is not a model for a user project.

## Hosting situations

Pick the row that matches your program. Each sample is a small project that builds in the Hot++ tree and as a separate project.

| Situation | Links | Key API | Sample |
|---|---|---|---|
| Compile at run time | `hot_backend` | `make_default_backend` | [runtime-compile](https://github.com/rokups/hotpp/tree/master/samples/runtime-compile) |
| Precompiled library | `hot_runtime` | `module_from_library` | [precompiled-library](https://github.com/rokups/hotpp/tree/master/samples/precompiled-library) |
| Hot reload | `hot_backend` | `request_reload`, `commit_pending`, `frame_boundary` | [hot-reload](https://github.com/rokups/hotpp/tree/master/samples/hot-reload) |
| In-process JIT | `hot_jit_backend` (needs `HOT_ENABLE_JIT`) | `JitBackend` | [jit-backend](https://github.com/rokups/hotpp/tree/master/samples/jit-backend) |
| Explicit Clang files backend | `hot_clang_backend` | `ClangFilesBackend` | [clang-files-backend](https://github.com/rokups/hotpp/tree/master/samples/clang-files-backend) |
| AOT | `hot::aot` (through `hot_add_host` with `MODE AOT`) | `AotRegistry` | [aot](https://github.com/rokups/hotpp/tree/master/samples/aot) |
| AOT with run-time overrides | `hot::backend` (through `hot_add_host` with `MODE AOT_RELOAD`) | `Loader::load_aot`, `Backend::seed_slots`, `Loader::revert_unit` | [aot-reload](https://github.com/rokups/hotpp/tree/master/samples/aot-reload) |

### Comparison

| Situation | Compiler at run time | Hot reload | Cost at start | Link targets | Notes |
|---|---|---|---|---|---|
| AOT | No | No | None. `AotRegistry` registers the classes. | `hot_aot` through the AOT library | No loader, no Clang, no `ENABLE_EXPORTS`. |
| AOT with run-time overrides | Yes, for an override only | Yes, for each script | `load_aot` registers the compiled-in classes. An override costs one translate and one compile. | `hot_backend` and the AOT library | `ENABLE_EXPORTS`. One `Loader` in the process. Never link `hot::aot`. |
| Precompiled library | No | Yes, when you load a newer generation that you built ahead | `dlopen` of the library | `hot_runtime` | No `libclang` at run time. `ENABLE_EXPORTS`. A writable temp folder (see [Core copies](embedding-rules.html#core-copies)). |
| Compile at run time | `CLANG_FILES`: yes, the compiler of the build. `JIT`: no process, but Clang libraries. | Yes | One translate and one compile for each load. | `hot_backend` | The program picks the backend at run time (`BackendKind`, the environment variable `HOT_BACKEND`). `ENABLE_EXPORTS`. |
| Hot reload | Same as compile at run time | Yes | Same as compile at run time | `hot_backend` | The host calls `request_reload`, `commit_pending` and `frame_boundary`. |
| In-process JIT | No process. The program links LLVM and Clang. | Yes | No process start. About 140 ms less than `CLANG_FILES` for one load at `-O1` (see below the table). | `hot_jit_backend` | `HOT_ENABLE_JIT`. `ENABLE_EXPORTS`. |
| Explicit Clang files backend | Yes. `ClangFilesOptions::compiler`. | Yes | One compiler process for each load. | `hot_clang_backend` | You set every option. `ENABLE_EXPORTS`. |

The load numbers come from the benchmark in `README.md` (section "JIT backend"). The run-time cost is in [Benchmarks](benchmarks.html). The tool `tools/bench/bench_backends` measures all backends in one run. The author measured them on one machine. A translate with a new `TranslatorSession` takes about 406 ms, because `libclang` parses the headers. A translate with the same session takes about 1.3 ms. A warm load with `CLANG_FILES` at `-O1` and no precompiled header takes about 1.1 s. The JIT is about 140 ms (15%) faster at the same `-O1`. The load numbers use `-O1` and the test headers, which are small. Measure again with your own headers. The code of a script runs at the same speed in every backend.

Linux and Windows (MSVC) are the tested platforms. macOS is untested. `src/runtime/module.cpp` has a Windows branch (`LoadLibraryA`). `make_default_backend` also works on Windows.

## Launcher

The library `hot_launcher` (`hot/launcher.h`, CMake target `hot::launcher`) loads an entry script and calls its `main`. It needs a run-time backend. Each project builds its own runner. The runner links the engine of the project and `hot_launcher`. Hot++ ships no runner program. The sample `samples/launcher` and the launcher tests have a small runner. Set `ENABLE_EXPORTS` and call `hot_export_host` for the runner target, as the other hosts do.

```cpp
hot::launcher::Options options;            // include_dirs, extra_flags, std_version, backend, work_dir, log
std::string error;
auto program = hot::launcher::load(options, "game.hot", error);
if (!program) { /* error has the diagnostics */ }
int code = hot::launcher::run_main(*program, args);
```

- `load` compiles the entry script with all imports (`using "x.hot";`) and loads the module. A relative import path is relative to the folder of the entry script. On an error it returns null and sets `error`.
- `run_main` calls `main` with `argv[0]` set to the script path, then `args`. It returns the result of `main`. It returns 1 and logs the message when the script throws a C++ exception. It returns 2 when the script has no `main`.
- `Program` is move-only. It owns the backend, the loader and the loaded generation. `dependencies()` lists the imported scripts and the native headers. A later reload milestone uses the list.
- `Options::backend` is `""`, `"files"` or `"jit"`. `Options::log` receives the diagnostics (default: stderr).
- `Options::work_dir` is the folder for the compiled files. When it is empty, the folder is `default_cache_dir(script)`: `$XDG_CACHE_HOME/hot/<hash>`, or `~/.cache/hot/<hash>`, or `%LOCALAPPDATA%\hot\<hash>` on Windows.
- The backend owns the sub-directory `<work_dir>/p<pid>`. It removes the sub-directory at start and at exit, so do not keep files there. A crash can leave stale `p<pid>` directories.

Usually `main` calls native engine code that runs a blocking loop. The launcher has no frame loop. The program ends when `main` returns. The sample runner is `sample_runner [-I dir]... [--backend=files|jit] file.hot [args...]`.

## Parts of this guide

- [Rules, errors and traceback](embedding-rules.html): ownership, threads, symbol export, error reports and the traceback.
- [Run-time compile and precompiled library](embedding-runtime.html): the two situations that load a library.
- [Reload API](embedding-reload.html): `request_reload`, the status values and the full reload.
- [JIT, clang files and AOT](embedding-backends.html): the JIT, the clang files backend, AOT and AOT with overrides.
- [File system of the host](embedding-vfs.html): the virtual file system for imports and headers.
- [Backend interface and options](embedding-interface.html): the backend interface, the options structs and the CMake pointers.
