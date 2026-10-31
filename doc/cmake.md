---
layout: default
title: CMake
nav_order: 35
group: CMake
---

This guide shows how a host project uses Hot++ with CMake. This page has the quick start. The other pages list the requirements, the targets, the options and the functions. They also describe portable binaries. They end with the presets and a table of errors.

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
set(CMAKE_CXX_STANDARD 17) # the default of Hot++. Use 20 or 23 for std::format.

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

You can also use Hot++ as an installed SDK. You do not need the Hot++ source tree. See the section [Install the SDK](cmake-sdk.html#install-the-sdk) for how to make the SDK. The `CMakeLists.txt` of the program is shorter:

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

## Parts of this guide

- [Setup and targets](cmake-setup.html): the requirements, how to consume Hot++ and the targets.
- [Options and standard](cmake-options.html): the build options, the C++ standard and the format library.
- [Hosts with hot_add_host](cmake-host.html): the keywords and the modes.
- [Script functions](cmake-scripts.html): `hot_add_scripts` and `hot_script_host`.
- [SDK and portable binaries](cmake-sdk.html): install the SDK, and build a host without build paths.
- [Presets, tests and troubleshooting](cmake-presets.html): sanitizers, presets, the table of errors and `hot_gtest`.

## Samples

Each sample is a small project. It builds in the Hot++ tree and as a separate project. Each sample has a `README.md` with the sections "How it works", "Files", "Build and run" and "Adapt it to your project". The page [Embedding](embedding.html) maps each hosting situation to its sample and has the code outline.

- [aot](https://github.com/rokups/hotpp/tree/main/samples/aot): the C++ of the scripts is in the binary.
- [runtime-compile](https://github.com/rokups/hotpp/tree/main/samples/runtime-compile): the program compiles a script at run time.
- [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library): the build makes a script library and the program loads it.
- [hot-reload](https://github.com/rokups/hotpp/tree/main/samples/hot-reload): the program replaces the code of a live object.
- [clang-files-backend](https://github.com/rokups/hotpp/tree/main/samples/clang-files-backend): the program builds `ClangFilesBackend` itself.
- [jit-backend](https://github.com/rokups/hotpp/tree/main/samples/jit-backend): the program builds `JitBackend` itself. It needs the JIT backend (`HOT_ENABLE_JIT`).

The samples use the Hot++ tree in `HOT_ROOT`. The default is the tree that holds the `samples` folder. To use a sample in your project, set `HOT_ROOT` to the Hot++ tree (or use FetchContent) and copy the `add_executable` and `hot_add_host` lines.
