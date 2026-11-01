---
layout: default
title: SDK and portable binaries
nav_order: 40
group: CMake
---

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
| `share/hot/LICENSE` | The MIT license of Hot++. |

On some systems `lib` is `lib64`. CMake uses the standard folder names (`GNUInstallDirs`).

### Move the SDK

The prefix is relocatable. You can copy or move it to another folder or machine. `HotConfig.cmake` finds the other files from its own location, so the user project needs only `CMAKE_PREFIX_PATH` or `Hot_DIR` to point at the new place. The `HOT_PORTABLE` libraries also find the headers at run time, as "Portable binaries" describes.

### What the SDK needs on the machine of the user

- **`libclang`.** `hotc` and the language server link `libclang`. The export holds no path of it. `HotConfig.cmake` first runs `find_package(Clang CONFIG QUIET)` and links the imported target `libclang`. The folder of the Clang package of the SDK build is a hint, so the same version wins. If that target is missing, or its major version differs from the SDK build, it uses the fallback `find_library` for `clang` (or `libclang`) with the folder of the SDK build as a hint, then the standard places. A value of the cache variable `HOT_CLANG_C_LIBRARY` always wins and skips the package. Set `-DHOT_CLANG_C_LIBRARY=/path/to/libclang.so` when the library is somewhere else. If CMake finds no `libclang`, it prints a warning, and the link of `hot::hotc_lib`, `hot::clang_backend` and `hot::backend` fails with the message `HOT_CLANG_C_LIBRARY-NOTFOUND`. A program in AOT mode does not link `libclang`.
- **A C++ compiler.** A user project that uses the CMake package needs a Clang C++ compiler of the same family and version as the SDK build, as for any Hot++ build.
- **LLVM and Clang (JIT only).** An SDK with the JIT backend (`HOT_SDK_JIT` is `ON`) has the target `hot::jit_backend`. `HotConfig.cmake` then calls `find_package(LLVM CONFIG)` and `find_package(Clang CONFIG)`, so the user machine needs the LLVM and Clang CMake packages. Add their folders to `CMAKE_PREFIX_PATH` if CMake does not find them. An SDK without the JIT backend does not need them.
- **`clangd` (for `hot-ls`).** The language server starts a stock `clangd` as a child process. It must be in `PATH`, or you give its path with `--clangd` (see the VS Code extension). `hot-ls` also needs the Clang resource folder; pass `--resource-dir` if the server cannot find it. Without `clangd`, `--clangd ""` leaves only the analysis features.

### The format library of the SDK

The SDK fixes the format library of `${...}` (see [The format library](cmake-options.html#the-format-library)). The runtime in the SDK was built with it, so the scripts of your project must use it too.

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
