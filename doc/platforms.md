---
layout: default
title: Platforms
nav_order: 41.7
group: CMake
---

## AOT-only platforms

Some platforms cannot create code at run time. A program on these platforms runs scripts that the build compiled ahead of time (AOT). These platforms are AOT-only:

| Platform | Status |
|---|---|
| iOS (and tvOS, visionOS, watchOS) | Required. Not tested. |
| Android | Wanted. Not tested. |
| Web (Emscripten) | Wanted. Not tested. |
| UWP (`WindowsStore`) | Wanted. Not tested. |

An AOT-only program has no code generation at run time: no libclang or LLVM, no JIT, no compiler process, no `dlopen` of generated libraries and no file watcher. The program links only `hot::aot` and `hot::runtime_core`.

## CMake behavior

The root `CMakeLists.txt` sets `HOT_AOT_ONLY_PLATFORM` to `ON` when `EMSCRIPTEN`, `ANDROID` or `IOS` is true, or when `CMAKE_SYSTEM_NAME` is `Emscripten`, `Android`, `iOS`, `tvOS`, `visionOS`, `watchOS`, `WindowsStore` or `WindowsPhone`. In this case:

- `HOT_ENABLE_CLANG_FILES` and `HOT_ENABLE_JIT` default to `OFF`. `HOT_ENABLE_JIT=AUTO` becomes `OFF`.
- `HOT_ENABLE_CLANG_FILES=ON` or `HOT_ENABLE_JIT=ON` stops the configure step with an error.
- `HOT_BUILD_TRANSLATOR` defaults to `OFF`. Give a host `hotc` with `HOT_HOST_HOTC`.
- The target `hot_runtime` (loader, migration, `dlopen`, process and path code) is not made. The target `hot::runtime` does not exist.
- The targets `hot_runtime_core` and `hot_aot` are made. They have no `dlopen`, no `fork` and no compiler call.

The desktop defaults do not change.

## Toolchain flow

1. Build `hotc` for the build host (a desktop build).
2. Compile the scripts ahead of time on the host: `hotc --aot <file.hot>... --out-dir DIR`. In CMake use `hot_add_scripts(... AOT [OPTIMIZED])` with `HOT_HOST_HOTC`. See [CMake](cmake-scripts.html).
3. Compile the generated C++ with the target toolchain into a static library. Link it with `hot::aot` into the game.
4. The game registers the units with `hot::AotRegistry` (`hot/aot.h`).

The packing step of a game engine (for example `rbfx_script_pack`) is not part of this repository.

## What is verified

On Linux with the preset `linux-clang-aot`:

- `hot_aot` and `hot_runtime_core` build. The test program `aot_demo` has no `libclang` or LLVM in `ldd`, and no `clang`, `llvm`, `dlopen`, `fork`, `posix_spawn` or `execv` in `nm -D --undefined-only`. The undefined symbols of their objects have no `clang`, `llvm`, `dlopen`, `dlsym`, `fork`, `posix_spawn`, `exec*`, `popen`, `system` or `inotify`.
- The AOT tests pass: `unit_runtime_tests`, `integration_aot_iface_tests`, `integration_aot_iface_units_tests`, `integration_aot_units_tests`, `integration_aot_registry_tests` and `integration_aot_parity_tests`.
- The configure step with `EMSCRIPTEN=ON` on a Linux host (a simulation of the guard only) builds `hot_aot` without `hot_runtime`. `HOT_ENABLE_JIT=ON` and `HOT_ENABLE_CLANG_FILES=ON` stop the configure step there.
- The presets `linux-clang-both` and `linux-clang-aot` configure as before.

## What is not verified

- No build for iOS, Android, Emscripten or UWP. No toolchain is available. The target headers are not available for a `--target=... -fsyntax-only` check either.
- The runtime uses `std::mutex`, `std::condition_variable`, `thread_local` and C++ exceptions (`ScriptError`). Web needs `-fexceptions` (or `-sDISABLE_EXCEPTION_CATCHING=0`). A web build without pthreads has only one thread. A wait on a task from another thread cannot finish there.
- Script coroutines (`frame_reaper.cpp`) need C++20 coroutine support in the target library.
- UWP forbids some Win32 calls. The AOT part uses none of them, but this is not tested.
