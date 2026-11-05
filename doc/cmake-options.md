---
layout: default
title: Options and standard
nav_order: 37
group: CMake
---

## Build options and variables

Hot++ has these options that select what the build adds. They are `ON` by default only when Hot++ is the main project (`HOT_TOP_LEVEL`). They are `OFF` by default when your project uses `add_subdirectory` or `FetchContent`.

| Option | Default | Meaning |
|---|---|---|
| `HOT_BUILD_TESTS` | main project: `ON` | Build the Hot++ tests. They need GoogleTest. It adds `tests/`, `editors/vscode` and `tools/bench`. It also adds the test engine dir `tests/engine` to the default include dirs of the backends. |
| `HOT_BUILD_SAMPLES` | main project: `ON` | Build the samples in `samples/`. The targets start with `sample_`. |
| `HOT_RUNTIME_CHECKS` | `ON` | Check null dereference and the bounds of an index (array, `std::vector`, a type with `checked_index`) in scripts. A failed check throws `NullReference` or `IndexOutOfRange`. `OFF` removes the checks: the runtime library defines `HOT_RUNTIME_CHECKS=0` for the host, and the backends pass `-DHOT_RUNTIME_CHECKS=0` to the compile of the scripts. See [Error reports](embedding-rules.html#error-reports). |
| `HOT_THREAD_SAFE` | `ON` | `ON`: any thread can create and destroy script objects. `OFF`: only one thread at a time does this, and the runtime has no lock. `OFF` defines `HOT_THREAD_SAFE=0` for the host, and the backends pass `-DHOT_THREAD_SAFE=0` to the compile of the scripts. See [Thread safety](#thread-safety). |
| `HOT_TRACEBACK` | `OFF` | Record the script call stack. A `ScriptError` keeps it, and `ScriptErrorReport::traceback` gives it. `ON` defines `HOT_TRACEBACK=1` for the host, and the backends pass `-DHOT_TRACEBACK=1` to the compile of the scripts. See [Traceback](embedding-rules.html#traceback). |
| `HOT_VIEW_TYPE` | `hot` | The view type of `T[]`. `hot`: the type is `hot::span<T>` and it works with C++17. `std`: the type is `std::span<T>`. `std` needs C++20, and the configure step fails with a lower `CMAKE_CXX_STANDARD`. `std` defines `HOT_VIEW_STD=1` for the host, and the backends pass `-DHOT_VIEW_STD=1` to the compile of the scripts. `hot_add_scripts` passes `--view=std` to `hotc`. The value is one constant for the whole build. |
| `HOT_PORTABLE` | `OFF` | Keep build paths out of the binaries. See the section [Portable binaries](cmake-sdk.html#portable-binaries). It cannot be `ON` together with `HOT_BUILD_TESTS`: the configure step stops with an error. |
| `HOT_SAMPLES_STANDALONE_TEST` | `OFF` | Add the CTest test `samples_standalone_aot`. The test builds `samples/aot` as a separate project in `samples_standalone/aot` of the build tree. Only `aot` has this test. It works only when `HOT_BUILD_SAMPLES` is `ON`. |

When Hot++ is a subproject, `HOT_BUILD_*` are `OFF`. Set one to `ON` in your cache only if you want Hot++ to build it in your tree. These options work also in a subproject. If `HOT_BUILD_TESTS` is `ON`, the default include dirs of the backends (`HOT_DEFAULT_INCLUDE_DIRS`) also have `tests/engine`.

## Options

Set an option with `-D` on the command line, or with `set(... CACHE ...)` before `add_subdirectory`.

| Option | Default | Meaning and when to change it |
|---|---|---|
| `HOT_ENABLE_CLANG_FILES` | `ON` | `ON` builds the clang files backend into `hot::backend`. It runs an external `clang++`. Set `OFF` when the program must not need a compiler process. |
| `HOT_ENABLE_JIT` | `AUTO` | The JIT backend compiles in memory with the Clang library. `AUTO`: on when CMake finds the LLVM and Clang packages. `ON`: on, and the configure step stops with an error when the packages are missing. `OFF`: off. The JIT backend needs LLVM and Clang. |
| `HOT_BACKEND` | not set | Deprecated. The configure step reads it once and removes it from the cache. `JIT` becomes `HOT_ENABLE_CLANG_FILES=OFF` with `HOT_ENABLE_JIT=ON` (with a warning), unless one of the new options is already in the cache. `CLANG_FILES` (the old default) changes nothing. A later `-D` value of the new options always wins. |
| `HOT_KEEP_TEMP` | `OFF` | `ON` keeps the scratch files of the default backend and of the tests. Change it to read the generated `.cpp` files. The environment variable `HOT_KEEP_TEMP` overrides it at run time: an empty value or `0` means `OFF`, any other value means `ON`. |
| `HOT_USE_FMT` | `OFF` | `ON` selects fmt for string interpolation at any C++ standard. Change it when you want one format library for C++17 and C++20 builds. |
| `HOT_FMT_PROVIDER` | `AUTO` | Where fmt comes from. See the section "The format library". Change it for an offline build, or when your project has its own fmt. |
| `HOT_HOSTS_DIR` | `${CMAKE_SOURCE_DIR}/.cache/hot/hosts` | The folder where `hot_script_host` writes the host files. `hot-ls` reads them from `<root>/.cache/hot/hosts` (or from its `--hosts-dir` argument). Change it only if you start `hot-ls` with another folder. |
| `HOT_CLANG_RESOURCE_DIR` | empty | Only for MSVC. The LLVM release sets it. MSVC has no `-print-resource-dir`. Give the resource folder of a Clang install. On other compilers the root file computes `CLANG_RESOURCE_DIR` from the compiler and ignores this variable. |

### Windows options

These options matter for a Windows target. See [Windows](windows.html).

| Option | Default | Meaning |
|---|---|---|
| `HOT_BUILD_TRANSLATOR` | `ON`; `OFF` in a cross build with no LLVM release | Build the translator (`hotc`, `hot-ls`) and the run-time backends. They need `libclang`. A cross build has no `libclang` for the target, so the default is `OFF`. Set `HOT_LLVM_DIR`, `HOT_LLVM_RELEASES_DIR` or `HOT_DOWNLOAD_LLVM` to get `ON`. |
| `HOT_HOST_HOTC` | empty | The path of a `hotc` that runs on the build host. A cross build with no translator uses it to make the modules. |
| `HOT_DOWNLOAD_LLVM` | `OFF` | `ON` downloads the LLVM release for Windows (`clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.zst`, 263 MB) and unpacks it. The build checks the SHA256. It needs a Windows target. |
| `HOT_LLVM_DIR` | empty | The folder of an unpacked LLVM release for Windows. No download occurs. |
| `HOT_LLVM_RELEASES_DIR` | empty | A folder with unpacked releases. When `HOT_LLVM_DIR` is empty, the release of the right version in this folder is `HOT_LLVM_DIR`. |
| `HOT_LLVM_URL` | the release 23.1.4 | The URL of the archive. A value of yours overrides the default. Set `HOT_LLVM_SHA256` too. |
| `HOT_LLVM_SHA256` | the hash of the release 23.1.4 | The SHA256 of the archive at `HOT_LLVM_URL`. |
| `HOT_SCRIPT_COMPILER` | `CMAKE_CXX_COMPILER` | The compiler that compiles the script modules at run time. With an LLVM release it is the `clang++.exe` of the release. |
| `HOT_HOTC_TARGET` | empty | The target triple of the script modules (`--target`). The MinGW toolchain file sets `x86_64-w64-mingw32`. An empty value adds no flag. |
| `HOT_SCRIPT_SYSROOT` | empty | The sysroot of the script modules (`--sysroot`). The MinGW toolchain file sets `/usr/x86_64-w64-mingw32`. An empty value adds no flag. |

These variables are not options but they matter:

| Variable | Meaning |
|---|---|
| `CMAKE_CXX_STANDARD` | The standard of the host and of the scripts. Default 17. See below. |
| `CMAKE_CXX_FLAGS` | The default backend passes it to every script compile (split into arguments). Sanitizer flags go here. |
| `CMAKE_CXX_COMPILER` | The default backend, `hotc` and the language server use this compiler for the scripts. |
| `CPM_*` | See [Offline builds](cmake-setup.html#offline-builds). |
| `HOT_INCLUDE_DIR` | An `INTERNAL` cache variable. It is the `include` folder of Hot++. `hot_add_scripts` puts it first in the `-I` list. (At run time, a portable host reads an environment variable with the same name. See [Portable binaries](cmake-sdk.html#portable-binaries).) |
| `HOT_SCRATCH_DIR` | `${CMAKE_BINARY_DIR}/tmp`. The scratch root. `HOT_KEEP_TEMP` keeps it. |

The default backend writes its files to `${HOT_SCRATCH_DIR}/hot_work/p<pid>`. It removes this folder at exit, unless `HOT_KEEP_TEMP` keeps it.

The presets `linux-clang-files` and `linux-clang-jit` select the backend in this repository. See [Presets](cmake-presets.html#presets).

## Thread safety

`HOT_THREAD_SAFE` is `ON` by default. Any thread can then create and destroy script objects: `new` in script code, `ClassEntry::create`, and the release of the last handle. No host lock is needed. The runtime keeps a list of the live objects for the reload. A small spinlock protects the list, and it costs 5 to 17 ns for each `new` and free, depending on the work around it. See [The cost of `new`](benchmarks.html#the-cost-of-new).

Set `HOT_THREAD_SAFE` to `OFF` when only one thread creates and destroys script objects, and you want the lowest cost. The list has no lock then. If two threads change it at the same time, the result is not defined. A debug build asserts.

The host and the runtime library must use the same value. The CMake targets do this for you, because the runtime library defines `HOT_THREAD_SAFE=0` for the host when the option is `OFF`. The constructor of `hot::Loader` throws `std::runtime_error` when the host differs from the runtime library. The backends pass `-DHOT_THREAD_SAFE=0` to the compile of the scripts when the option is `OFF`. If you compile a script with your own flags, pass the same `-DHOT_THREAD_SAFE=` value. The loader does not check the scripts.

`Loader::stats`, `frame_boundary` and `reset` can run while other threads create and destroy script objects. A reload still needs a safe point: do not create or destroy script objects while `commit_pending` runs (see [Embedding rules](embedding-rules.html#threads)).

## The C++ standard and the format library

The build chooses the standard and the format library for string interpolation `${}`. No runtime switch exists.

The standard is the first of these that has a value:

1. `CMAKE_CXX_STANDARD` in the scope of the caller.
2. The default of Hot++: 17. Hot++ sets it when you set none, and it also keeps the value in a global property so `hot_add_scripts` agrees.
3. 17 (only a fallback).

The standard is then fixed to 17, 20 or 23. A value of 21 or 22 gives 20. A smaller value gives 17. A larger value gives 23. `hot_add_scripts` passes the result to `hotc` as `--std=c++<n>`. The runtime backends use the same value (`HOT_CXX_STANDARD` in `include/hot/config.h`), both for the compile and for the parse of the native headers.

The format library follows from the standard and from `HOT_USE_FMT`:

| `CMAKE_CXX_STANDARD` in your project | `HOT_USE_FMT` | Standard of the scripts | Format library | Flag for `hotc` |
|---|---|---|---|---|
| not set | `OFF` or `ON` | 17 | fmt | `--format=fmt` |
| 17 | `OFF` or `ON` | 17 | fmt | `--format=fmt` |
| 20 | `OFF` | 20 | `std::format` | none |
| 20 | `ON` | 20 | fmt | `--format=fmt` |
| 21 or 22 | `OFF` | 20 | `std::format` | none |
| 23 | `OFF` | 23 | `std::format` | none |
| 23 | `ON` | 23 | fmt | `--format=fmt` |
| less than 17 | any | 17 | fmt | `--format=fmt` |
| more than 23 | `OFF` | 23 | `std::format` | none |

`std::format` needs C++20, so a standard below 20 always selects fmt. When the build uses fmt, an AOT library links `hot_fmt`. A default-mode compile gets `-DFMT_HEADER_ONLY` and, if needed, `-I<fmt include dir>`.

Your own targets keep the default of the compiler when you set no standard. Set `CMAKE_CXX_STANDARD` before `add_subdirectory`. Then your host code, the runtime and the scripts use the same standard.

If you set `CMAKE_CXX_STANDARD` after `add_subdirectory`, the runtime and `hotc` are already built for the old value. Then `hot_add_scripts` can ask `hotc` for another format library. `hotc` stops with the error "`this hotc is built for --format=...`". See the troubleshooting table.

The default standard is 17, so string interpolation uses fmt by default. To use `std::format`, set `CMAKE_CXX_STANDARD` to 20 or 23 before `add_subdirectory`. `hotc`, the language server and the tests build with the same standard as the rest. The header `include/hot/config.h` has the macros `HOT_USE_FMT` and `HOT_CXX_STANDARD`.

### The format library

The cache variable `HOT_FMT_PROVIDER` says where fmt comes from:

| Value | Meaning |
|---|---|
| `AUTO` (default) | `find_package(fmt)`. If fmt is not installed, `CPM.cmake` fetches fmt. |
| `SYSTEM` | `find_package(fmt REQUIRED)`. The build never fetches. |
| `CPM` | `CPM.cmake` always fetches fmt (version 11.2.0). |
| `NONE` | Your project makes the target `fmt::fmt-header-only` or `fmt::fmt`. Hot++ only links to it. The configure step fails if the target does not exist. |

A target `fmt::fmt-header-only` or `fmt::fmt` that exists already is always used. Then the build does not search and does not fetch. The configure step prints the choice, for example `String interpolation uses: fmt`, and the source of fmt.
