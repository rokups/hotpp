---
layout: default
title: Run-time compile and precompiled library
nav_order: 30
group: Embedding
---

## Compile at run time

**Links:** `hot_backend`. It brings `hot_runtime` and the backends that the build has. Set `ENABLE_EXPORTS`. The target does not exist when both CMake options `HOT_ENABLE_CLANG_FILES` and `HOT_ENABLE_JIT` are off.

**Run time:** the backend turns script text into a `Module`. Two backends exist. The clang files backend runs an external `clang++` and loads a `.so`. The JIT backend compiles in memory with the Clang library and LLVM ORC. The CMake options `HOT_ENABLE_CLANG_FILES` (default `ON`) and `HOT_ENABLE_JIT` (default `AUTO`: on when CMake finds LLVM and Clang) decide which backends the build has. The program picks one at run time.

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

`make_default_backend` takes the compiler, the include folders and the flags from the build. The folders are the include folder of Hot++ and, with `HOT_BUILD_TESTS` ON, `tests/engine`. The flags are `CMAKE_CXX_FLAGS` at the time of `add_subdirectory`. The clang files work folder is `<build>/tmp/hot_work/p<pid>`, and the backend removes it at exit unless `HOT_KEEP_TEMP` keeps it. A host adds its own folders and flags with `hot::DefaultBackendOptions` (`include/hot/backend.h`):

```cpp
hot::DefaultBackendOptions options;
options.include_dirs = {"engine"};   // after the build-time include folders
options.extra_flags = {"-DGAME=1"};  // after the build-time flags
auto backend = hot::make_default_backend(options);
```

`make_default_backend()` without an argument is the same as a call with empty options. Both backends use the options.

**Run-time choice:** `options.kind` is a `hot::BackendKind`. The value `Auto` (the default) works in this order:

1. The environment variable `HOT_BACKEND`, when it is set. The values are `jit` and `files`, in any case. `clang_files` and `clang-files` also mean `files`. Any other value is an error.
2. Else the clang files backend, when the build has it.
3. Else the JIT backend.

The value `ClangFiles` or `Jit` selects that backend and ignores the environment variable. `hot::backend_available(kind)` tells if the build has a backend. Use it before you ask for one.

```cpp
hot::DefaultBackendOptions options;
if (hot::backend_available(hot::BackendKind::Jit))
    options.kind = hot::BackendKind::Jit;   // else keep Auto
auto backend = hot::make_default_backend(options);
```

**Disabled backend:** when you ask for a backend that the build does not have, `make_default_backend` throws `std::runtime_error`. The message names the CMake option, for example `The JIT backend is not in this build. Configure Hot++ with HOT_ENABLE_JIT=ON (it needs the LLVM and Clang packages).`. The same error comes when the environment variable names such a backend.

**Requires:** with the clang files backend, the compiler of the build must exist on the machine that runs the program. With the JIT backend, the program needs the LLVM and Clang libraries and no compiler process. A build with both backends links LLVM and Clang into the program, also when it uses the clang files backend.

**Failure:** `LoadResult` has `error` (the compiler diagnostics with script names) and `reason` (`CompileError`, or `SlotsRemoved` when a class has fewer slots). `Loader::load` returns a `ReloadResult` with `Failed` and the reason `BadModule` when the module has no `script_register`, or when the loader already has a generation.

**Sample:** [runtime-compile](https://github.com/rokups/hotpp/tree/master/samples/runtime-compile).

## Precompiled library

**Links:** `hot_runtime`. No backend and no `libclang`. Set `ENABLE_EXPORTS`.

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

**Sample:** [precompiled-library](https://github.com/rokups/hotpp/tree/master/samples/precompiled-library).
