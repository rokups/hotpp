---
layout: default
title: Presets, tests and troubleshooting
nav_order: 41
group: CMake
---

## Sanitizers and the build type

Scripts and the host must use the same sanitizer runtime. The root file `CMakeLists.txt` says that the script backends pass the same flags to the compile of the scripts, so the generation modules use the same runtime as the host.

- `make_default_backend` passes `CMAKE_CXX_FLAGS` to every script compile. A preset with `-fsanitize=address` therefore works with no more settings.
- A backend that you build yourself needs the flags in `extra_flags`. The samples `clang-files-backend` and `jit-backend` take them from `hot::default_paths().cxxflags`, which has `CMAKE_CXX_FLAGS`.
- `hot_add_scripts` in the default mode does not read `CMAKE_CXX_FLAGS`. Put the host flags in `FLAGS`: `separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")`, then `FLAGS ${host_flags}`. The sample [precompiled-library](https://github.com/rokups/hotpp/tree/main/samples/precompiled-library) does this.
- `hot_add_scripts` in AOT mode builds a normal library. It follows the flags of the project.
- Use one build folder for each mode (plain, ASan, TSan). The presets do this.
- The build type flags (`CMAKE_CXX_FLAGS_DEBUG` and the other ones) are not part of `CMAKE_CXX_FLAGS`. The script compile does not get them. The compile always uses `-g` and has no `-O` flag. Add `-O2` (or a flag you need) to `FLAGS`, `extra_flags` or `CMAKE_CXX_FLAGS` for optimized scripts.
- The JIT backend works with ThreadSanitizer.

## Presets

`CMakePresets.json` (version 6, CMake 3.25 or later) has these presets. The generator is `Ninja Multi-Config`, the configurations are `Debug` and `RelWithDebInfo`, and the build folder is `cmake-build/<preset>`. The compiler is `clang++`.

| Configure preset | Backends in the build | Sanitizer (`CMAKE_CXX_FLAGS`) | Purpose |
|---|---|---|---|
| `linux-clang-files` | clang files only | none | The default development build. |
| `linux-clang-files-asan` | clang files only | `-fsanitize=address` | Memory errors. |
| `linux-clang-files-tsan` | clang files only | `-fsanitize=thread` | Data races. |
| `linux-clang-jit` | JIT only | none | The JIT backend. It needs LLVM and Clang. |
| `linux-clang-jit-asan` | JIT only | `-fsanitize=address` | Memory errors with the JIT. |
| `linux-clang-jit-tsan` | JIT only | `-fsanitize=thread` | Data races with the JIT. |

The build presets and the test presets have the same names for each configure preset:

| Build and test preset | Configure preset | Configuration |
|---|---|---|
| `linux-clang-files-dbg` | `linux-clang-files` | Debug |
| `linux-clang-files-rel` | `linux-clang-files` | RelWithDebInfo |
| `linux-clang-files-asan` | `linux-clang-files-asan` | Debug |
| `linux-clang-files-tsan` | `linux-clang-files-tsan` | Debug |
| `linux-clang-jit-dbg` | `linux-clang-jit` | Debug |
| `linux-clang-jit-rel` | `linux-clang-jit` | RelWithDebInfo |
| `linux-clang-jit-asan` | `linux-clang-jit-asan` | Debug |
| `linux-clang-jit-tsan` | `linux-clang-jit-tsan` | Debug |

The hidden presets `base`, `base-files`, `base-jit`, `base-clang`, `base-asan`, `base-tsan` and `base-test` hold the common settings. The sanitizer presets set `CMAKE_CXX_FLAGS` to the sanitizer flag only. The test presets print the output of a failed test. Example:

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg
ctest --preset linux-clang-files-dbg
```

A consumer project can copy the presets or set the same cache variables itself.

## Troubleshooting

The first column has the text as it appears in the source. A text with `...` has a value in its place.

| Message | Cause | Fix |
|---|---|---|
| `hotc: error: this hotc is built for --format=std, not --format=fmt` (or the other way) | `hot_add_scripts` chose another format library than the one `hotc` has. The standard in your scope differed from the standard when Hot++ was configured. | Set `CMAKE_CXX_STANDARD` and `HOT_USE_FMT` before `add_subdirectory`. Do not change them later. Configure again in a new build folder. |
| `HOT_ENABLE_JIT must be AUTO, ON or OFF. It is: ...` | The cache variable has another value. | Use `AUTO`, `ON` or `OFF`. |
| `HOT_ENABLE_JIT=ON needs the LLVM and Clang CMake packages. CMake does not find them.` | You asked for the JIT backend, and the packages are missing. | Install the LLVM and Clang development packages, add them to `CMAKE_PREFIX_PATH`, or use `AUTO` or `OFF`. |
| `hot_add_host: MODE HOT_RELOAD needs a backend` (or `AOT_RELOAD`) | Hot++ has no run-time backend: `HOT_ENABLE_CLANG_FILES` and `HOT_ENABLE_JIT` are both off. | Use `MODE AOT` or `PRECOMPILED`, or turn on a backend. |
| `The JIT backend is not in this build. Configure Hot++ with HOT_ENABLE_JIT=ON ...` (a `std::runtime_error` at run time) | The program asked for the JIT backend (`BackendKind::Jit` or `HOT_BACKEND=jit`), but the build does not have it. | Build with `HOT_ENABLE_JIT=ON`, or ask for the other backend. The error for the clang files backend names `HOT_ENABLE_CLANG_FILES`. |
| `HOT_FMT_PROVIDER must be AUTO, SYSTEM, CPM or NONE. It is: ...` | A wrong value. | Use one of the four values. |
| `HOT_FMT_PROVIDER is NONE, but the target fmt::fmt-header-only or fmt::fmt does not exist...` | `NONE` needs your own fmt target. | Create the target before `add_subdirectory`, or use another provider. |
| `Hot++: fmt is not available. Check HOT_FMT_PROVIDER (now ...).` | No provider gave a fmt target. | Install fmt, or use `CPM`. |
| `hot_add_scripts needs TARGET and SCRIPTS` (also `hot_add_host needs TARGET and SCRIPTS`) | A required keyword is missing. | Give both. |
| `hot_add_host: MODE must be AOT, AOT_RELOAD, HOT_RELOAD or PRECOMPILED. It is: ...` | An unknown mode. | Use one of the four. |
| `hot_add_host: MODE AOT_RELOAD needs a backend (the target hot::backend) ...` | The Hot++ in use has no `hot::backend` target. The override needs a compiler at run time. | Use a Hot++ build or SDK with a backend, or use `MODE AOT`. |
| `hot_add_host: the target ... does not exist` | The call comes before `add_executable`. | Make the program target first. |
| `HOT_PORTABLE and HOT_BUILD_TESTS are both ON ...` | The tests need the build paths. | Set `HOT_BUILD_TESTS=OFF`, or build the tests in a normal build. |
| `hot_add_scripts: unknown arguments: ...` (also `hot_add_host: unknown arguments: ...`) | A word is not a keyword (for example a typo). | Check the spelling. Keywords are upper case. |
| `hot_add_scripts: AFTER does not work with AOT (target ...)` | `AFTER` and `AOT` in one call. | Remove `AFTER`. AOT makes only generation 1. |
| `hot_add_scripts: PRELUDES works only with AOT (target ...)` | `PRELUDES` in the default mode. | Add `AOT` or remove `PRELUDES`. |
| `hot_script_host needs NAME and SCRIPT_DIRS or SCRIPTS` | A required keyword is missing. | Give `NAME` and one of the two lists. |
| `hot_script_host: the target ... does not exist` | `TARGET` names a target that is not defined yet. | Call the function after `add_executable`. |
| `hotc: error: two scripts have the name ...` | Two scripts have the same file name in one call. | Give the files different names. |
| `hotc: error: cannot read ...` | A script file does not exist. | Check the path in `SCRIPTS`. |
| `the compile of ... failed (exit code ...)` | The C++ compiler rejected the generated code. The text before it has the compiler diagnostics. | Read the diagnostics. Check `INCLUDES`, `FLAGS` and the standard. |
| `the script '...' is not found` | A `using "x.hot";` line names a script that the translator cannot read. | Check the path. Add the folder to `INCLUDES`. |
| `base class '...' is not found in the imported headers` | The header of the native base class is not in the `using` lines or not in the include folders. | Add the header with `using`. Add its folder to `INCLUDES`. |
| `symbol script_register not found in ...` | The file is a library, but not an Hot++ generation module. | Load a library that `hotc` made. |
| `the class ... is in two AOT scripts (...)` | Two AOT scripts have a class of the same name. It is an exception of `AotRegistry`. | Rename one class. |
| `no AOT script class ...` | `AotRegistry::create` got an unknown class name. | Check the name and the scripts in `SCRIPTS`. |
| `the loader already has a generation: call reset first` | `Loader::load` ran twice. | Use `request_reload` for a later generation, or call `reset` first. |
| `class ...: generation ... is not newer than the active generation ...` | The module has the same generation number as the active one. | Use a higher `--gen` number. |
| `cannot write ...` | The backend or `hotc` cannot write the slot file or an output file. | Check the rights of the `OUT_DIR` or the work folder. |

Some problems have no fixed Hot++ message. The text comes from the tool that reports them.

| Symptom | Cause | Fix |
|---|---|---|
| The linker reports an undefined reference to `hot::aot_table`. | A program that uses `AotRegistry` does not link the library from `hot_add_scripts(... AOT ...)`. Only that library defines the table. | Link the AOT library. |
| `Loader::load` or `module_from_library` fails and `error` has the `dlopen` text with `undefined symbol`. | The host is not linked with `ENABLE_EXPORTS`, or the host does not contain a function that the script calls. The `dlopen` text comes from the system. | Set `ENABLE_EXPORTS ON`. Link the code with the function, as a whole archive if it is a static library. |
| The JIT backend fails with `script_register not found or not linkable: ...`. | The in-memory code needs a symbol that the executable does not export (see `README.md`, the section on the JIT backend). | Same as above. |
| The configure step stops in `find_library` or `find_path` for libclang (the fallback, used when the Clang CMake package has no target `libclang`). | The development package of libclang is not installed. The message is from CMake. | Install it, or set `CMAKE_PREFIX_PATH`. |
| The JIT backend is not in the build, and you want it. | `HOT_ENABLE_JIT` is `AUTO` and CMake does not find the LLVM and Clang packages. The configure output says `JIT backend: off`. | Install the LLVM and Clang development packages, or set `HOT_ENABLE_JIT=ON` to get an error that names the problem. |
| The script compile cannot find compiler headers such as `stddef.h`. | The resource folder is wrong or empty. The root file takes it from `<compiler> -print-resource-dir`. The compiler must be Clang. MSVC has no such option. | Use `clang++` as `CMAKE_CXX_COMPILER`. For a backend that you build yourself, set `resource_dir`. For MSVC set `HOT_CLANG_RESOURCE_DIR`. |
| CMake reports an unknown command `hot_add_scripts`. | The call comes before `add_subdirectory` of Hot++. The message is from CMake. | Move the call after `add_subdirectory`. |
| A script change does not rebuild the library. | A changed header or an imported script is not in `DEPENDS`. | Add the file to `DEPENDS`. |
| A program with sanitizer flags crashes when it loads a module. | The module is compiled without the same flags. | Put the flags in `FLAGS` or `extra_flags`. See "Sanitizers and the build type". |

## hot_gtest

`hot_gtest(<target> [gtest_discover_tests arguments...] [PROPERTIES ...])` is an internal helper. It is in `cmake/HotTests.cmake`, and the build loads it only when `HOT_BUILD_TESTS` is ON. It registers the GoogleTest cases of a test target, makes each test depend on the fixture `hot_scratch`. Host projects do not call it.
