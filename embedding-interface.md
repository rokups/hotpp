---
layout: default
title: Backend interface and options
nav_order: 34
group: Embedding
---

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
| `fs` | empty | The file system of the host for the script imports and the headers. Empty: the disk only. It must be thread safe (see [Virtual file system](embedding-vfs.html#virtual-file-system)). |
| `kind` | `Auto` | The backend to make (see [Runtime](embedding-runtime.html)). |
| `std_version` | empty | The C++ standard of the scripts, for example `"c++20"`. A script with `async` needs C++20 (see [Async and await](async.html)). Empty: the standard of the host. |

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
| `fs` | empty | The file system of the host for the script imports and the headers. Empty: the disk only. It must be thread safe (see [Virtual file system](embedding-vfs.html#virtual-file-system)). |

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
| `fs` | empty | The file system of the host for the script imports and the headers. Empty: the disk only. It must be thread safe (see [Virtual file system](embedding-vfs.html#virtual-file-system)). |

`DefaultBackendOptions` sets only `include_dirs` and `extra_flags`. The default backend sets the compiler, the resource folder and (for `CLANG_FILES`) the work folder from the build.

## CMake

The page [CMake](cmake.html) describes the targets, the options and the functions `hot_add_host`, `hot_add_scripts` and `hot_script_host`. A complete project is the sample [aot](https://github.com/rokups/hotpp/tree/master/samples/aot).

## Format library for `${}`

The build chooses the format library for string interpolation. The page [CMake](cmake-options.html#the-format-library) has the rules and the table of `HOT_FMT_PROVIDER`.
