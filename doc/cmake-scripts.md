---
layout: default
title: Script functions
nav_order: 39
group: CMake
---

## `hot_add_scripts`

`hot_add_scripts` is the lower-level call. `hot_add_host` calls it.

```
hot_add_scripts(TARGET name SCRIPTS a.hot b.hot
                [INCLUDES dir...] [FLAGS cxxflag...] [DEPENDS file...]
                [OUT_DIR dir] [AFTER target] [HOTC_ARGS arg...])
hot_add_scripts(TARGET name SCRIPTS a.hot b.hot AOT [OPTIMIZED]
                [INCLUDES dir...] [PRELUDES file...] [FLAGS hotc-flag...]
                [DEPENDS file...] [OUT_DIR dir] [UNIT_ROOT dir] [PARSE_TARGET target]
                [RUNTIME_TARGET target] [HOST_TARGET target] [PRELUDE_SUFFIX suffix] [HOTC_ARGS arg...])
```

The function runs `hotc` at build time. It has two modes. `AOT` selects the second mode. The function also calls `hot_script_host` with the name `TARGET`, so the language server knows the scripts. Call it after `add_subdirectory` of Hot++.

### Keywords

| Keyword | Default | Meaning | Path rule |
|---|---|---|---|
| `TARGET` | none, required | The name of the target. In the default mode it is a custom target. In AOT mode it is a static library. | none |
| `SCRIPTS` | none, required | The script files. A file is a `.hot` script or a `.cpp` script (see [C++ scripts](cpp-scripts.html)). The extension selects the parser. | A relative path is relative to the current source folder. The function makes it absolute. |
| `AOT` | off | A flag. It selects the AOT mode. | none |
| `OPTIMIZED` | off | A flag. It works only with `AOT`. It builds optimized AOT code: a call from the host into a script is as fast as a native virtual call, and the code cannot be overridden or reloaded (see [Optimized AOT](embedding-backends.html#optimized-aot)). `OPTIMIZED` without `AOT` stops with an error: `hot_add_scripts: OPTIMIZED works only with AOT`. | none |
| `INCLUDES` | empty | Include folders. `hotc` gets each one as `-I`. In AOT mode the library also gets them as `PUBLIC` include folders. The language server host gets them too. | Use an absolute path. The function passes the text to `hotc` as it is, and the custom command runs in the current build folder. The AOT include folders and the host file read a relative path from the current source folder. |
| `PRELUDES` | empty | Prelude files for `hotc --prelude`. They work only with `AOT`. The default mode stops with an error. | A relative path is relative to the current source folder. |
| `FLAGS` | empty | In the default mode: compile flags for the scripts (`--cxxflag`). In AOT mode: flags for `hotc`. | none |
| `DEPENDS` | empty | More files that the build depends on. | A relative path is relative to the current source folder (the rule of `add_custom_command`). |
| `OUT_DIR` | default mode: `${CMAKE_BINARY_DIR}/hot_scripts`. AOT: `${CMAKE_BINARY_DIR}/hot_aot/<name>`. | The output folder. | Use an absolute path. A relative path is relative to the current build folder, and then `HOT_SCRIPTS_<TARGET>_LIBRARIES` has relative paths. |
| `UNIT_ROOT` | none | A folder. It works only with `AOT`. Each script gets `--unit-name <path below the folder, without the extension>`, for example `Game/Game`. The unit names then equal the names at run time. A script outside the folder is an error. | A relative path is relative to the current source folder. |
| `PARSE_TARGET` | none | A target. It works only with `AOT`. The include folders (`INCLUDE_DIRECTORIES`) and the definitions (`COMPILE_DEFINITIONS`) of the target go to `hotc --aot` as `--includes-file` and `--defines-file`. The files are `OUT_DIR/parse_includes.txt` and `OUT_DIR/parse_defines.txt`, one entry for each line, and the command depends on them. | The properties of the target only. The usage requirements of the targets that it links are not included: list them in `INCLUDES`. |
| `RUNTIME_TARGET` | `hot::runtime_core` | A target. It works only with `AOT`. The library links this target (`PUBLIC`) instead of `hot::runtime_core`. Use it when the program already links the shared runtime. | none |
| `HOST_TARGET` | none | A target. It works only with `AOT`. The language server host `name` gets the definitions, the options and the include folders of this target (the files `name.defines`, `name.options` and `name.includes`), as `hot_script_host(TARGET ...)` does. Without it the host has only `INCLUDES`. | The target must exist when the call runs. |
| `PRELUDE_SUFFIX` | none | A suffix, for example `.prelude.hot`. It works only with `AOT`. It goes to the host as `preludeSuffix`. `PRELUDES` go to the host too. | none |
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

`hotc` writes a depfile (`--depfile`) with the files that the translation read. The native headers that a script includes with `using "X.h";` are dependencies. A script that another script imports with `using "parent.hot";` is also a dependency. The system headers, such as the headers of the standard library, are not dependencies. A change of one of these files runs `hotc` again. The depfile needs the Ninja generator or a Makefile generator. With another generator, list these files in `DEPENDS`. `DEPENDS` is for other files that the command needs.

### Default mode

The function makes one custom target `name`. The target makes the library `OUT_DIR/<script name>.g<n>.so` for each script. The script name is the file name without the last extension. `wolf.hot` gives `wolf.g1.so`. The command also writes the generated `.cpp` files and the slot file `OUT_DIR/slots.txt` there. Load a library with `hot::module_from_library`.

Each library gets the compile flags `-fPIC -shared -g -Werror=return-type` and the standard from the section [The C++ standard and the format library](cmake-options.html#the-c-standard-and-the-format-library). The compiler is `CMAKE_CXX_COMPILER`. `hotc` adds no `-O` flag and does not read `CMAKE_CXX_FLAGS`. Give the flags of the host in `FLAGS`.

Two scripts in one call must have different names (the file name without the extension). Else `hotc` stops with `two scripts have the name ...`.

The stem rule also applies to `.cpp` scripts. A `foo.hot` and a `foo.cpp` in one call have the same stem `foo`. The configure step fails with an error that names both scripts. The `hotc` output of a `.cpp` script is named `<input>.cpp`, for example `foo.cpp.cpp`. The backends keep the stem rule, for example `foo.g1.cpp` and `foo.g1.so`.

The target does not build automatically. Your host target must depend on it: `add_dependencies(app name)`.

### Generation numbers

The generation `n` in the file name is 1. The option `--gen n` in `HOTC_ARGS` sets another number. The function reads the number for the output names, and `hotc` gets the option. A reload needs a new generation that is newer than the active one. A module with the same number gives `GenerationNotNewer`. The module exports the number as the symbol `hot_module_generation`, and `module_from_library` reads it. A module of a backend has no such symbol: the loader gives it the next number. A second `module_from_library` of a loaded library gives the same code, so a reload with it fails with `BadModule`.

The generations of one class share the slot file `OUT_DIR/slots.txt`. This file keeps the slot order between generations, also the defunct slots of removed functions. So build generation 2 after generation 1 and use the same `OUT_DIR`. Give `AFTER` the target of generation 1. `AFTER` only orders the targets. It adds a dependency of the target, not of the files.

### Variables that the function sets

| Variable | Scope | Mode | Value |
|---|---|---|---|
| `HOT_SCRIPTS_<TARGET>_LIBRARIES` | the scope of the caller (`PARENT_SCOPE`) | default | The paths of the libraries, one for each script. |
| `HOT_INCLUDE_DIR` | the cache (`INTERNAL`) | both | The include folder of Hot++. Set when the Hot++ file is loaded, not by the call. |

AOT mode sets no variable. The function also writes the file `${HOT_HOSTS_DIR}/<TARGET>.json` when CMake generates (see `hot_script_host`).

### Multi-config generators

In the AOT mode, a multi-config generator adds the part `/<config>` to `OUT_DIR` (`$<CONFIG>`), so each configuration has its own generated files and its own `PARSE_TARGET` lists. `hotc` writes an output file only when its content changes, and Ninja uses `restat`, so an unchanged output starts no compile. The next text is for the default mode: the default `OUT_DIR` has no part for the configuration. All configurations write to the same files, and `FLAGS` are the same for all of them. Build one configuration at a time in one tree. The libraries of the AOT mode are normal CMake libraries, so they follow the configuration. This behavior comes from the code of `hot_add_scripts`. A build with two configurations in one tree is untested.

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

The library links `hot_runtime_core` (`PUBLIC`). A program that links `name` and `hot::aot` has no loader, no reload and no Clang. When the build uses fmt, the library also links `hot_fmt`. AOT mode makes only generation 1. Script names and class names must be unique across the scripts of one table.

Add `OPTIMIZED` after `AOT` to build optimized AOT code. The program that links the library can call the scripts faster. It cannot override or reload them. Use it for a shipped build.

The library is a normal target, so it uses the compile flags of your project (`CMAKE_CXX_FLAGS` and the build type). `FLAGS` go to `hotc`, not to the compiler.

### Cross builds

The AOT step runs `hotc` on the build host, also when the target is web (Emscripten), iOS, Android or UWP. So `hotc` must be a host executable. Do not build it for the target.

1. Build `hotc` in a native build of Hot++. Keep the path of the file.
2. Configure the cross build with `-DHOT_HOTC_EXECUTABLE=/path/to/hotc`.

```sh
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files --target hotc
emcmake cmake -S game -B build-web -DHOT_HOTC_EXECUTABLE=/path/to/hotc
```

When `HOT_HOTC_EXECUTABLE` is set, the custom command of an AOT target runs this file. The command depends on the file, so a new `hotc` makes the generated code again. The project does not build the target `hot::hotc` and does not need it. libclang is not needed for the target.

The variable has priority over the target `hot::hotc`, but only in AOT mode. The default mode (no `AOT`) still runs `hot::hotc`. `HOT_HOST_HOTC` is a different variable: it makes an imported target `hot::hotc` for the builds that do not build the translator. A relative `HOT_HOTC_EXECUTABLE` is relative to the top source folder. The file must exist when the build runs, not when CMake configures. Thus another build can make it.

When the variable is not set, the build is a cross build and the target `hot::hotc` does not exist, `hot_add_scripts` stops with an error. The error message tells you to set `HOT_HOTC_EXECUTABLE`. A native build does not change. It uses the target `hot::hotc` as before.

The compile of the generated `.aot.cpp` is a normal compile of the target toolchain. The include directories of Hot++ and of the runtime come from the usage requirements of the library `name`, which links `hot::runtime_core` (`PUBLIC`). They do not come from the host tool. `hotc` uses `HOT_INCLUDE_DIR`, `INCLUDES` and `PARSE_TARGET` only to parse the scripts. Use `HOT_HOTC_TARGET` and `HOT_SCRIPT_SYSROOT` when the parse must see the target headers. The cache variable `CLANG_RESOURCE_DIR` sets the Clang resource directory of the parse.

The test `hotc_executable` builds the sample `samples/aot` with the translator off and with a built `hotc`. It checks that the project has no target `hotc`. It also checks the error message of a cross build with no `hotc`.

### Precompiled scripts and run-time reload

A build can use both ways. This is the hybrid design (`MODE AOT_RELOAD` of `hot_add_host`).

- The AOT code is generation 1. It is in the game binary. The game starts with it, and no compiler is needed.
- Each script is one unit. With `UNIT_ROOT`, the unit name is the path of the script below the root, without the extension. Without `UNIT_ROOT`, the unit name is the script name. For example, `UNIT_ROOT` set to `scripts` gives `ai/wolf` for `scripts/ai/wolf.hot`. The run-time override must use the same unit name.
- The AOT table keeps two hashes. `source_hash` is the hash of the script text. `env_hash` is the hash of the build environment (preludes, format library, view type, standard). A unit is stale when the text on disk has another `source_hash`, or when the `env_hash` of the build differs from the hash of the run-time compile.
- A stale or changed script compiles at run time. The new module replaces the unit. Other units do not change.
- A change of the interface of an imported unit makes all importers compile again. A change of only a body compiles only that unit.
- `Loader::revert_unit` returns a unit to its AOT code. Use it to drop an override.

See [AOT with run-time overrides](embedding-backends.html#aot-with-run-time-overrides) for the calls.

## `hot_script_host`

```
hot_script_host(NAME name [TARGET target] [SCRIPT_DIRS dir...] [SCRIPTS file...]
                [INCLUDES dir...] [FLAGS flag...] [PRELUDES file...]
                [PRELUDE_SUFFIX suffix])
```

The function describes a script host for the language server `hot-ls`. It does not build anything. When CMake generates the build system, it writes `${HOT_HOSTS_DIR}/<name>.json`. With `TARGET`, it also writes three side files in the same folder.

`NAME` and one of `SCRIPT_DIRS` or `SCRIPTS` are required. `TARGET` is the CMake target of your host program. The file gets its include folders, defines and options. The include folder of Hot++ is always first. A relative path in `SCRIPT_DIRS`, `SCRIPTS`, `INCLUDES` or `PRELUDES` is relative to the current source folder (`sourceDir` in the file). A `UTILITY` target (a custom target) has no such lists, so the function skips them.

`PRELUDES` names the prelude files that your host gives to the translator at run time (`TranslateOptions::preludes`). The file has them in `preludes`. `hot-ls` gives them to each script of the host. Thus it knows the stubs and the script attributes of the prelude, as `hotc --prelude` does. A prelude file that is open in the editor does not get its own text as a prelude.

`PRELUDE_SUFFIX` names the file suffix of user preludes, for example `.prelude.hot`. The file has it in `preludeSuffix`. At each analysis, `hot-ls` searches the script dirs (with subfolders) for files with this suffix. It sorts them by path and gives them after the `PRELUDES`. Thus a new user prelude works at once. Your host must find the same files in the same order.

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
