---
layout: default
title: Setup and targets
nav_order: 36
group: CMake
---

## Requirements

| Item | Requirement |
|---|---|
| CMake | 3.20 or later (`cmake_minimum_required` of the root file). The presets in `CMakePresets.json` need 3.25. |
| Generator | Any generator. The presets use `Ninja Multi-Config`, so they need Ninja. |
| C++ compiler | A Clang C++ compiler. The presets set `clang++`. The configure step runs `<compiler> -print-resource-dir`. The build does not check which compiler you use. Other compilers are untested. |
| C++ standard | The default is C++17. The host, `hotc`, the language server and the tests all build as C++17. The host can use C++20 or C++23 when it sets `CMAKE_CXX_STANDARD`. |
| `libclang` | The C API (`libclang`) and the header `clang-c/Index.h`, from the system. The configure step first runs `find_package(Clang CONFIG QUIET)` and links the imported target `libclang`. The package must have the same major version as the compiler that sets the Clang resource folder (`CLANG_RESOURCE_DIR`), and the step looks first in `<resource folder>/../../cmake/clang`. If the package has no such target or has another version, the step uses the fallback: `find_library(NAMES clang REQUIRED)` and `find_path(clang-c/Index.h REQUIRED)`. The configure step fails if the fallback finds nothing. Every build needs `libclang`, also an AOT build, because `hotc` links it. |
| Threads | `find_package(Threads REQUIRED)`. |
| LLVM and Clang | Only for the JIT backend. With `HOT_ENABLE_JIT=AUTO` (the default) the configure step runs `find_package(LLVM CONFIG)` and `find_package(Clang CONFIG)`, and the JIT backend is on only when both are found. With `HOT_ENABLE_JIT=ON` the configure step stops with an error when a package is missing. With `OFF` it does not look for them. The JIT backend links `clang-cpp` and `LLVM`. The root file also enables the language C when `HOT_ENABLE_JIT` is not `OFF`. |
| Platform | Linux is the tested platform. Windows and macOS are untested. See [Limits](limits.html). |

A host that links only `hot_runtime` (a precompiled library) or only `hot_aot` has no `libclang` or Clang at run time. A host that links `hot_backend` needs the libraries of the backend at run time, and `ClangFilesBackend` also needs the compiler (see [Embedding](embedding.html)).

### What CPM fetches

Hot++ uses `CPM.cmake` (`cmake/CPM.cmake`, in the repository, so CPM itself needs no download). The root file sets `CPM_USE_LOCAL_PACKAGES` to `ON` when you do not define it. So CPM calls `find_package` first and fetches only a package that is missing.

| Package | Version | When |
|---|---|---|
| `lexy` | v2025.05.0 | Always. |
| `nlohmann_json` | 3.12.0 (a release archive) | Always. The language server uses it. |
| GoogleTest | v1.17.0 | Only when `HOT_BUILD_TESTS` is `ON`. |
| fmt | 11.2.0 | Only when the build uses fmt and no fmt is installed (see `HOT_FMT_PROVIDER`). |
| Dear ImGui | v1.92.9b | Only in the sample `samples/imgui`, and only when `find_package(SDL3)` succeeds (see `README.md`). |

`libclang`, the Clang compiler, Threads, LLVM and Clang (for JIT) come from the system. CPM never fetches them.

### Offline builds

| Setting | Effect |
|---|---|
| `-DCPM_USE_LOCAL_PACKAGES=OFF` | Fetch every CPM package. Hot++ sets the default to `ON`. |
| `-DCPM_DOWNLOAD_ALL=ON` | Fetch every CPM package, also when an installed one exists. |
| `-DCPM_LOCAL_PACKAGES_ONLY=ON` | Never fetch. The configure step fails when a package is missing. |
| `-DCPM_SOURCE_CACHE=<dir>` | Keep the fetched sources in one folder. The next configure does not download again. The environment variable `CPM_SOURCE_CACHE` gives the default. |
| `-DCMAKE_DISABLE_FIND_PACKAGE_<name>=ON` | Fetch one package. Use `GTest`, `lexy` or `nlohmann_json` as the name. |

For an offline build, install `lexy`, `nlohmann_json` (and GoogleTest, if you build the tests) or fill the cache with `CPM_SOURCE_CACHE` on a machine that has network access. Then set `CPM_SOURCE_CACHE` to that folder.

The fmt package has its own switch. For an offline build with fmt, set `HOT_FMT_PROVIDER` to `SYSTEM` (an installed fmt) or `NONE` (your project makes the fmt target). Both values never fetch.

## Consume Hot++

Add the Hot++ source tree with `add_subdirectory`. The second argument is a build folder, because the tree is outside your source tree.

```cmake
set(HOT_ROOT "/path/to/hotpp" CACHE PATH "The Hot++ source tree")
add_subdirectory(${HOT_ROOT} hot)

add_executable(app main.cpp)
hot_add_host(TARGET app MODE AOT SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
```

The call to `add_subdirectory` defines the functions `hot_add_host`, `hot_add_scripts` and `hot_script_host`. It also defines the targets that the next table lists.

`FetchContent` gives the same result. Replace the first two lines of the example:

```cmake
include(FetchContent)
FetchContent_Declare(hotpp GIT_REPOSITORY <the URL of the Hot++ repository> GIT_TAG <a tag or a commit>)
FetchContent_MakeAvailable(hotpp)
```

Two variables are read when `add_subdirectory` runs. Set them before the call:

- `CMAKE_CXX_STANDARD`. See the section [The C++ standard and the format library](cmake-options.html#the-c-standard-and-the-format-library).
- `CMAKE_CXX_FLAGS`. The default backend takes its compile flags from this variable at this time (`HOT_CXXFLAGS_JOINED` in the root `CMakeLists.txt`). A later change has no effect on `make_default_backend`.

### Targets

| Target | Alias | What a host gets |
|---|---|---|
| `hot_runtime` | `hot::runtime` | The loader, the reload, the migration and `module_from_library`. It has no backend. It also has `hot::default_paths()`. |
| `hot_runtime_core` | `hot::runtime_core` | The globals and the registry that generated code needs. The other runtime targets link it. |
| `hot_backend` | `hot::backend` | `make_default_backend` and `backend_available`. It links `hot_runtime` and the backends that the build has. It defines `HOT_WITH_CLANG_FILES` and `HOT_WITH_JIT` for them. The target does not exist when both backends are off. |
| `hot_clang_backend` | `hot::clang_backend` | `ClangFilesBackend`. It runs an external `clang++`. The alias exists when `HOT_ENABLE_CLANG_FILES` is `ON`. (The library `hot_clang_backend` is always built, because `hotc` and `hot-ls` use it.) It links `hot_runtime` and `hotc_lib`. |
| `hot_jit_backend` | `hot::jit_backend` | `JitBackend`. It compiles in memory. It exists only when the JIT backend is on (see `HOT_ENABLE_JIT`). |
| `hot_aot` | `hot::aot` | The AOT registry and the core of the runtime. It has no loader. |
| `hotc` | `hot::hotc` | The tool. `hot_add_host` and `hot_add_scripts` run it at build time. |
| `hot_fmt` | none | An `INTERFACE` target for fmt. It exists only when the build uses fmt. |

Use the aliases in your project. The real names can change. `hot_add_host` links the right target for you, so you seldom link one by hand.

Do not link `hot::runtime` and `hot::aot` in one program. A program has one AOT library: it cannot link a plain AOT library and an optimized one (`OPTIMIZED`) together. The library from `hot_add_host` or `hot_add_scripts` with `AOT` links only `hot::runtime_core`. `hot_add_host` with `MODE AOT` adds `hot::aot`. `MODE AOT_RELOAD` adds `hot::backend` and never `hot::aot`.

### Export the host symbols

A host that loads generation modules must export its symbols. This applies to a program that links `hot::runtime`, `hot::backend` or one backend library. `hot_add_host` does this for the modes `HOT_RELOAD`, `AOT_RELOAD` and `PRECOMPILED`. The modules resolve the host functions and the runtime functions from the executable.

```cmake
set_target_properties(app PROPERTIES ENABLE_EXPORTS ON)
```

On Linux this adds `-rdynamic`. All samples that load a module set it. An AOT program does not load a module, so it does not need `ENABLE_EXPORTS`.

A static library of your own can hide a host function from the linker, because the linker drops an object that no code in the executable uses. A script can still call that function. Link such a library as a whole archive, as `samples/imgui/CMakeLists.txt` does:

```cmake
target_link_libraries(app PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,my_native_lib>")
```

`LINK_LIBRARY` needs CMake 3.24. The Hot++ libraries need no whole-archive link in the samples.

The generated code and the headers in `include/hot` need C++17 only.
