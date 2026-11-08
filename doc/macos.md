---
layout: default
title: macOS
nav_order: 41.6
group: CMake
---

## Support level

macOS support is experimental. No maintainer runs macOS. The code has macOS branches, and the CI job `macos` builds and tests them on Apple silicon. Before that job passes on `master`, treat each macOS feature as untested.

| Part | Status |
| --- | --- |
| Configure and build with the LLVM of Homebrew | CI job `macos` (preset `macos-clang-both`) |
| The test suite (files backend and JIT backend) | CI job `macos` runs `ctest` |
| AOT mode (preset `macos-clang-aot`) | No CI job. Untested. |
| Intel Macs (x86_64) | No CI job. Untested. |
| AppleClang as the host compiler | Only with the files backend. Untested. |
| A host with the hardened runtime, a signed or notarized app | Untested. See [Code signing](#code-signing). |
| An SDK package for macOS | None. The CI makes SDKs for Linux and Windows only. |
| The language server `hot-ls` on macOS | Builds. No test on macOS. |

The Linux build checks the syntax of the macOS branches (see [The guard check](#the-guard-check)). This check does not link and does not run code.

## Build

Install the LLVM of Homebrew and Ninja:

```
brew install llvm ninja
```

The presets `macos-clang-files`, `macos-clang-both` and `macos-clang-aot` use `/opt/homebrew/opt/llvm`. This is the Homebrew path on Apple silicon. For another LLVM, set `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER` and `CMAKE_PREFIX_PATH`:

```
cmake --preset macos-clang-both
cmake --build --preset macos-clang-both-rel
ctest --preset macos-clang-both-rel
```

The LLVM major version must be the version of the Linux build (`LLVM_VERSION` in `.github/workflows/ci.yml`). Homebrew has an older version as `llvm@N`.

The build links the system libc++ (`/usr/lib/libc++.dylib`). The clang++ of Homebrew uses its own libc++ headers. If the link cannot find a libc++ symbol, the headers are newer than the system library. Then use an LLVM with the same libc++, or raise the deployment target.

## Design

These rules are different from Linux:

- **Compiler.** The host must use Clang with libc++, because the script modules use Clang with libc++. CMake stops with an error for another compiler.
- **One LLVM version.** The JIT backend is the Clang library of the LLVM package. Its builtin headers (the resource directory) come from the clang++ of the same LLVM. AppleClang has another version. So with AppleClang and no `HOT_CLANG_RESOURCE_DIR`, `HOT_ENABLE_JIT=ON` is a CMake error and `AUTO` turns the JIT off.
- **Module suffix.** A module is a Mach-O library with the suffix `.so`. This is the suffix of a CMake `MODULE` library on macOS (`CMAKE_SHARED_MODULE_SUFFIX`), and `dlopen` takes any name.
- **Undefined symbols.** A module takes the runtime and the native symbols from the host when it loads. The Apple linker refuses undefined symbols by default, so the module link has `-Wl,-undefined,dynamic_lookup`. An executable on macOS exports its global symbols without `-rdynamic`.
- **No `-fno-plt`.** It is an ELF flag. Mach-O has no PLT.
- **Call depth.** ELF reads the call depth from static TLS. Mach-O uses the function `hot::detail::call_depth_slot()` (`hot/runtime/errors.h`).
- **SDK.** The script modules use the SDK of the host build (`CMAKE_OSX_SYSROOT` becomes `HOT_SCRIPT_SYSROOT`). The JIT uses `$SDKROOT`, else the output of `xcrun --show-sdk-path`.
- **JIT unwind data.** JITLink registers the DWARF unwind data of the code. The JIT compile has `-femit-dwarf-unwind=always`, so an exception can pass a JIT frame.
- **Executable path.** A portable build finds its folder with `_NSGetExecutablePath`, because macOS has no `/proc`.
- **Pipes.** macOS has no `pipe2`. `hot::detail::pipe_cloexec` uses `pipe` and `fcntl`.
- **File times.** The `stat` fields are `st_mtimespec` and `st_ctimespec`.
- **Folders.** The cache and the work folder follow the Linux rules (`$XDG_CACHE_HOME` or `~/.cache`, `$TMPDIR`). macOS sets `TMPDIR` to a folder of the user.

## Code signing

On Apple silicon, each executable page must have a signature. The Apple linker signs each module (ad-hoc signature). The JIT writes its code to memory, then makes the memory executable.

A host without the hardened runtime (a development build) needs nothing more. A host with the hardened runtime needs these entitlements:

- `com.apple.security.cs.disable-library-validation`: the files backend and the precompiled mode load modules with an ad-hoc signature.
- `com.apple.security.cs.allow-unsigned-executable-memory`: the JIT makes memory executable without `MAP_JIT`.

When `dlopen` or the JIT fails for one of these causes, the error text has a line that starts with `hot: macOS refused` and names the entitlement. This is untested.

## Known gaps

- The debugger support of the JIT (`enableDebuggerSupport`) can be unavailable on Mach-O. Then the JIT runs without it, and the debug info tests can fail.
- The file watcher of the test engine polls on macOS. It has no FSEvents or kqueue code.
- The CI job `macos` is new. Its first runs can fail. The release does not wait for it.

## The guard check

`tools/macos-guard-check/check.py` runs on Linux. It takes the compile commands of a Linux build and compiles each source with a macOS branch again. The compile has `-fsyntax-only`, `__APPLE__` defined, and `__linux__` and `__ELF__` not defined. The Linux headers stand in for the SDK. The folder `shim` adds the few macOS names.

```
tools/macos-guard-check/check.py cmake-build/linux-clang-both
```

The check finds a typing error or a missing include in a macOS branch. It does not find a wrong macOS API signature, a link error or a run-time error.
