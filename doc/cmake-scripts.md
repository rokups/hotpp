---
layout: default
title: Script functions
nav_order: 39
group: CMake
---

## hot_add_scripts

`hot_add_scripts` is the lower-level call. `hot_add_host` calls it.

```
hot_add_scripts(TARGET name SCRIPTS a.hot b.hot
                [INCLUDES dir...] [FLAGS cxxflag...] [DEPENDS file...]
                [OUT_DIR dir] [AFTER target] [HOTC_ARGS arg...])
hot_add_scripts(TARGET name SCRIPTS a.hot b.hot AOT
                [INCLUDES dir...] [PRELUDES file...] [FLAGS hotc-flag...]
                [DEPENDS file...] [OUT_DIR dir] [HOTC_ARGS arg...])
```

The function runs `hotc` at build time. It has two modes. `AOT` selects the second mode. The function also calls `hot_script_host` with the name `TARGET`, so the language server knows the scripts. Call it after `add_subdirectory` of Hot++.

### Keywords

| Keyword | Default | Meaning | Path rule |
|---|---|---|---|
| `TARGET` | none, required | The name of the target. In the default mode it is a custom target. In AOT mode it is a static library. | none |
| `SCRIPTS` | none, required | The script files. A file is a `.hot` script or a `.cpp` script (see [C++ scripts](cpp-scripts.html)). The extension selects the parser. | A relative path is relative to the current source folder. The function makes it absolute. |
| `AOT` | off | A flag. It selects the AOT mode. | none |
| `INCLUDES` | empty | Include folders. `hotc` gets each one as `-I`. In AOT mode the library also gets them as PUBLIC include folders. The language server host gets them too. | Use an absolute path. The function passes the text to `hotc` as it is, and the custom command runs in the current build folder. The AOT include folders and the host file read a relative path from the current source folder. |
| `PRELUDES` | empty | Prelude files for `hotc --prelude`. They work only with `AOT`. The default mode stops with an error. | A relative path is relative to the current source folder. |
| `FLAGS` | empty | In the default mode: compile flags for the scripts (`--cxxflag`). In AOT mode: flags for `hotc`. | none |
| `DEPENDS` | empty | More files that the build depends on. | A relative path is relative to the current source folder (the rule of `add_custom_command`). |
| `OUT_DIR` | default mode: `${CMAKE_BINARY_DIR}/hot_scripts`. AOT: `${CMAKE_BINARY_DIR}/hot_aot/<name>`. | The output folder. | Use an absolute path. A relative path is relative to the current build folder, and then `HOT_SCRIPTS_<TARGET>_LIBRARIES` has relative paths. |
| `AFTER` | none | A target that must build first. It works only in the default mode. AOT mode stops with an error. | none |
| `HOTC_ARGS` | empty | More arguments for `hotc`, in both modes. | none |

The `hotc` call always has the include folder of Hot++ (`HOT_INCLUDE_DIR`) as the first `-I`. You do not need to put it in `INCLUDES`.

The function checks its arguments. A word that is not a keyword stops the configure step with `hot_add_scripts: unknown arguments: ...`.

### What the custom command depends on

The custom command depends on these items:

- the target `hotc`: a new build of the tool runs the command again;
- every file in `SCRIPTS`;
- every file in `DEPENDS`;
- in AOT mode, every file in `PRELUDES`.

The command does not read the headers of the script. The native headers that a script includes with `using "X.h";` are not dependencies. A script that another script imports with `using "parent.hot";` is also not a dependency. List such files in `DEPENDS`, or the build does not run `hotc` when they change. The samples use `DEPENDS Creature.h`.

### Default mode

The function makes one custom target `name`. The target makes the library `OUT_DIR/<script name>.g<n>.so` for each script. The script name is the file name without the last extension. `wolf.hot` gives `wolf.g1.so`. The command also writes the generated `.cpp` files and the slot file `OUT_DIR/slots.txt` there. Load a library with `hot::module_from_library`.

Each library gets the compile flags `-fPIC -shared -g -Werror=return-type` and the standard from the section [The C++ standard and the format library](cmake-options.html#the-c-standard-and-the-format-library). The compiler is `CMAKE_CXX_COMPILER`. `hotc` adds no `-O` flag and does not read `CMAKE_CXX_FLAGS`. Give the flags of the host in `FLAGS`.

Two scripts in one call must have different names (the file name without the extension). Else `hotc` stops with `two scripts have the name ...`.

The stem rule also applies to `.cpp` scripts. A `foo.hot` and a `foo.cpp` in one call have the same stem `foo`. The configure step fails with an error that names both scripts. The `hotc` output of a `.cpp` script is named `<input>.cpp`, for example `foo.cpp.cpp`. The backends keep the stem rule, for example `foo.g1.cpp` and `foo.g1.so`.

The target does not build automatically. Your host target must depend on it: `add_dependencies(app name)`.

### Generation numbers

The generation `n` in the file name is 1. The option `--gen n` in `HOTC_ARGS` sets another number. The function reads the number for the output names, and `hotc` gets the option. A reload needs a new generation that is newer than the active one. A module with the same number gives `GenerationNotNewer`.

The generations of one class share the slot file `OUT_DIR/slots.txt`. This file keeps the slot order between generations. So build generation 2 after generation 1 and use the same `OUT_DIR`. Give `AFTER` the target of generation 1. `AFTER` only orders the targets. It adds a dependency of the target, not of the files.

### Variables that the function sets

| Variable | Scope | Mode | Value |
|---|---|---|---|
| `HOT_SCRIPTS_<TARGET>_LIBRARIES` | the scope of the caller (`PARENT_SCOPE`) | default | The paths of the libraries, one for each script. |
| `HOT_INCLUDE_DIR` | the cache (INTERNAL) | both | The include folder of Hot++. Set when the Hot++ file is loaded, not by the call. |

AOT mode sets no variable. The function also writes the file `${HOT_HOSTS_DIR}/<TARGET>.json` when CMake generates (see `hot_script_host`).

### Multi-config generators

With `Ninja Multi-Config` or another multi-config generator, the default `OUT_DIR` has no part for the configuration. All configurations write to the same files, and `FLAGS` are the same for all of them. Build one configuration at a time in one tree. The libraries of the AOT mode are normal CMake libraries, so they follow the configuration. This behavior comes from the code of `hot_add_scripts`. A build with two configurations in one tree is untested.

### Worked example: default mode with generations

```cmake
separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
set(engine ${CMAKE_SOURCE_DIR}/engine)

hot_add_scripts(TARGET gen1 SCRIPTS player_v1.hot
                INCLUDES ${engine} FLAGS ${host_flags} DEPENDS ${engine}/Actor.h)
hot_add_scripts(TARGET gen2 SCRIPTS player_v2.hot AFTER gen1 HOTC_ARGS --gen 2
                INCLUDES ${engine} FLAGS ${host_flags} DEPENDS ${engine}/Actor.h)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE hot_runtime)
set_target_properties(app PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(app gen2)

enable_testing()
add_test(NAME app COMMAND app ${HOT_SCRIPTS_gen1_LIBRARIES} ${HOT_SCRIPTS_gen2_LIBRARIES})
```

The build makes these files in `${CMAKE_BINARY_DIR}/hot_scripts`: `player_v1.g1.so`, `player_v2.g2.so` and `slots.txt`. The program loads generation 1 with `load` and generation 2 with `request_reload`. See [Hot reload](hot-reload.html). The call in `tests/scripts/CMakeLists.txt` uses the same keywords.

### Worked example: AOT mode

```cmake
hot_add_scripts(TARGET game_scripts AOT SCRIPTS player.hot boss.hot
                INCLUDES ${CMAKE_SOURCE_DIR}/engine
                PRELUDES ${CMAKE_SOURCE_DIR}/scripts/stubs.hot
                DEPENDS ${CMAKE_SOURCE_DIR}/engine/Actor.h
                HOTC_ARGS --no-strict-borrowed)
add_executable(game main.cpp)
target_link_libraries(game PRIVATE game_scripts)
```

The function runs `hotc --aot` and makes the static library `name`. The library has the files `OUT_DIR/<script name>.aot.cpp` and `OUT_DIR/hot_aot_registry.cpp`. The library defines `hot::aot_table` (`include/hot/aot.h`).

The library links `hot_runtime_core` (PUBLIC). A program that links `name` and `hot::aot` has no loader, no reload and no Clang. When the build uses fmt, the library also links `hot_fmt`. AOT mode makes only generation 1. Script names and class names must be unique across the scripts of one table.

The library is a normal target, so it uses the compile flags of your project (`CMAKE_CXX_FLAGS` and the build type). `FLAGS` go to `hotc`, not to the compiler.

## hot_script_host

```
hot_script_host(NAME name [TARGET target] [SCRIPT_DIRS dir...] [SCRIPTS file...]
                [INCLUDES dir...] [FLAGS flag...])
```

The function describes a script host for the language server `hot-ls`. It does not build anything. When CMake generates the build system, it writes `${HOT_HOSTS_DIR}/<name>.json`. With `TARGET`, it also writes three side files in the same folder.

`NAME` and one of `SCRIPT_DIRS` or `SCRIPTS` are required. `TARGET` is the CMake target of your host program. The file gets its include folders, defines and options. The include folder of Hot++ is always first. A relative path in `SCRIPT_DIRS`, `SCRIPTS` or `INCLUDES` is relative to the current source folder (`sourceDir` in the file). A UTILITY target (a custom target) has no such lists, so the function skips them.

```cmake
hot_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

You do not need this call for scripts that `hot_add_scripts` builds, because that function calls it. Use it for a host that compiles its scripts at run time. See [Tools](tools.html) and the section "Automatic setup" of `README.md`.

### The files

| File | When | Content |
|---|---|---|
| `<name>.json` | always | `version` (1), `name`, `binaryDir`, `sourceDir`, `compiler`, `std`, `resourceDir`, `scriptDirs`, `scripts`, `includeDirs`, `defines` (always empty), `flags`. With `TARGET`: `definesFile`, `optionsFile` and `includesFile`. |
| `<name>.defines` | `TARGET` is a real target | The compile definitions of the target, one on each line. |
| `<name>.options` | `TARGET` is a real target | The compile options of the target, one on each line. |
| `<name>.includes` | `TARGET` is a real target | The raw `INCLUDE_DIRECTORIES` of the target, one on each line. A relative entry is relative to `sourceDir`. |

The folder is `HOT_HOSTS_DIR`. The default is `${CMAKE_SOURCE_DIR}/.cache/hot/hosts`. Add `.cache/` to the ignore list of your project. The side files exist because a quote or a backslash in a path cannot pass through a generator expression into JSON. A path with a newline, and a path with a semicolon, do not work.

`hot_add_scripts` calls `hot_script_host` without `TARGET`, so it writes only the JSON file. The `includeDirs` entry has the include folder of Hot++, the `INCLUDES` and (with fmt) the fmt folder. In default mode `flags` has `FLAGS` and the fmt flags.

Two notes on the fields:

- `std`: with `TARGET`, the `CXX_STANDARD` property of the target, else `CMAKE_CXX_STANDARD`, else 17. Without `TARGET` it is `CMAKE_CXX_STANDARD` or 17. It agrees with the default 17 of Hot++.
- `resourceDir`: the value of `CLANG_RESOURCE_DIR`. A consumer project does not see this Hot++ variable, so the field can be empty. Then `hot-ls` uses its own `--resource-dir` option or its default.
