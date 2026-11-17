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

An AOT-only program has no code generation at run time: no libclang or LLVM, no JIT, no compiler process, no `dlopen` of generated libraries and no file watcher. The program links `hot::aot` and `hot::runtime_core`, or `hot::runtime` when it needs the loader for the AOT table (for example rbfx-script).

## CMake behavior

The root `CMakeLists.txt` sets `HOT_AOT_ONLY_PLATFORM` to `ON` when `EMSCRIPTEN`, `ANDROID` or `IOS` is true, or when `CMAKE_SYSTEM_NAME` is `Emscripten`, `Android`, `iOS`, `tvOS`, `visionOS`, `watchOS`, `WindowsStore` or `WindowsPhone`. In this case:

- `HOT_ENABLE_CLANG_FILES` and `HOT_ENABLE_JIT` default to `OFF`. `HOT_ENABLE_JIT=AUTO` becomes `OFF`.
- `HOT_ENABLE_CLANG_FILES=ON` or `HOT_ENABLE_JIT=ON` stops the configure step with an error.
- `HOT_BUILD_TRANSLATOR` defaults to `OFF`. Give a host `hotc` with `HOT_HOST_HOTC`.
- The target `hot_runtime` (the loader, the migration, the registry) is built with the definition `HOT_AOT_ONLY`. It loads the precompiled AOT table and no generated code. `module.cpp` has no `dlopen`, `dlsym` and `LoadLibrary`: loading a library returns an error. `paths.cpp` starts no process (`popen`). `hot_runtime` does not link the dl library.
- The targets `hot_runtime_core`, `hot_aot` and `hot::runtime` are made. None of them has `dlopen`, `fork` or a compiler call.

The desktop defaults do not change.

## Toolchain flow

1. Build `hotc` for the build host (a desktop build).
2. Compile the scripts ahead of time on the host: `hotc --aot <file.hot>... --out-dir DIR`. In CMake use `hot_add_scripts(... AOT [OPTIMIZED])` with `HOT_HOST_HOTC`. See [CMake](cmake-scripts.html).
3. Compile the generated C++ with the target toolchain into a static library. Link it with `hot::aot` into the game.
4. The game registers the units with `hot::AotRegistry` (`hot/aot.h`).

The packing step of a game engine (for example `rbfx_script_pack`) is not part of this repository.

## What is verified

The option `HOT_FORCE_AOT_ONLY` (default `OFF`) builds a desktop host as an AOT-only platform. It sets `HOT_AOT_ONLY_PLATFORM`. The backends stay off and `ON` is a configure error for them. The loader has no `dlopen` and `hot_runtime` has `HOT_AOT_ONLY`. The translator and `hotc` stay on, because the host is the target and the AOT tests need `hotc`. The option needs `HOT_SCRIPT_MODE` `AOT` or `AOT_OPTIMIZED`. The preset `linux-clang-aot-only` sets it.

On Linux with the preset `linux-clang-aot-only`, the ctest `aot_only_symbols` (`tests/check_aot_only_symbols.cmake`) checks this automatically. It runs `nm --undefined-only` on `libhot_runtime.a`, `libhot_aot.a` and the AOT script library of `aot_demo`, and `nm -D --undefined-only` on `aot_demo`. The check fails on `dlopen`, `dlsym`, `dlclose`, `fork`, `posix_spawn`, `exec*`, `popen`, `system`, `inotify*`, `clang` or `llvm`. It also fails when `ldd` of `aot_demo` lists `libclang` or LLVM. This build does not register the tests that need a loader or a backend: the integration tests `reload`, `closures`, `async`, `threading`, `backends` and `launcher`, the parity, load, override and `*_load` tests of `aot`, the benchmark and the loader samples. The AOT tests pass in this build.

On Linux with the preset `linux-clang-aot`:

- `hot_aot`, `hot_runtime_core` and (with `HOT_AOT_ONLY`) `hot_runtime` build. The test program `aot_demo` has no `libclang` or LLVM in `ldd`, and no `clang`, `llvm`, `dlopen`, `fork`, `posix_spawn` or `execv` in `nm -D --undefined-only`. The undefined symbols of their objects have no `clang`, `llvm`, `dlopen`, `dlsym`, `fork`, `posix_spawn`, `exec*`, `popen`, `system` or `inotify`.
- The AOT tests pass: `unit_runtime_tests`, `integration_aot_iface_tests`, `integration_aot_iface_units_tests`, `integration_aot_units_tests`, `integration_aot_registry_tests` and `integration_aot_parity_tests`.
- The configure step with `EMSCRIPTEN=ON` on a Linux host (a simulation of the guard only) builds `hot_aot` and `hot_runtime`. The undefined symbols of `libhot_runtime.a` have no `dlopen`, `dlsym`, `dlclose`, `fork`, `posix_spawn`, `exec*`, `popen`, `system`, `clang` or `llvm`. `HOT_ENABLE_JIT=ON` and `HOT_ENABLE_CLANG_FILES=ON` stop the configure step there.
- The presets `linux-clang-both` and `linux-clang-aot` configure as before.

## What is not verified

- No build for iOS, Android, Emscripten or UWP. No toolchain is available. The target headers are not available for a `--target=... -fsyntax-only` check either.
- The runtime uses `std::mutex`, `std::condition_variable`, `thread_local` and C++ exceptions (`ScriptError`). Web needs `-fexceptions` (or `-sDISABLE_EXCEPTION_CATCHING=0`). A web build without pthreads has only one thread. A wait on a task from another thread cannot finish there.
- Script coroutines (`frame_reaper.cpp`) need C++20 coroutine support in the target library.
- UWP forbids some Win32 calls. The AOT part uses none of them, but this is not tested.
