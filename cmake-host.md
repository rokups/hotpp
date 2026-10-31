---
layout: default
title: Hosts with hot_add_host
nav_order: 38
group: CMake
---

## `hot_add_host`

```
hot_add_host(TARGET name [MODE AOT|AOT_RELOAD|HOT_RELOAD|PRECOMPILED] SCRIPTS a.hot b.hot
             [INCLUDES dir...] [DEPENDS file...] [PRELUDES file...]
             [FLAGS flag...] [HOTC_ARGS arg...])
```

This is the call that most hosts need. Make your program with `add_executable` first. Then call `hot_add_host` with the same name. The call links the Hot++ target that the mode needs, sets `ENABLE_EXPORTS` when the mode needs it, and sets up the scripts. It uses `hot_add_scripts` and `hot_script_host` inside. Call it after `add_subdirectory` of Hot++.

### Keywords

| Keyword | Default | Meaning |
|---|---|---|
| `TARGET` | none, required | The program target. It must exist before the call. |
| `MODE` | `HOT_RELOAD` | How the program gets its scripts. See the next table. |
| `SCRIPTS` | none, required | The script files (`.hot` or `.cpp`). A relative path is relative to the current source folder. Two scripts in one call need different names (the file name without the extension). |
| `INCLUDES` | empty | Include folders. The program, `hotc` and the language server get them. Use absolute paths. |
| `DEPENDS` | empty | More files that the build depends on, for example your headers. The build does not track the headers of a script by itself. |
| `PRELUDES` | empty | Prelude files for `hotc --prelude`. Only for `MODE AOT` and `MODE AOT_RELOAD`. |
| `FLAGS` | empty | Compile flags for the scripts (`PRECOMPILED`) or flags for `hotc` (`AOT`). |
| `HOTC_ARGS` | empty | More arguments for `hotc`. |

A word that is not a keyword, a missing `TARGET` or `SCRIPTS`, and an unknown `MODE` stop the configure step with an error.

### Modes

| Mode | What the build does | What the call links and sets | At run time |
|---|---|---|---|
| `AOT` | `hotc` translates the scripts to C++. The build makes the static library `<TARGET>_scripts`. | The program links `<TARGET>_scripts` and `hot::aot` (`PRIVATE`). The library links only `hot::runtime_core`. | No loader. No compiler. The scripts are in the binary. |
| `AOT_RELOAD` | `hotc` translates the scripts to C++ as in `AOT`. The build makes the static library `<TARGET>_scripts`. The override files are not in `SCRIPTS`. | The program links `<TARGET>_scripts` and `hot::backend` (not `hot::aot`) and gets `ENABLE_EXPORTS`. A development build also gets the define `HOT_SCRIPT_DIR`, the folder of the first script. | The program starts with `Loader::load_aot(hot::aot_table)`. Then a backend compiles an override of one script at run time, and `Loader::revert_unit` goes back to the AOT code. The host needs a compiler for the override. See [AOT with run-time overrides](embedding-backends.html#aot-with-run-time-overrides). |
| `PRECOMPILED` | `hotc` builds a script library (`.so`) for each script, in `${CMAKE_BINARY_DIR}/hot_scripts/<TARGET>`. The program depends on the target `<TARGET>_scripts`. | The program links `hot::runtime` and gets `ENABLE_EXPORTS`. The variable `HOT_SCRIPTS_<TARGET>_LIBRARIES` has the library paths (in the scope of the caller). | The program loads the libraries with `hot::module_from_library`. No compiler. |
| `HOT_RELOAD` | The build does not compile the scripts. | The program links `hot::backend` and gets `ENABLE_EXPORTS`. A development build also gets the define `HOT_SCRIPT_DIR`, the folder of the first script. | The backend compiles the scripts. The host needs a compiler. The scripts can reload. |

```cmake
add_executable(game main.cpp)
hot_add_host(TARGET game MODE HOT_RELOAD
             SCRIPTS script/player.hot script/boss.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/engine DEPENDS engine/Actor.h)
```

In every mode the program also gets `INCLUDES` as private include folders.

With `HOT_PORTABLE` `ON` and `MODE HOT_RELOAD`, the call does not define `HOT_SCRIPT_DIR`, because that is a build path. Instead it copies the Hot++ headers to `hot/include` next to the program after each build. The host finds them there at run time. See [Portable binaries](cmake-sdk.html#portable-binaries). Give the script folder to the host in another way, for example with an argument.

Use `hot_add_scripts` directly when you need more control: more than one generation (`AFTER`, `--gen`), your own `OUT_DIR`, or a script library without a program target. The page [Script functions](cmake-scripts.html) describes it.
