---
layout: default
title: CMake
nav_order: 21
---

This page shows how a host project uses Hot++ with CMake. It lists the requirements, the targets, the options and the functions. It also describes portable binaries. It ends with the presets and a table of errors.

The page [Embedding](embedding.html) describes the six hosting situations. Each situation has a sample project. The links to the samples are at the end of this page.

## Quick start

This quick start makes an AOT program. The C++ of the script is in the binary. The program needs no loader and no compiler at run time. Copy `Creature.h`, `script/wolf.hot` and `main.cpp` from [samples/aot](https://github.com/rokups/hotpp/tree/main/samples/aot).

```
myapp/
  CMakeLists.txt
  main.cpp        (from samples/aot)
  Creature.h      (from samples/aot)
  wolf.hot        (from samples/aot/script)
```

This is the complete `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)

# Set the standard and the flags BEFORE add_subdirectory. Hot++ reads them there.
set(CMAKE_CXX_STANDARD 20)

set(HOT_ROOT "/path/to/hotpp" CACHE PATH "The Hot++ source tree")
add_subdirectory(${HOT_ROOT} hot)

add_executable(myapp main.cpp)
# hotc translates wolf.hot to C++. The call links the result to myapp.
hot_add_host(TARGET myapp MODE AOT SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
```

Configure, build and run:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build
./build/myapp
```

The program prints three lines, one for each call of `Tick`. The first configure fetches the Hot++ dependencies with CPM.cmake. Then the build runs `hotc` and compiles the result. Hot++ sets `CMAKE_RUNTIME_OUTPUT_DIRECTORY` only for its own targets. So `hotc` is in `build/bin/` and your program is in `build/`.

For a host that loads scripts, change `MODE` to `HOT_RELOAD` (the host compiles the script at run time), to `PRECOMPILED` (the build makes a script library and the host loads it) or to `AOT_RELOAD` (AOT code at start, run-time overrides later). The call is the same. See the section `hot_add_host`. The cases are in [Embedding](embedding.html).

## Quick start with an installed SDK

You can also use Hot++ as an installed SDK. You do not need the Hot++ source tree. See the section "Install the SDK" for how to make the SDK. The `CMakeLists.txt` of the program is shorter:

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)

find_package(Hot REQUIRED)

add_executable(myapp main.cpp)
hot_add_host(TARGET myapp MODE AOT SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
```

Tell CMake where the SDK is:

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/hot-sdk -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

`find_package(Hot)` defines the same functions and the same targets (`hot::runtime`, `hot::aot`, `hot::backend`, `hot::hotc` and the others) as `add_subdirectory`. It sets `HOT_HOTC_EXECUTABLE` and `HOT_LS_EXECUTABLE` to the paths of the tools. The SDK records which backends it has in the variables `HOT_SDK_CLANG_FILES` and `HOT_SDK_JIT` (`ON` or `OFF`). The target `hot::clang_backend` exists only when the SDK has the clang files backend. The target `hot::jit_backend` exists only when the SDK has the JIT backend. The target `hot::backend` exists when the SDK has at least one of them.

## Requirements

| Item | Requirement |
|---|---|
| CMake | 3.20 or later (`cmake_minimum_required` of the root file). The presets in `CMakePresets.json` need 3.25. |
| Generator | Any generator. The presets use `Ninja Multi-Config`, so they need Ninja. |
| C++ compiler | A Clang C++ compiler. The presets set `clang++`. The configure step runs `<compiler> -print-resource-dir`. The build does not check which compiler you use. Other compilers are untested. |
| C++ standard | The host can use C++17, C++20 or C++23. The tool `hotc`, the language server and the tests always build as C++20, so the compiler must support C++20. |
| libclang | The C API (`libclang`) and the header `clang-c/Index.h`, from the system. The configure step first runs `find_package(Clang CONFIG QUIET)` and links the imported target `libclang`. The package must have the same major version as the compiler that sets the Clang resource folder (`CLANG_RESOURCE_DIR`), and the step looks first in `<resource folder>/../../cmake/clang`. If the package has no such target or has another version, the step uses the fallback: `find_library(NAMES clang REQUIRED)` and `find_path(clang-c/Index.h REQUIRED)`. The configure step fails if the fallback finds nothing. Every build needs libclang, also an AOT build, because `hotc` links it. |
| Threads | `find_package(Threads REQUIRED)`. |
| LLVM and Clang | Only for the JIT backend. With `HOT_ENABLE_JIT=AUTO` (the default) the configure step runs `find_package(LLVM CONFIG)` and `find_package(Clang CONFIG)`, and the JIT backend is on only when both are found. With `HOT_ENABLE_JIT=ON` the configure step stops with an error when a package is missing. With `OFF` it does not look for them. The JIT backend links `clang-cpp` and `LLVM`. The root file also enables the language C when `HOT_ENABLE_JIT` is not `OFF`. |
| Platform | Linux is the tested platform. Windows and macOS are untested. See [Limits](limits.html). |

A host that links only `hot_runtime` (a precompiled library) or only `hot_aot` has no libclang or Clang at run time. A host that links `hot_backend` needs the libraries of the backend at run time, and `ClangFilesBackend` also needs the compiler (see [Embedding](embedding.html)).

### What CPM fetches

Hot++ uses CPM.cmake (`cmake/CPM.cmake`, in the repository, so CPM itself needs no download). The root file sets `CPM_USE_LOCAL_PACKAGES` to ON when you do not define it. So CPM calls `find_package` first and fetches only a package that is missing.

| Package | Version | When |
|---|---|---|
| lexy | v2025.05.0 | Always. |
| nlohmann_json | 3.12.0 (a release archive) | Always. The language server uses it. |
| GoogleTest | v1.17.0 | Only when `HOT_BUILD_TESTS` is ON. |
| fmt | 11.2.0 | Only when the build uses fmt and no fmt is installed (see `HOT_FMT_PROVIDER`). |
| Dear ImGui | v1.92.9b | Only in the sample `samples/imgui`, and only when `find_package(SDL3)` succeeds (see `README.md`). |

libclang, the Clang compiler, Threads, LLVM and Clang (for JIT) come from the system. CPM never fetches them.

### Offline builds

| Setting | Effect |
|---|---|
| `-DCPM_USE_LOCAL_PACKAGES=OFF` | Fetch every CPM package. Hot++ sets the default to ON. |
| `-DCPM_DOWNLOAD_ALL=ON` | Fetch every CPM package, also when an installed one exists. |
| `-DCPM_LOCAL_PACKAGES_ONLY=ON` | Never fetch. The configure step fails when a package is missing. |
| `-DCPM_SOURCE_CACHE=<dir>` | Keep the fetched sources in one folder. The next configure does not download again. The environment variable `CPM_SOURCE_CACHE` gives the default. |
| `-DCMAKE_DISABLE_FIND_PACKAGE_<name>=ON` | Fetch one package. Use `GTest`, `lexy` or `nlohmann_json` as the name. |

For an offline build, install lexy, nlohmann_json (and GoogleTest, if you build the tests) or fill the cache with `CPM_SOURCE_CACHE` on a machine that has network access. Then set `CPM_SOURCE_CACHE` to that folder.

The fmt package has its own switch. For an offline build with fmt, set `HOT_FMT_PROVIDER` to `SYSTEM` (an installed fmt) or `NONE` (your project makes the fmt target). Both values never fetch.

## Consume Hot++

Add the Hot++ source tree with `add_subdirectory`. The second argument is a build folder, because the tree is outside your source tree.

```cmake
set(HOT_ROOT "/path/to/hotpp" CACHE PATH "The Hot++ source tree")
add_subdirectory(${HOT_ROOT} hot)

add_executable(app main.cpp)
hot_add_host(TARGET app MODE AOT SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
```

The call to `add_subdirectory` defines the functions `hot_add_host`, `hot_add_scripts` and `hot_script_host`. It also defines the targets that the next table lists.

FetchContent gives the same result. Replace the first two lines of the example:

```cmake
include(FetchContent)
FetchContent_Declare(hotpp GIT_REPOSITORY <the URL of the Hot++ repository> GIT_TAG <a tag or a commit>)
FetchContent_MakeAvailable(hotpp)
```

Two variables are read when `add_subdirectory` runs. Set them before the call:

- `CMAKE_CXX_STANDARD`. See the section "The C++ standard and the format library".
- `CMAKE_CXX_FLAGS`. The default backend takes its compile flags from this variable at this time (`HOT_CXXFLAGS_JOINED` in the root `CMakeLists.txt`). A later change has no effect on `make_default_backend`.

### Targets

| Target | Alias | What a host gets |
|---|---|---|
| `hot_runtime` | `hot::runtime` | The loader, the reload, the migration and `module_from_library`. It has no backend. It also has `hot::default_paths()`. |
| `hot_runtime_core` | `hot::runtime_core` | The globals and the registry that generated code needs. The other runtime targets link it. |
| `hot_backend` | `hot::backend` | `make_default_backend` and `backend_available`. It links `hot_runtime` and the backends that the build has. It defines `HOT_WITH_CLANG_FILES` and `HOT_WITH_JIT` for them. The target does not exist when both backends are off. |
| `hot_clang_backend` | `hot::clang_backend` | `ClangFilesBackend`. It runs an external `clang++`. The alias exists when `HOT_ENABLE_CLANG_FILES` is `ON`. (The library `hot_clang_backend` is always built, because `hotc` and `hot-ls` use it.) It links `hot_runtime` and `hotc_lib`. |
| `hot_jit_backend` | `hot::jit_backend` | `JitBackend`. It compiles in memory. It exists only when the JIT backend is on (see `HOT_ENABLE_JIT`). |
| `hot_aot` | `hot::aot` | The AOT registry and the core of the runtime. It has no loader. |
| `hotc` | `hot::hotc` | The tool. `hot_add_host` and `hot_add_scripts` run it at build time. |
| `hot_fmt` | none | An INTERFACE target for fmt. It exists only when the build uses fmt. |

Use the aliases in your project. The real names can change. `hot_add_host` links the right target for you, so you seldom link one by hand.

Do not link `hot::runtime` and `hot::aot` in one program. The library from `hot_add_host` or `hot_add_scripts` with `AOT` links only `hot::runtime_core`. `hot_add_host` with `MODE AOT` adds `hot::aot`. `MODE AOT_RELOAD` adds `hot::backend` and never `hot::aot`.

### Export the host symbols

A host that loads generation modules must export its symbols. This applies to a program that links `hot::runtime`, `hot::backend` or one backend library. `hot_add_host` does this for the modes `HOT_RELOAD`, `AOT_RELOAD` and `PRECOMPILED`. The modules resolve the host functions and the runtime functions from the executable.

```cmake
set_target_properties(app PROPERTIES ENABLE_EXPORTS ON)
```

On Linux this adds `-rdynamic`. All samples that load a module set it. An AOT program does not load a module, so it does not need `ENABLE_EXPORTS`.

A static library of your own can hide a host function from the linker, because the linker drops an object that no code in the executable uses. A script can still call that function. Link such a library as a whole archive, as `samples/imgui/CMakeLists.txt` does:

```cmake
target_link_libraries(app PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,my_native_lib>")
```

`LINK_LIBRARY` needs CMake 3.24. The Hot++ libraries need no whole-archive link in the samples.

The generated code and the headers in `include/hot` need C++17 only.

## Build options and variables

Hot++ has these options that select what the build adds. They are ON by default only when Hot++ is the main project (`HOT_TOP_LEVEL`). They are OFF by default when your project uses `add_subdirectory` or FetchContent.

| Option | Default | Meaning |
|---|---|---|
| `HOT_BUILD_TESTS` | main project: ON | Build the Hot++ tests. They need GoogleTest. It adds `tests/`, `editors/vscode` and `tools/bench`. It also adds the test engine dir `tests/engine` to the default include dirs of the backends. |
| `HOT_BUILD_SAMPLES` | main project: ON | Build the samples in `samples/`. The targets start with `sample_`. |
| `HOT_RUNTIME_CHECKS` | `ON` | Check null dereference and array bounds in scripts. A failed check throws `NullReference` or `IndexOutOfRange`. `OFF` removes the checks: the runtime library defines `HOT_RUNTIME_CHECKS=0` for the host, and the backends pass `-DHOT_RUNTIME_CHECKS=0` to the compile of the scripts. See "Error reports" in [Embedding](embedding.html). |
| `HOT_PORTABLE` | `OFF` | Keep build paths out of the binaries. See the section "Portable binaries". It cannot be ON together with `HOT_BUILD_TESTS`: the configure step stops with an error. |
| `HOT_SAMPLES_STANDALONE_TEST` | `OFF` | Add the CTest test `samples_standalone_aot`. The test builds `samples/aot` as a separate project in `samples_standalone/aot` of the build tree. Only `aot` has this test. It works only when `HOT_BUILD_SAMPLES` is ON. |

When Hot++ is a subproject, `HOT_BUILD_*` are OFF. Set one to ON in your cache only if you want Hot++ to build it in your tree. These options work also in a subproject. If `HOT_BUILD_TESTS` is ON, the default include dirs of the backends (`HOT_DEFAULT_INCLUDE_DIRS`) also have `tests/engine`.

## Options

Set an option with `-D` on the command line, or with `set(... CACHE ...)` before `add_subdirectory`.

| Option | Default | Meaning and when to change it |
|---|---|---|
| `HOT_ENABLE_CLANG_FILES` | `ON` | `ON` builds the clang files backend into `hot::backend`. It runs an external `clang++`. Set `OFF` when the program must not need a compiler process. |
| `HOT_ENABLE_JIT` | `AUTO` | The JIT backend compiles in memory with the Clang library. `AUTO`: on when CMake finds the LLVM and Clang packages. `ON`: on, and the configure step stops with an error when the packages are missing. `OFF`: off. The JIT backend needs LLVM and Clang. |
| `HOT_BACKEND` | not set | Deprecated. The configure step reads it once and removes it from the cache. `JIT` becomes `HOT_ENABLE_CLANG_FILES=OFF` with `HOT_ENABLE_JIT=ON` (with a warning), unless one of the new options is already in the cache. `CLANG_FILES` (the old default) changes nothing. A later `-D` value of the new options always wins. |
| `HOT_KEEP_TEMP` | `OFF` | `ON` keeps the scratch files of the default backend and of the tests. Change it to read the generated `.cpp` files. The environment variable `HOT_KEEP_TEMP` overrides it at run time: an empty value or `0` means OFF, any other value means ON. |
| `HOT_USE_FMT` | `OFF` | `ON` selects fmt for string interpolation at any C++ standard. Change it when you want one format library for C++17 and C++20 builds. |
| `HOT_FMT_PROVIDER` | `AUTO` | Where fmt comes from. See the section "The format library". Change it for an offline build, or when your project has its own fmt. |
| `HOT_HOSTS_DIR` | `${CMAKE_SOURCE_DIR}/.cache/hot/hosts` | The folder where `hot_script_host` writes the host files. `hot-ls` reads them from `<root>/.cache/hot/hosts` (or from its `--hosts-dir` argument). Change it only if you start `hot-ls` with another folder. |
| `HOT_CLANG_RESOURCE_DIR` | empty | Only for MSVC (untested). MSVC has no `-print-resource-dir`. Give the resource folder of a Clang install. On other compilers the root file computes `CLANG_RESOURCE_DIR` from the compiler and ignores this variable. |

These variables are not options but they matter:

| Variable | Meaning |
|---|---|
| `CMAKE_CXX_STANDARD` | The standard of the host and of the scripts. Default 20. See below. |
| `CMAKE_CXX_FLAGS` | The default backend passes it to every script compile (split into arguments). Sanitizer flags go here. |
| `CMAKE_CXX_COMPILER` | The default backend, `hotc` and the language server use this compiler for the scripts. |
| `CPM_*` | See "Offline builds". |
| `HOT_INCLUDE_DIR` | An INTERNAL cache variable. It is the `include` folder of Hot++. `hot_add_scripts` puts it first in the `-I` list. (At run time, a portable host reads an environment variable with the same name. See "Portable binaries".) |
| `HOT_SCRATCH_DIR` | `${CMAKE_BINARY_DIR}/tmp`. The scratch root. `HOT_KEEP_TEMP` keeps it. |

The default backend writes its files to `${HOT_SCRATCH_DIR}/hot_work/p<pid>`. It removes this folder at exit, unless `HOT_KEEP_TEMP` keeps it.

The presets `linux-clang-files` and `linux-clang-jit` select the backend in this repository. See "Presets".

## The C++ standard and the format library

The build chooses the standard and the format library for string interpolation `${}`. No runtime switch exists.

The standard is the first of these that has a value:

1. `CMAKE_CXX_STANDARD` in the scope of the caller.
2. The default of Hot++: 20. Hot++ sets it when you set none, and it also keeps the value in a global property so `hot_add_scripts` agrees.
3. 17 (only a fallback).

The standard is then fixed to 17, 20 or 23. A value of 21 or 22 gives 20. A smaller value gives 17. A larger value gives 23. `hot_add_scripts` passes the result to `hotc` as `--std=c++<n>`. The runtime backends use the same value (`HOT_CXX_STANDARD` in `include/hot/config.h`), both for the compile and for the parse of the native headers.

The format library follows from the standard and from `HOT_USE_FMT`:

| `CMAKE_CXX_STANDARD` in your project | `HOT_USE_FMT` | Standard of the scripts | Format library | Flag for `hotc` |
|---|---|---|---|---|
| not set | OFF | 20 | `std::format` | none |
| not set | ON | 20 | fmt | `--format=fmt` |
| 17 | OFF or ON | 17 | fmt | `--format=fmt` |
| 20 | OFF | 20 | `std::format` | none |
| 20 | ON | 20 | fmt | `--format=fmt` |
| 21 or 22 | OFF | 20 | `std::format` | none |
| 23 | OFF | 23 | `std::format` | none |
| 23 | ON | 23 | fmt | `--format=fmt` |
| less than 17 | any | 17 | fmt | `--format=fmt` |
| more than 23 | OFF | 23 | `std::format` | none |

`std::format` needs C++20, so a standard below 20 always selects fmt. When the build uses fmt, an AOT library links `hot_fmt`. A default-mode compile gets `-DFMT_HEADER_ONLY` and, if needed, `-I<fmt include dir>`.

Your own targets keep the default of the compiler when you set no standard. Set `CMAKE_CXX_STANDARD` before `add_subdirectory`. Then your host code, the runtime and the scripts use the same standard.

If you set `CMAKE_CXX_STANDARD` after `add_subdirectory`, the runtime and `hotc` are already built for the old value. Then `hot_add_scripts` can ask `hotc` for another format library. `hotc` stops with the error "this hotc is built for --format=...". See the troubleshooting table.

`hotc`, the language server and the tests always build as C++20. This does not change the standard of your host. The header `include/hot/config.h` has the macros `HOT_USE_FMT` and `HOT_CXX_STANDARD`.

### The format library

The cache variable `HOT_FMT_PROVIDER` says where fmt comes from:

| Value | Meaning |
|---|---|
| `AUTO` (default) | `find_package(fmt)`. If fmt is not installed, CPM.cmake fetches fmt. |
| `SYSTEM` | `find_package(fmt REQUIRED)`. The build never fetches. |
| `CPM` | CPM.cmake always fetches fmt (version 11.2.0). |
| `NONE` | Your project makes the target `fmt::fmt-header-only` or `fmt::fmt`. Hot++ only links to it. The configure step fails if the target does not exist. |

A target `fmt::fmt-header-only` or `fmt::fmt` that exists already is always used. Then the build does not search and does not fetch. The configure step prints the choice, for example `String interpolation uses: fmt`, and the source of fmt.

## hot_add_host

```
hot_add_host(TARGET name [MODE AOT|AOT_RELOAD|HOT_RELOAD|PRECOMPILED] SCRIPTS a.hot b.hot
             [INCLUDES dir...] [DEPENDS file...] [PRELUDES file...]
             [FLAGS flag...] [HOTC_ARGS arg...])
```

This is the call that most hosts need. Make your program with `add_executable` first. Then call `hot_add_host` with the same name. The call links the Hot++ target that the mode needs, sets `ENABLE_EXPORTS` when the mode needs it, and sets up the scripts. It uses `hot_add_scripts` and `hot_script_host` inside. Call it after `add_subdirectory` of Hot++.

### Keywords

| Keyword | Default | Meaning |
|---|---|---|
| `TARGET` | none, required | The program target. It must exist before the call. |
| `MODE` | `HOT_RELOAD` | How the program gets its scripts. See the next table. |
| `SCRIPTS` | none, required | The script files (`.hot` or `.cpp`). A relative path is relative to the current source folder. Two scripts in one call need different names (the file name without the extension). |
| `INCLUDES` | empty | Include folders. The program, `hotc` and the language server get them. Use absolute paths. |
| `DEPENDS` | empty | More files that the build depends on, for example your headers. The build does not track the headers of a script by itself. |
| `PRELUDES` | empty | Prelude files for `hotc --prelude`. Only for `MODE AOT` and `MODE AOT_RELOAD`. |
| `FLAGS` | empty | Compile flags for the scripts (`PRECOMPILED`) or flags for `hotc` (`AOT`). |
| `HOTC_ARGS` | empty | More arguments for `hotc`. |

A word that is not a keyword, a missing `TARGET` or `SCRIPTS`, and an unknown `MODE` stop the configure step with an error.

### Modes

| Mode | What the build does | What the call links and sets | At run time |
|---|---|---|---|
| `AOT` | `hotc` translates the scripts to C++. The build makes the static library `<TARGET>_scripts`. | The program links `<TARGET>_scripts` and `hot::aot` (PRIVATE). The library links only `hot::runtime_core`. | No loader. No compiler. The scripts are in the binary. |
| `AOT_RELOAD` | `hotc` translates the scripts to C++ as in `AOT`. The build makes the static library `<TARGET>_scripts`. The override files are not in `SCRIPTS`. | The program links `<TARGET>_scripts` and `hot::backend` (not `hot::aot`) and gets `ENABLE_EXPORTS`. A development build also gets the define `HOT_SCRIPT_DIR`, the folder of the first script. | The program starts with `Loader::load_aot(hot::aot_table)`. Then a backend compiles an override of one script at run time, and `Loader::revert_unit` goes back to the AOT code. The host needs a compiler for the override. See "AOT with run-time overrides" in [Embedding](embedding.html). |
| `PRECOMPILED` | `hotc` builds a script library (`.so`) for each script, in `${CMAKE_BINARY_DIR}/hot_scripts/<TARGET>`. The program depends on the target `<TARGET>_scripts`. | The program links `hot::runtime` and gets `ENABLE_EXPORTS`. The variable `HOT_SCRIPTS_<TARGET>_LIBRARIES` has the library paths (in the scope of the caller). | The program loads the libraries with `hot::module_from_library`. No compiler. |
| `HOT_RELOAD` | The build does not compile the scripts. | The program links `hot::backend` and gets `ENABLE_EXPORTS`. A development build also gets the define `HOT_SCRIPT_DIR`, the folder of the first script. | The backend compiles the scripts. The host needs a compiler. The scripts can reload. |

```cmake
add_executable(game main.cpp)
hot_add_host(TARGET game MODE HOT_RELOAD
             SCRIPTS script/player.hot script/boss.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/engine DEPENDS engine/Actor.h)
```

In every mode the program also gets `INCLUDES` as private include folders.

With `HOT_PORTABLE` ON and `MODE HOT_RELOAD`, the call does not define `HOT_SCRIPT_DIR`, because that is a build path. Instead it copies the Hot++ headers to `hot/include` next to the program after each build. The host finds them there at run time. See "Portable binaries". Give the script folder to the host in another way, for example with an argument.

Use `hot_add_scripts` directly when you need more control: more than one generation (`AFTER`, `--gen`), your own `OUT_DIR`, or a script library without a program target. The next section describes it.

## hot_add_scripts

`hot_add_scripts` is the lower-level call. `hot_add_host` calls it.

```
hot_add_scripts(TARGET name SCRIPTS a.hot b.hot
                [INCLUDES dir...] [FLAGS cxxflag...] [DEPENDS file...]
                [OUT_DIR dir] [AFTER target] [HOTC_ARGS arg...])
hot_add_scripts(TARGET name SCRIPTS a.hot b.hot AOT
                [INCLUDES dir...] [PRELUDES file...] [FLAGS hotc-flag...]
                [DEPENDS file...] [OUT_DIR dir] [HOTC_ARGS arg...])
```

The function runs `hotc` at build time. It has two modes. `AOT` selects the second mode. The function also calls `hot_script_host` with the name `TARGET`, so the language server knows the scripts. Call it after `add_subdirectory` of Hot++.

### Keywords

| Keyword | Default | Meaning | Path rule |
|---|---|---|---|
| `TARGET` | none, required | The name of the target. In the default mode it is a custom target. In AOT mode it is a static library. | none |
| `SCRIPTS` | none, required | The script files. A file is a `.hot` script or a `.cpp` script (see [C++ scripts](cpp-scripts.html)). The extension selects the parser. | A relative path is relative to the current source folder. The function makes it absolute. |
| `AOT` | off | A flag. It selects the AOT mode. | none |
| `INCLUDES` | empty | Include folders. `hotc` gets each one as `-I`. In AOT mode the library also gets them as PUBLIC include folders. The language server host gets them too. | Use an absolute path. The function passes the text to `hotc` as it is, and the custom command runs in the current build folder. The AOT include folders and the host file read a relative path from the current source folder. |
| `PRELUDES` | empty | Prelude files for `hotc --prelude`. They work only with `AOT`. The default mode stops with an error. | A relative path is relative to the current source folder. |
| `FLAGS` | empty | In the default mode: compile flags for the scripts (`--cxxflag`). In AOT mode: flags for `hotc`. | none |
| `DEPENDS` | empty | More files that the build depends on. | A relative path is relative to the current source folder (the rule of `add_custom_command`). |
| `OUT_DIR` | default mode: `${CMAKE_BINARY_DIR}/hot_scripts`. AOT: `${CMAKE_BINARY_DIR}/hot_aot/<name>`. | The output folder. | Use an absolute path. A relative path is relative to the current build folder, and then `HOT_SCRIPTS_<TARGET>_LIBRARIES` has relative paths. |
| `AFTER` | none | A target that must build first. It works only in the default mode. AOT mode stops with an error. | none |
| `HOTC_ARGS` | empty | More arguments for `hotc`, in both modes. | none |

The `hotc` call always has the include folder of Hot++ (`HOT_INCLUDE_DIR`) as the first `-I`. You do not need to put it in `INCLUDES`.

The function checks its arguments. A word that is not a keyword stops the configure step with `hot_add_scripts: unknown arguments: ...`.

### What the custom command depends on

The custom command depends on these items:

- the target `hotc`: a new build of the tool runs the command again;
- every file in `SCRIPTS`;
- every file in `DEPENDS`;
- in AOT mode, every file in `PRELUDES`.

The command does not read the headers of the script. The native headers that a script includes with `using "X.h";` are not dependencies. A script that another script imports with `using "parent.hot";` is also not a dependency. List such files in `DEPENDS`, or the build does not run `hotc` when they change. The samples use `DEPENDS Creature.h`.

### Default mode

The function makes one custom target `name`. The target makes the library `OUT_DIR/<script name>.g<n>.so` for each script. The script name is the file name without the last extension. `wolf.hot` gives `wolf.g1.so`. The command also writes the generated `.cpp` files and the slot file `OUT_DIR/slots.txt` there. Load a library with `hot::module_from_library`.

Each library gets the compile flags `-fPIC -shared -g -Werror=return-type` and the standard from the section above. The compiler is `CMAKE_CXX_COMPILER`. `hotc` adds no `-O` flag and does not read `CMAKE_CXX_FLAGS`. Give the flags of the host in `FLAGS`.

Two scripts in one call must have different names (the file name without the extension). Else `hotc` stops with `two scripts have the name ...`.

The stem rule also applies to `.cpp` scripts. A `foo.hot` and a `foo.cpp` in one call have the same stem `foo`. The configure step fails with an error that names both scripts. The `hotc` output of a `.cpp` script is named `<input>.cpp`, for example `foo.cpp.cpp`. The backends keep the stem rule, for example `foo.g1.cpp` and `foo.g1.so`.

The target does not build automatically. Your host target must depend on it: `add_dependencies(app name)`.

### Generation numbers

The generation `n` in the file name is 1. The option `--gen n` in `HOTC_ARGS` sets another number. The function reads the number for the output names, and `hotc` gets the option. A reload needs a new generation that is newer than the active one. A module with the same number gives `GenerationNotNewer`.

The generations of one class share the slot file `OUT_DIR/slots.txt`. This file keeps the slot order between generations. So build generation 2 after generation 1 and use the same `OUT_DIR`. Give `AFTER` the target of generation 1. `AFTER` only orders the targets. It adds a dependency of the target, not of the files.

### Variables that the function sets

| Variable | Scope | Mode | Value |
|---|---|---|---|
| `HOT_SCRIPTS_<TARGET>_LIBRARIES` | the scope of the caller (`PARENT_SCOPE`) | default | The paths of the libraries, one for each script. |
| `HOT_INCLUDE_DIR` | the cache (INTERNAL) | both | The include folder of Hot++. Set when the Hot++ file is loaded, not by the call. |

AOT mode sets no variable. The function also writes the file `${HOT_HOSTS_DIR}/<TARGET>.json` when CMake generates (see `hot_script_host`).

### Multi-config generators

With `Ninja Multi-Config` or another multi-config generator, the default `OUT_DIR` has no part for the configuration. All configurations write to the same files, and `FLAGS` are the same for all of them. Build one configuration at a time in one tree. The libraries of the AOT mode are normal CMake libraries, so they follow the configuration. This behavior comes from the code of `hot_add_scripts`. A build with two configurations in one tree is untested.

### Worked example: default mode with generations

```cmake
separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
set(engine ${CMAKE_SOURCE_DIR}/engine)

hot_add_scripts(TARGET gen1 SCRIPTS player_v1.hot
                INCLUDES ${engine} FLAGS ${host_flags} DEPENDS ${engine}/Actor.h)
hot_add_scripts(TARGET gen2 SCRIPTS player_v2.hot AFTER gen1 HOTC_ARGS --gen 2
                INCLUDES ${engine} FLAGS ${host_flags} DEPENDS ${engine}/Actor.h)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE hot_runtime)
set_target_properties(app PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(app gen2)

enable_testing()
add_test(NAME app COMMAND app ${HOT_SCRIPTS_gen1_LIBRARIES} ${HOT_SCRIPTS_gen2_LIBRARIES})
```

The build makes these files in `${CMAKE_BINARY_DIR}/hot_scripts`: `player_v1.g1.so`, `player_v2.g2.so` and `slots.txt`. The program loads generation 1 with `load` and generation 2 with `request_reload`. See [Hot reload](hot-reload.html). The call in `tests/scripts/CMakeLists.txt` uses the same keywords.

### Worked example: AOT mode

```cmake
hot_add_scripts(TARGET game_scripts AOT SCRIPTS player.hot boss.hot
                INCLUDES ${CMAKE_SOURCE_DIR}/engine
                PRELUDES ${CMAKE_SOURCE_DIR}/scripts/stubs.hot
                DEPENDS ${CMAKE_SOURCE_DIR}/engine/Actor.h
                HOTC_ARGS --no-strict-borrowed)
add_executable(game main.cpp)
target_link_libraries(game PRIVATE game_scripts)
```

The function runs `hotc --aot` and makes the static library `name`. The library has the files `OUT_DIR/<script name>.aot.cpp` and `OUT_DIR/hot_aot_registry.cpp`. The library defines `hot::aot_table` (`include/hot/aot.h`).

The library links `hot_runtime_core` (PUBLIC). A program that links `name` and `hot::aot` has no loader, no reload and no Clang. When the build uses fmt, the library also links `hot_fmt`. AOT mode makes only generation 1. Script names and class names must be unique across the scripts of one table.

The library is a normal target, so it uses the compile flags of your project (`CMAKE_CXX_FLAGS` and the build type). `FLAGS` go to `hotc`, not to the compiler.

## hot_script_host

```
hot_script_host(NAME name [TARGET target] [SCRIPT_DIRS dir...] [SCRIPTS file...]
                [INCLUDES dir...] [FLAGS flag...])
```

The function describes a script host for the language server `hot-ls`. It does not build anything. When CMake generates the build system, it writes `${HOT_HOSTS_DIR}/<name>.json`. With `TARGET`, it also writes three side files in the same folder.

`NAME` and one of `SCRIPT_DIRS` or `SCRIPTS` are required. `TARGET` is the CMake target of your host program. The file gets its include folders, defines and options. The include folder of Hot++ is always first. A relative path in `SCRIPT_DIRS`, `SCRIPTS` or `INCLUDES` is relative to the current source folder (`sourceDir` in the file). A UTILITY target (a custom target) has no such lists, so the function skips them.

```cmake
hot_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

You do not need this call for scripts that `hot_add_scripts` builds, because that function calls it. Use it for a host that compiles its scripts at run time. See [Tools](tools.html) and the section "Automatic setup" of `README.md`.

### The files

| File | When | Content |
|---|---|---|
| `<name>.json` | always | `version` (1), `name`, `binaryDir`, `sourceDir`, `compiler`, `std`, `resourceDir`, `scriptDirs`, `scripts`, `includeDirs`, `defines` (always empty), `flags`. With `TARGET`: `definesFile`, `optionsFile` and `includesFile`. |
| `<name>.defines` | `TARGET` is a real target | The compile definitions of the target, one on each line. |
| `<name>.options` | `TARGET` is a real target | The compile options of the target, one on each line. |
| `<name>.includes` | `TARGET` is a real target | The raw `INCLUDE_DIRECTORIES` of the target, one on each line. A relative entry is relative to `sourceDir`. |

The folder is `HOT_HOSTS_DIR`. The default is `${CMAKE_SOURCE_DIR}/.cache/hot/hosts`. Add `.cache/` to the ignore list of your project. The side files exist because a quote or a backslash in a path cannot pass through a generator expression into JSON. A path with a newline, and a path with a semicolon, do not work.

`hot_add_scripts` calls `hot_script_host` without `TARGET`, so it writes only the JSON file. The `includeDirs` entry has the include folder of Hot++, the `INCLUDES` and (with fmt) the fmt folder. In default mode `flags` has `FLAGS` and the fmt flags.

Two notes on the fields:

- `std`: with `TARGET`, the `CXX_STANDARD` property of the target, else `CMAKE_CXX_STANDARD`, else 17. Without `TARGET` it is `CMAKE_CXX_STANDARD` or 17. It does not use the default 20 of Hot++. If your project sets no standard, the host file says `c++17` and the scripts build as C++20. Set `CMAKE_CXX_STANDARD` to make them agree.
- `resourceDir`: the value of `CLANG_RESOURCE_DIR`. A consumer project does not see this Hot++ variable, so the field can be empty. Then `hot-ls` uses its own `--resource-dir` option or its default.

## Install the SDK

`cmake --install` makes a prefix that holds everything a program needs: the tools, the libraries, the headers and the CMake package. A user project then calls `find_package(Hot REQUIRED)`. This is the way to give Hot++ to a team that does not build it.

For a release, configure like this:

```
cmake -S . -B build-sdk -G Ninja -DHOT_PORTABLE=ON -DHOT_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build-sdk
cmake --install build-sdk --prefix /opt/hot-sdk
```

`HOT_PORTABLE=ON` keeps the paths of the build machine out of the libraries (see "Portable binaries"). The option `HOT_INSTALL` makes the install rules. It is ON when Hot++ is the main project and OFF when you use `add_subdirectory`.

| Path in the prefix | Content |
|---|---|
| `bin/hotc` | The translator and AOT tool. |
| `bin/hot-ls` | The language server. |
| `lib/` | The static libraries (`libhot_runtime.a`, `libhot_aot.a`, `libhot_backend.a` and others). |
| `include/hot/` | The public headers. `hotc` and the host compile with this folder. |
| `lib/cmake/Hot/` | `HotConfig.cmake`, `HotTargets.cmake`, `HotScripts.cmake`, `HotFormat.cmake` and `CPM.cmake`. |
| `share/hot/vscode/` | The VS Code extension: the file `hotpp-<version>.vsix`, or the folder `hotpp` when `vsce` was not found at configure time. |

On some systems `lib` is `lib64`. CMake uses the standard folder names (`GNUInstallDirs`).

### Move the SDK

The prefix is relocatable. You can copy or move it to another folder or machine. `HotConfig.cmake` finds the other files from its own location, so the user project needs only `CMAKE_PREFIX_PATH` or `Hot_DIR` to point at the new place. The `HOT_PORTABLE` libraries also find the headers at run time, as "Portable binaries" describes.

### What the SDK needs on the machine of the user

- **libclang.** `hotc` and the language server link `libclang`. The export holds no path of it. `HotConfig.cmake` first runs `find_package(Clang CONFIG QUIET)` and links the imported target `libclang`. The folder of the Clang package of the SDK build is a hint, so the same version wins. If that target is missing, or its major version differs from the SDK build, it uses the fallback `find_library` for `clang` (or `libclang`) with the folder of the SDK build as a hint, then the standard places. A value of the cache variable `HOT_CLANG_C_LIBRARY` always wins and skips the package. Set `-DHOT_CLANG_C_LIBRARY=/path/to/libclang.so` when the library is somewhere else. If CMake finds no libclang, it prints a warning, and the link of `hot::hotc_lib`, `hot::clang_backend` and `hot::backend` fails with the message `HOT_CLANG_C_LIBRARY-NOTFOUND`. A program in AOT mode does not link libclang.
- **A C++ compiler.** A user project that uses the CMake package needs a Clang C++ compiler of the same family and version as the SDK build, as for any Hot++ build.
- **LLVM and Clang (JIT only).** An SDK with the JIT backend (`HOT_SDK_JIT` is `ON`) has the target `hot::jit_backend`. `HotConfig.cmake` then calls `find_package(LLVM CONFIG)` and `find_package(Clang CONFIG)`, so the user machine needs the LLVM and Clang CMake packages. Add their folders to `CMAKE_PREFIX_PATH` if CMake does not find them. An SDK without the JIT backend does not need them.
- **clangd (for `hot-ls`).** The language server starts a stock `clangd` as a child process. It must be in `PATH`, or you give its path with `--clangd` (see the VS Code extension). `hot-ls` also needs the Clang resource folder; pass `--resource-dir` if the server cannot find it. Without `clangd`, `--clangd ""` leaves only the analysis features.

### The format library of the SDK

The SDK fixes the format library of `${...}` (see "The format library"). The runtime in the SDK was built with it, so the scripts of your project must use it too.

- An SDK that uses fmt (built with C++17, or with `HOT_USE_FMT=ON`) makes your project use fmt at any C++ standard. The project gets fmt like a normal build (`HOT_FMT_PROVIDER`).
- An SDK that uses `std::format` needs C++20 in your project. With a lower `CMAKE_CXX_STANDARD`, the configure step stops with an error. Build the SDK with C++17 for such projects.

### Use the SDK from the samples

Each project in `samples/` works with the SDK. Set `HOT_ROOT` to an empty value:

```
cmake -S samples/hot-reload -B /tmp/hr -DHOT_ROOT= -DCMAKE_PREFIX_PATH=/opt/hot-sdk
```

## Portable binaries

A normal (development) build puts paths of the build tree into the binary: the compiler, the Hot++ headers, the Clang resource folder and the work folder. A binary like this works only on the machine that built it. For a release, configure with `-DHOT_PORTABLE=ON`. The binaries then hold no build path. The host finds the paths at run time with `hot::default_paths()` (`#include "hot/paths.h"`).

```
cmake -S . -B build-release -DHOT_PORTABLE=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
```

`HOT_PORTABLE` and `HOT_BUILD_TESTS` cannot both be ON, because the tests need the build paths. Build the tests in a normal build.

```cpp
struct DefaultPaths {
    std::string compiler;                  // the C++ compiler
    std::vector<std::string> include_dirs; // the Hot++ include folders
    std::string resource_dir;              // the Clang resource folder. Empty means the compiler default.
    std::string work_dir;                  // the parent of the work folder
    std::vector<std::string> cxxflags;     // the compile flags
};
DefaultPaths default_paths();
```

`make_default_backend` uses this function. A host that builds its own backend (see the samples `clang-files-backend` and `jit-backend`) fills its options from it. A field that the search does not find is empty.

### The search order

| Value | Development build | Portable build, in this order |
|---|---|---|
| `compiler` | The compiler of the build. | 1. The environment variable `HOT_CXX`. 2. `clang++` on `PATH`. |
| `include_dirs` | The `include` folder of Hot++ (and the folders of the build, for example `tests/engine`). | 1. The environment variable `HOT_INCLUDE_DIR`. 2. `<exe>/hot/include`. 3. `<exe>/../include`. The first folder of 2 and 3 that has `hot/script.h` wins. |
| `resource_dir` | The folder of the build compiler. | 1. The environment variable `HOT_RESOURCE_DIR`. 2. The output of `<compiler> -print-resource-dir`. The host runs this one time. |
| `work_dir` | A folder in the build tree. | 1. `$XDG_RUNTIME_DIR/hot_work`, if the variable is set. 2. `hot_work-<uid>` in the temporary folder of the system. |
| `cxxflags` | The flags of the build (`CMAKE_CXX_FLAGS`). | The same. A flag that names a path puts that path into the binary. See below. |

`<exe>` is the folder of the running program. On Linux it comes from `/proc/self/exe`. If that fails, the current folder is used. The backend adds a folder `p<pid>` to `work_dir` for each process.

The default backend of a portable build makes `work_dir` with mode 0700. It refuses to use the folder, and `make_default_backend` throws `std::runtime_error` with a message, when the folder is not a real directory, when another user owns it, or when group or other users can write to it. `hot::check_work_dir(dir)` does this check. It returns an empty text when the folder is safe. Windows uses the plain temporary folder.

`CMAKE_CXX_FLAGS` goes into the host as the default flags, in a portable build too. A flag that names a path (for example `--gcc-toolchain=/opt/gcc`) puts that path into the binary. Keep such flags out of `CMAKE_CXX_FLAGS` for a release build. Give them to the host at run time with `DefaultBackendOptions::extra_flags` instead.

In a portable build the translator and the backends also use the resource folder from this search when you give none. A host without `clang++` on `PATH` sets `HOT_RESOURCE_DIR` (and `HOT_CXX`). The build-time `hotc` is different: `hot_add_scripts` and `hot_add_host` pass `--resource-dir` to it. The value comes from `CLANG_RESOURCE_DIR`, else from `<CMAKE_CXX_COMPILER> -print-resource-dir` at configure time. So the build does not need `clang++` on `PATH`, and the value does not go into the host binary. The fmt headers are not in the binary either. With fmt, the compiler must find the fmt headers by itself on the target machine.

### What a release host must provide

A host that compiles scripts at run time (`HOT_RELOAD`) needs two things next to the release:

- **A compiler.** A Clang C++ compiler. It must be on `PATH` as `clang++`, or the environment variable `HOT_CXX` must name it. The compiler must be the same family and version as the one that built the host, or the module ABI can differ.
- **The Hot++ headers.** The folder `include` of Hot++ (it has `hot/script.h`). Put it in `<exe>/hot/include` or in `<exe>/../include`, or set `HOT_INCLUDE_DIR`. `hot_add_host` with `MODE HOT_RELOAD` copies it to `<exe>/hot/include` after the build. Copy this folder into your package. Your own headers that the scripts include (for example `Creature.h`) must be in `include_dirs` too.

An `AOT` host needs neither. A `PRECOMPILED` host needs no compiler. It needs the script libraries.

### Security

The compiler name from the environment runs a program: `HOT_CXX` is the program that the host starts, and `HOT_INCLUDE_DIR` picks the headers that the compiler reads. A host must not take these values from untrusted input. The search reads the environment of the process only. If your host runs in an environment that a user does not control, set the values in your own code and fill the backend options yourself.

## Sanitizers and the build type

Scripts and the host must use the same sanitizer runtime. The root file `CMakeLists.txt` says that the script backends pass the same flags to the compile of the scripts, so the generation modules use the same runtime as the host.

- `make_default_backend` passes `CMAKE_CXX_FLAGS` to every script compile. A preset with `-fsanitize=address` therefore works with no more settings.
- A backend that you build yourself needs the flags in `extra_flags`. The samples `clang-files-backend` and `jit-backend` take them from `hot::default_paths().cxxflags`, which has `CMAKE_CXX_FLAGS`.
- `hot_add_scripts` in the default mode does not read `CMAKE_CXX_FLAGS`. Put the host flags in `FLAGS`: `separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")`, then `FLAGS ${host_flags}`. The sample [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library) does this.
- `hot_add_scripts` in AOT mode builds a normal library. It follows the flags of the project.
- Use one build folder for each mode (plain, ASan, TSan). The presets do this.
- The build type flags (`CMAKE_CXX_FLAGS_DEBUG` and the other ones) are not part of `CMAKE_CXX_FLAGS`. The script compile does not get them. The compile always uses `-g` and has no `-O` flag. Add `-O2` (or a flag you need) to `FLAGS`, `extra_flags` or `CMAKE_CXX_FLAGS` for optimized scripts.
- The JIT backend works with ThreadSanitizer.

## Presets

`CMakePresets.json` (version 6, CMake 3.25 or later) has these presets. The generator is `Ninja Multi-Config`, the configurations are `Debug` and `RelWithDebInfo`, and the build folder is `cmake-build/<preset>`. The compiler is `clang++`.

| Configure preset | Backends in the build | Sanitizer (`CMAKE_CXX_FLAGS`) | Purpose |
|---|---|---|---|
| `linux-clang-files` | clang files only | none | The default development build. |
| `linux-clang-files-asan` | clang files only | `-fsanitize=address` | Memory errors. |
| `linux-clang-files-tsan` | clang files only | `-fsanitize=thread` | Data races. |
| `linux-clang-jit` | JIT only | none | The JIT backend. It needs LLVM and Clang. |
| `linux-clang-jit-asan` | JIT only | `-fsanitize=address` | Memory errors with the JIT. |
| `linux-clang-jit-tsan` | JIT only | `-fsanitize=thread` | Data races with the JIT. |

The build presets and the test presets have the same names for each configure preset:

| Build and test preset | Configure preset | Configuration |
|---|---|---|
| `linux-clang-files-dbg` | `linux-clang-files` | Debug |
| `linux-clang-files-rel` | `linux-clang-files` | RelWithDebInfo |
| `linux-clang-files-asan` | `linux-clang-files-asan` | Debug |
| `linux-clang-files-tsan` | `linux-clang-files-tsan` | Debug |
| `linux-clang-jit-dbg` | `linux-clang-jit` | Debug |
| `linux-clang-jit-rel` | `linux-clang-jit` | RelWithDebInfo |
| `linux-clang-jit-asan` | `linux-clang-jit-asan` | Debug |
| `linux-clang-jit-tsan` | `linux-clang-jit-tsan` | Debug |

The hidden presets `base`, `base-files`, `base-jit`, `base-clang`, `base-asan`, `base-tsan` and `base-test` hold the common settings. The sanitizer presets set `CMAKE_CXX_FLAGS` to the sanitizer flag only. The test presets print the output of a failed test. Example:

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg
ctest --preset linux-clang-files-dbg
```

A consumer project can copy the presets or set the same cache variables itself.

## Troubleshooting

The first column has the text as it appears in the source. A text with `...` has a value in its place.

| Message | Cause | Fix |
|---|---|---|
| `hotc: error: this hotc is built for --format=std, not --format=fmt` (or the other way) | `hot_add_scripts` chose another format library than the one `hotc` has. The standard in your scope differed from the standard when Hot++ was configured. | Set `CMAKE_CXX_STANDARD` and `HOT_USE_FMT` before `add_subdirectory`. Do not change them later. Configure again in a new build folder. |
| `HOT_ENABLE_JIT must be AUTO, ON or OFF. It is: ...` | The cache variable has another value. | Use `AUTO`, `ON` or `OFF`. |
| `HOT_ENABLE_JIT=ON needs the LLVM and Clang CMake packages. CMake does not find them.` | You asked for the JIT backend, and the packages are missing. | Install the LLVM and Clang development packages, add them to `CMAKE_PREFIX_PATH`, or use `AUTO` or `OFF`. |
| `hot_add_host: MODE HOT_RELOAD needs a backend` (or `AOT_RELOAD`) | Hot++ has no run-time backend: `HOT_ENABLE_CLANG_FILES` and `HOT_ENABLE_JIT` are both off. | Use `MODE AOT` or `PRECOMPILED`, or turn on a backend. |
| `The JIT backend is not in this build. Configure Hot++ with HOT_ENABLE_JIT=ON ...` (a `std::runtime_error` at run time) | The program asked for the JIT backend (`BackendKind::Jit` or `HOT_BACKEND=jit`), but the build does not have it. | Build with `HOT_ENABLE_JIT=ON`, or ask for the other backend. The error for the clang files backend names `HOT_ENABLE_CLANG_FILES`. |
| `HOT_FMT_PROVIDER must be AUTO, SYSTEM, CPM or NONE. It is: ...` | A wrong value. | Use one of the four values. |
| `HOT_FMT_PROVIDER is NONE, but the target fmt::fmt-header-only or fmt::fmt does not exist...` | `NONE` needs your own fmt target. | Create the target before `add_subdirectory`, or use another provider. |
| `Hot++: fmt is not available. Check HOT_FMT_PROVIDER (now ...).` | No provider gave a fmt target. | Install fmt, or use `CPM`. |
| `hot_add_scripts needs TARGET and SCRIPTS` (also `hot_add_host needs TARGET and SCRIPTS`) | A required keyword is missing. | Give both. |
| `hot_add_host: MODE must be AOT, AOT_RELOAD, HOT_RELOAD or PRECOMPILED. It is: ...` | An unknown mode. | Use one of the four. |
| `hot_add_host: MODE AOT_RELOAD needs a backend (the target hot::backend) ...` | The Hot++ in use has no `hot::backend` target. The override needs a compiler at run time. | Use a Hot++ build or SDK with a backend, or use `MODE AOT`. |
| `hot_add_host: the target ... does not exist` | The call comes before `add_executable`. | Make the program target first. |
| `HOT_PORTABLE and HOT_BUILD_TESTS are both ON ...` | The tests need the build paths. | Set `HOT_BUILD_TESTS=OFF`, or build the tests in a normal build. |
| `hot_add_scripts: unknown arguments: ...` (also `hot_add_host: unknown arguments: ...`) | A word is not a keyword (for example a typo). | Check the spelling. Keywords are upper case. |
| `hot_add_scripts: AFTER does not work with AOT (target ...)` | `AFTER` and `AOT` in one call. | Remove `AFTER`. AOT makes only generation 1. |
| `hot_add_scripts: PRELUDES works only with AOT (target ...)` | `PRELUDES` in the default mode. | Add `AOT` or remove `PRELUDES`. |
| `hot_script_host needs NAME and SCRIPT_DIRS or SCRIPTS` | A required keyword is missing. | Give `NAME` and one of the two lists. |
| `hot_script_host: the target ... does not exist` | `TARGET` names a target that is not defined yet. | Call the function after `add_executable`. |
| `hotc: error: two scripts have the name ...` | Two scripts have the same file name in one call. | Give the files different names. |
| `hotc: error: cannot read ...` | A script file does not exist. | Check the path in `SCRIPTS`. |
| `the compile of ... failed (exit code ...)` | The C++ compiler rejected the generated code. The text before it has the compiler diagnostics. | Read the diagnostics. Check `INCLUDES`, `FLAGS` and the standard. |
| `the script '...' is not found` | A `using "x.hot";` line names a script that the translator cannot read. | Check the path. Add the folder to `INCLUDES`. |
| `base class '...' is not found in the imported headers` | The header of the native base class is not in the `using` lines or not in the include folders. | Add the header with `using`. Add its folder to `INCLUDES`. |
| `symbol script_register not found in ...` | The file is a library, but not an Hot++ generation module. | Load a library that `hotc` made. |
| `the class ... is in two AOT scripts (...)` | Two AOT scripts have a class of the same name. It is an exception of `AotRegistry`. | Rename one class. |
| `no AOT script class ...` | `AotRegistry::create` got an unknown class name. | Check the name and the scripts in `SCRIPTS`. |
| `the loader already has a generation: call reset first` | `Loader::load` ran twice. | Use `request_reload` for a later generation, or call `reset` first. |
| `class ...: generation ... is not newer than the active generation ...` | The module has the same generation number as the active one. | Use a higher `--gen` number. |
| `cannot write ...` | The backend or `hotc` cannot write the slot file or an output file. | Check the rights of the `OUT_DIR` or the work folder. |

Some problems have no fixed Hot++ message. The text comes from the tool that reports them.

| Symptom | Cause | Fix |
|---|---|---|
| The linker reports an undefined reference to `hot::aot_table`. | A program that uses `AotRegistry` does not link the library from `hot_add_scripts(... AOT ...)`. Only that library defines the table. | Link the AOT library. |
| `Loader::load` or `module_from_library` fails and `error` has the `dlopen` text with `undefined symbol`. | The host is not linked with `ENABLE_EXPORTS`, or the host does not contain a function that the script calls. The `dlopen` text comes from the system. | Set `ENABLE_EXPORTS ON`. Link the code with the function, as a whole archive if it is a static library. |
| The JIT backend fails with `script_register not found or not linkable: ...`. | The in-memory code needs a symbol that the executable does not export (see `README.md`, the section on the JIT backend). | Same as above. |
| The configure step stops in `find_library` or `find_path` for libclang (the fallback, used when the Clang CMake package has no target `libclang`). | The development package of libclang is not installed. The message is from CMake. | Install it, or set `CMAKE_PREFIX_PATH`. |
| The JIT backend is not in the build, and you want it. | `HOT_ENABLE_JIT` is `AUTO` and CMake does not find the LLVM and Clang packages. The configure output says `JIT backend: off`. | Install the LLVM and Clang development packages, or set `HOT_ENABLE_JIT=ON` to get an error that names the problem. |
| The script compile cannot find compiler headers such as `stddef.h`. | The resource folder is wrong or empty. The root file takes it from `<compiler> -print-resource-dir`. The compiler must be Clang. MSVC has no such option. | Use `clang++` as `CMAKE_CXX_COMPILER`. For a backend that you build yourself, set `resource_dir`. For MSVC set `HOT_CLANG_RESOURCE_DIR`. |
| CMake reports an unknown command `hot_add_scripts`. | The call comes before `add_subdirectory` of Hot++. The message is from CMake. | Move the call after `add_subdirectory`. |
| A script change does not rebuild the library. | A changed header or an imported script is not in `DEPENDS`. | Add the file to `DEPENDS`. |
| A program with sanitizer flags crashes when it loads a module. | The module is compiled without the same flags. | Put the flags in `FLAGS` or `extra_flags`. See "Sanitizers and the build type". |

## hot_gtest

`hot_gtest(<target> [gtest_discover_tests arguments...] [PROPERTIES ...])` is an internal helper. It is in `cmake/HotTests.cmake`, and the build loads it only when `HOT_BUILD_TESTS` is ON. It registers the GoogleTest cases of a test target, requires C++20 for the target and makes each test depend on the fixture `hot_scratch`. Host projects do not call it.

## Samples

Each sample is a small project. It builds in the Hot++ tree and as a separate project. Each sample has a `README.md` with the sections "How it works", "Files", "Build and run" and "Adapt it to your project". The page [Embedding](embedding.html) maps each hosting situation to its sample and has the code outline.

- [aot](https://github.com/rokups/hotpp/tree/main/samples/aot): the C++ of the scripts is in the binary.
- [runtime-compile](https://github.com/rokups/hotpp/tree/main/samples/runtime-compile): the program compiles a script at run time.
- [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library): the build makes a script library and the program loads it.
- [hot-reload](https://github.com/rokups/hotpp/tree/main/samples/hot-reload): the program replaces the code of a live object.
- [clang-files-backend](https://github.com/rokups/hotpp/tree/main/samples/clang-files-backend): the program builds `ClangFilesBackend` itself.
- [jit-backend](https://github.com/rokups/hotpp/tree/main/samples/jit-backend): the program builds `JitBackend` itself. It needs the JIT backend (`HOT_ENABLE_JIT`).

The samples use the Hot++ tree in `HOT_ROOT`. The default is the tree that holds the `samples` folder. To use a sample in your project, set `HOT_ROOT` to the Hot++ tree (or use FetchContent) and copy the `add_executable` and `hot_add_host` lines.
