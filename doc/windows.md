---
layout: default
title: Windows
nav_order: 41.5
group: CMake
---

## What works

A MinGW host loads, runs and hot-reloads script modules on Windows. The host uses GCC and libstdc++. The tests build it on Linux and run it under wine.

- The script modules use the `clang++` of the official LLVM Windows release (version 23.1.4). They compile with `--target=x86_64-w64-windows-gnu` and the MinGW sysroot.
- The host and the modules share `std::string` and C++ exceptions. Both pass across the border.
- `hotc.exe` and `hot-ls` build for MinGW and run under wine.
- The runtime tests pass under wine.
- Precompiled mode and reload mode work. Tests cover them natively and under wine.

## Quick start with a preset

The preset `mingw-llvm` builds a Windows host on Linux. It needs the MinGW-w64 cross compiler and wine. It also needs the unpacked LLVM release in the folder `cmake-build`. The preset sets `HOT_LLVM_RELEASES_DIR` to this folder. To unpack the release, run this command:

```
zstd -d --long=31 -c clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.zst | tar -x -C cmake-build
```

Then configure, build and test:

```
cmake --preset mingw-llvm
cmake --build --preset mingw-llvm-dbg
ctest --preset mingw-llvm-dbg
```

The preset builds `hotc.exe` and `hot-ls`. The tests run under wine. See [Presets](cmake-presets.html#windows-presets) for all Windows presets.

## Download LLVM

The script modules need the LLVM release for Windows. It has `clang++.exe`, `libclang.dll` and the compiler headers. Hot++ can download it for you.

Set `HOT_DOWNLOAD_LLVM=ON`, and leave `HOT_LLVM_DIR` and `HOT_LLVM_RELEASES_DIR` empty. The configure step downloads the archive `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.zst` (263 MB). It checks the SHA256 of the archive and unpacks it. The build then sets `HOT_SCRIPT_COMPILER` to the `clang++.exe` of the release.

To use a release that you unpacked before, set `HOT_LLVM_DIR` to its folder. No download occurs then. `HOT_LLVM_RELEASES_DIR` is a folder that holds unpacked releases. The build uses the release of the right version in it, and does not download.

The preset `mingw-llvm` sets `HOT_LLVM_RELEASES_DIR`. To download with this preset, clear the value:

```
cmake --preset mingw-llvm -DHOT_LLVM_RELEASES_DIR= -DHOT_DOWNLOAD_LLVM=ON
```

To use another archive, set `HOT_LLVM_URL` and `HOT_LLVM_SHA256` together. Your values override the defaults. See [Options](cmake-options.html#windows-options).

## Cross build from Linux

Use the toolchain file `cmake/toolchains/mingw-w64-x86_64.cmake`. It selects the MinGW-w64 compilers and uses wine to run the programs.

You need these tools:

- `x86_64-w64-mingw32-g++` and its sysroot `/usr/x86_64-w64-mingw32`.
- `wine`.
- A `hotc` that runs on Linux, only for the preset `mingw-files`.

The preset `mingw-files` has no translator. A `hotc` from the Linux host makes the modules. Set `HOT_HOST_HOTC` to the path of that `hotc`.

```
cmake --preset mingw-files -DHOT_HOST_HOTC=/path/to/hotc
cmake --build --preset mingw-files-dbg
```

The preset `mingw-llvm` builds `hotc.exe` for Windows, so it needs no Linux `hotc`.

The toolchain file sets `HOT_HOTC_TARGET` to `x86_64-w64-mingw32` and `HOT_SCRIPT_SYSROOT` to the MinGW sysroot. Change them in the cache if your MinGW is in another folder.

The MinGW host exports all its symbols, so the modules reach the runtime. A host with `ENABLE_EXPORTS ON` gets this through `hot_export_host(<target>)`. On Windows `hot_add_scripts` has the option `HOST`. Give it the host executable. Each module then links the import library of this host:

```
set_target_properties(game PROPERTIES ENABLE_EXPORTS ON)
hot_export_host(game)
hot_add_scripts(TARGET game_scripts SCRIPTS game.hot HOST game)
```

Other systems ignore `HOST`.

## The SDK on Windows

The install of a `mingw-llvm` build has `libclang.dll` in `bin/` and the Clang headers in `lib/clang/<major>/include`. `HotConfig.cmake` gives these headers to `hotc`. A path of the build host that does not exist on your machine is not used. The sysroot then is the parent folder of the folder of `g++`, for example `C:/msys64/ucrt64`. Set `HOT_SCRIPT_SYSROOT` to use another folder. See [Download the SDK](download.html).

## Visual Studio

The preset `vs-files` uses the generator `Visual Studio 17 2022` for x64. It works only on a Windows host. It sets `HOT_DOWNLOAD_LLVM=ON`.

```
cmake --preset vs-files
cmake --build --preset vs-files-dbg
ctest --preset vs-files-dbg
```

This preset is untested. The code paths for `cl.exe` and `clang-cl` exist, but nobody built them.

A DLL that MSVC builds exports only the symbols that have the export macro. The MinGW host exports all its symbols.

## Limits

- The JIT backend is off for MinGW. The LLVM C++ libraries of the release use the MSVC ABI only.
- The `cl.exe` and `clang-cl` paths are written, but nobody built them.
- The preset `vs-files` is untested.
- Precompiled mode and reload mode are tested natively and under wine only.
- Two language server completion tests fail under wine.

See also [Options](cmake-options.html), [Presets](cmake-presets.html) and [Limits](limits.html).
