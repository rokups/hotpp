---
layout: default
title: Embedding
nav_order: 13
---

The host loads scripts with a `Loader`. A development build compiles them. An AOT build links them.

## Backends

A backend turns script text into a `Module`. `CLANG_FILES` is the default. It runs an external `clang++` and loads a `.so`. `JIT` compiles in memory with the Clang library and LLVM ORC. Select one with the CMake option `SPP_BACKEND`.

```
cmake --preset linux-clang-files
cmake --preset linux-clang-jit
```

```cpp
spp::Loader loader;                       // declare first: it is destroyed last
auto backend = spp::make_default_backend();
spp::LoadResult module = backend->load({"player.spp", text}, 1);
spp::ReloadResult loaded = loader.load(std::move(*module.module));
if (!loaded) { /* report loaded.error */ }
```

## Precompiled scripts

`sppc` builds a script into a `.so` ahead of time. Any build can load it. The backend is not needed.

```
sppc player_v1.spp --gen 1 -I include -I example/engine --out-dir out/
```

```cpp
spp::LoadResult module = spp::module_from_library("out/player_v1.g1.so");
if (!module || !loader.load(std::move(*module.module))) { /* report */ }
```

The host must export the functions that the script calls (`ENABLE_EXPORTS`).

## Reload

Ask for a reload at any time. Commit it at the frame boundary.

```cpp
// next: a LoadResult of generation 2, from a backend
spp::ReloadResult r = loader.request_reload(std::move(next));   // checks only
world.SetFrameBoundaryHook([&] {
    loader.commit_pending();
    loader.frame_boundary();                                    // unloads old generations
});
```

`request_reload` returns a `spp::ReloadResult` (`include/spp/runtime/reload_result.h`). It has a `status`, a `reason`, an `error` text and the `classes` involved. The status is one of these:

| Status | Meaning |
|---|---|
| `Applied` | The new generation is active. |
| `Pending` | The module is acceptable. `commit_pending` applies it. |
| `FullReloadRequired` | Hot reload cannot apply the change. The host does a full reload. |
| `Failed` | The module is not acceptable. The live generation continues. |

The result converts to `true` for `Applied` and `Pending`.

### Full reload

For `FullReloadRequired`, the host does these steps:

1. Save the host state.
2. Drop all script objects, handles, closures and thunk pointers.
3. Call `loader.reset(error)` on the same `Loader`.
4. Call `backend->load(source, 1, spp::LoadMode::Fresh)`.
5. Call `loader.load(module)`.
6. Restore the host state.

The host does the serialization. The script has no hook for it. `reset` returns `false` and changes nothing when objects remain. The error is, for example, `2 live objects remain: Player x1, Weapon x1`. It also fails when a reload is pending or a generation is pinned. The check for a call from script code is exact only in EntryCounters mode. The host must not call `reset` from script code. After `reset`, pointers from `find()`, `module_state()` and `thunk()` are invalid. The runtime does not see a change in a native header: the host starts a full reload itself. `example/engine/Harness.cpp` (`full_reload`) is a complete sample.

## AOT compilation

An AOT build has no loader, no reload and no Clang. `sppc --aot` (the AOT mode) translates generation 1 of each script to C++. The code becomes part of the game.

```
sppc --aot player.spp boss.spp --out-dir gen -I include -I example/engine
```

```cpp
#include "spp/aot.h"

spp::AotRegistry registry(spp::aot_table);
Actor* boss = registry.create_as<Actor>("Boss");
world.Add(boss);
```

Script names and class names must be unique. Do not link `spp_runtime` and `spp_aot` in one program.

## CMake

`spp_add_scripts` runs `sppc` for one generation. With `AOT` it runs `sppc --aot` and makes a static library. `PRELUDES` works only with `AOT`. `spp_script_host` tells `spp-ls` how the engine compiles the scripts. `spp_add_scripts` calls it for you.

```cmake
include(cmake/SppScripts.cmake)

spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS example/scripts/player_v1.spp
                INCLUDES include example/engine)

spp_add_scripts(TARGET game_scripts SCRIPTS player.spp boss.spp AOT
                INCLUDES include example/engine)
target_link_libraries(game PRIVATE game_scripts)

spp_script_host(NAME game TARGET game_engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

The example is `example/aot/main.cpp`.

### Format library for `${}`

The build chooses the format library for string interpolation. No runtime switch exists.

- C++17 (the standard is less than 20): fmt. `std::format` needs C++20.
- C++20 and later: `std::format`. Set `-DSPP_USE_FMT=ON` to use fmt at any standard.

The standard is `CMAKE_CXX_STANDARD`, else 17. `spp_add_scripts` passes it to `sppc` as `--std`. The runtime backends use the standard of the spp build. The header `include/spp/config.h` has the choice (`SPP_USE_FMT`). When the build uses fmt, `spp_add_scripts` passes `--format=fmt` to `sppc`, and an AOT library links the target `spp_fmt`.

The cache variable `SPP_FMT_PROVIDER` says where fmt comes from:

| Value | Meaning |
|---|---|
| `AUTO` (default) | `find_package(fmt)`. If fmt is not installed, CPM.cmake fetches it. |
| `SYSTEM` | `find_package(fmt REQUIRED)`. The build never fetches. |
| `CPM` | CPM.cmake always fetches fmt. |
| `NONE` | Your project makes the target `fmt::fmt-header-only` or `fmt::fmt`. spp only links to it. The configure step fails if the target does not exist. |

A target `fmt::fmt-header-only` or `fmt::fmt` that exists already is always used. Then the build does not search and does not fetch.
