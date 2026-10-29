---
layout: default
title: Embedding
nav_order: 12
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
std::string error;
loader.load(std::move(*module.module), error);
```

## Precompiled scripts

`sppc` builds a script into a `.so` ahead of time. Any build can load it. The backend is not needed.

```
sppc player_v1.spp --gen 1 -I include -I example/engine --out-dir out/
```

```cpp
spp::LoadResult module = spp::module_from_library("out/player_v1.g1.so");
std::string error;
if (!module || !loader.load(std::move(*module.module), error)) { /* report */ }
```

The host must export the functions that the script calls (`ENABLE_EXPORTS`).

## Reload

Ask for a reload at any time. Commit it at the frame boundary.

```cpp
// next: a Module of generation 2, from a backend or from module_from_library
std::string error;
loader.request_reload(std::move(*next.module), error);     // checks only
world.SetFrameBoundaryHook([&] {
    std::string e;
    loader.commit_pending(e);
    loader.frame_boundary();                                // unloads old generations
});
```

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

`spp_add_scripts` runs `sppc` for one generation. `spp_add_aot_scripts` runs `sppc --aot` and makes a static library. `spp_script_host` tells `spp-ls` how the engine compiles the scripts. The first two call it for you.

```cmake
include(cmake/SppScripts.cmake)

spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS example/scripts/player_v1.spp
                INCLUDES include example/engine)

spp_add_aot_scripts(TARGET game_scripts SCRIPTS player.spp boss.spp
                        INCLUDES include example/engine)
target_link_libraries(game PRIVATE game_scripts)

spp_script_host(NAME game TARGET game_engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

The example is `example/aot/main.cpp`.
