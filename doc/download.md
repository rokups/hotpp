---
layout: default
title: Download the SDK
nav_order: 41.6
group: CMake
---

## Where to get the SDK

The CI builds the Hot++ SDK after each change on the branch `master`. It puts the archives in the release [`latest`](https://github.com/rokups/hotpp/releases/tag/latest). The release is a prerelease. Its notes give the commit of the build.

| File | System |
|------|--------|
| `hotpp-sdk-linux-x86_64.tar.gz` | Linux, x86_64 |
| `hotpp-sdk-windows-x86_64.zip` | Windows, x86_64, MSVC |

Each archive has one top folder. This folder is the SDK prefix.

## What the archive contains

- `bin/`: the translator `hotc` and the language server `hot-ls`. The Windows SDK also has `libclang.dll`.
- `include/hot/`: the headers of the runtime.
- `lib/`: the static libraries (the runtime, the AOT support, the translator and the backend).
- `lib/cmake/Hot/`: the CMake package (`HotConfig.cmake`) and the functions `hot_add_host` and `hot_add_scripts`.
- `lib/clang/<version>/include/` (Windows only): the compiler headers that `hotc` uses to read your headers.
- `share/hot/LICENSE`: the license.

The SDK is relocatable. You can unpack it in any folder.

## What your machine needs

- CMake 3.20 or later. On Linux, also Ninja or Make.
- **Linux:** libclang 23 and Clang 23. On Ubuntu, add the [apt.llvm.org](https://apt.llvm.org) repository and install `clang-23`, `libclang-23-dev` and `llvm-23-dev`. Use `clang++-23` as the C++ compiler.
- **Windows:** Visual Studio 2022 or newer with the C++ tools (the workload "Desktop development with C++"), and CMake. Build in Release or RelWithDebInfo. The MSVC toolset must be the toolset of the SDK build or a newer one. The release notes give this version.

The CI tests the SDK on each system with an AOT host (`MODE AOT`). On Windows, the modes `HOT_RELOAD` and `PRECOMPILED` also need the `clang++.exe` of the LLVM release for Windows. See [Windows](windows.html).

## Use the SDK in your project

Use `find_package(Hot)` and `hot_add_host` in your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)

find_package(Hot REQUIRED)

add_executable(myapp main.cpp)
hot_add_host(TARGET myapp MODE AOT SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
```

Set `CMAKE_PREFIX_PATH` to the SDK folder when you configure:

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/hotpp-sdk-linux-x86_64
cmake --build build
```

On Windows, use the same commands in the UCRT64 shell, with the path of the Windows SDK. The sample `samples/aot` is a complete project of this type.

## Test the SDK

The project `tests/sdk_smoke` is a small consumer of the SDK. The CI uses it on each system. To test an SDK on your machine, run this command in the source tree of Hot++:

```
cmake -DSDK=/path/to/sdk -P tests/sdk_smoke/run.cmake
```

The script configures, builds and runs the project. It passes when the program prints `sdk_smoke: result=41`.

See also [SDK and portable binaries](cmake-sdk.html) and [Windows](windows.html).
