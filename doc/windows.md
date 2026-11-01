---
layout: default
title: Windows
nav_order: 41.5
group: CMake
---

## What works

Hot++ builds and runs natively on Windows with MSVC (`cl.exe`). The CI job `windows-msvc` builds with the preset `vs-files` on `windows-latest` and runs the tests. The Windows SDK comes from this job.

- `cl.exe` builds `hotc.exe`, `hot-ls.exe`, the runtime and the hosts. They use the DLL C++ runtime (`/MD`).
- The script modules use the `clang++.exe` of the official LLVM Windows release (version 23.1.4). They compile for `x86_64-pc-windows-msvc` with the same MSVC STL and the same DLL runtime.
- The host and the modules share `std::string` and C++ exceptions. Both pass across the border.
- AOT mode, precompiled mode and reload mode work. The integration tests cover them.
- A MinGW host also works. The presets `mingw-files` and `mingw-llvm` build it on Linux, and the tests run under wine. The CI does not build it.

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

On Windows a script module is a DLL. The module reaches the host only through the exports of the host executable. A host with `ENABLE_EXPORTS ON` gets these exports through `hot_export_host(<target>)`. On Windows `hot_add_scripts` has the option `HOST`. Give it the host executable. Each module then links the import library of this host:

```
set_target_properties(game PROPERTIES ENABLE_EXPORTS ON)
hot_export_host(game)
hot_add_scripts(TARGET game_scripts SCRIPTS game.hot HOST game)
```

Other systems ignore `HOST`.

### Export your native classes

The host exports only the symbols with the macro `HOT_EXPORT`. The build does not export all symbols. The runtime marks its own symbols. You mark the native classes and functions that a script uses:

```
#include "hot/runtime/export.h"

class HOT_EXPORT Actor
{
public:
    virtual ~Actor();
    void Move(float dx);
};

HOT_EXPORT float Distance(const Vec3& a, const Vec3& b);
```

- Mark a class when it has a member that is not inline. Mark the whole class, not the members. Then the vtable and the type information come from the host.
- Mark a function that is not inline.
- Mark a global variable that a script reads. A module cannot import a variable without the macro.
- An inline class or an inline function needs no mark. The module compiles its own copy.

The host compiles the header with `__declspec(dllexport)`. The module compiles it with `HOT_MODULE_BUILD` and gets `__declspec(dllimport)`. On Linux and macOS the macro gives default visibility.

## The SDK on Windows

The install of a `mingw-llvm` build has `libclang.dll` in `bin/` and the Clang headers in `lib/clang/<major>/include`. `HotConfig.cmake` gives these headers to `hotc`. A path of the build host that does not exist on your machine is not used. The sysroot then is the parent folder of the folder of `g++`, for example `C:/msys64/ucrt64`. Set `HOT_SCRIPT_SYSROOT` to use another folder. See [Download the SDK](download.html).

## Visual Studio

The preset `vs-files` has no generator, so CMake takes the newest Visual Studio of the host. It builds for x64. It works only on a Windows host. It sets `HOT_DOWNLOAD_LLVM=ON`.

```
cmake --preset vs-files
cmake --build --preset vs-files-dbg
ctest --preset vs-files-dbg
```

The script modules need `link.exe`, the UCRT and the MSVC STL. Run CMake from a "Developer Command Prompt", or load `VsDevCmd.bat` first. The CI uses `vswhere.exe` to find `VsDevCmd.bat`.

## Limits

- The JIT backend is off for MinGW and for MSVC. The preset `vs-files` uses the clang files backend.
- The CI excludes `hotc_tool_tests` on MSVC. These tests run POSIX shell commands.
- The CI skips the `DebugInfo` tests on MSVC. The modules have CodeView debug info, and the tests read DWARF with `llvm-dwarfdump`.
- The SDK is a Release build (`/MD`). A Debug consumer (`/MDd`) cannot link its static libraries.
- The static libraries of the SDK need the MSVC toolset of the SDK build or a newer one. The release notes give this version.
- Two language server completion tests fail under wine.

See also [Options](cmake-options.html), [Presets](cmake-presets.html) and [Limits](limits.html).
