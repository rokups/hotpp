# Sample: precompiled-library

This sample shows a script library that the build makes. The program loads the library.
The program has no backend and no compiler at run time.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the S++ syntax.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md).

The build makes one library and one program for each variant:

- `sample_precompiled_spp_scripts` builds the library of the `.hot` script. `sample_precompiled_spp` loads it.
- `sample_precompiled_cpp_scripts` builds the library of the `.cpp` script. `sample_precompiled_cpp` loads it.

Each variant has its own `spp_add_scripts` call and its own `OUT_DIR`, so the two `wolf.g1.so` files do not clash.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `precompiled_library_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## How it works

1. `spp_add_scripts(TARGET sample_precompiled_spp_scripts SCRIPTS script/wolf.hot ...)` runs `sppc`.
   It makes the library `wolf.g1.so`. `FLAGS` pass the flags of the host build to the compile.
2. The variable `SPP_SCRIPTS_sample_precompiled_spp_scripts_LIBRARIES` has the paths of the
   libraries. The test gives the path to the program as `argv[1]`.
3. `hot::module_from_library(path)` opens the library. `hot::Loader::load` takes the module.
   `Loader::find("Wolf")` returns the class entry and `create()` makes an object.
4. The loader is declared first, so it is destroyed last. The object dies before it.

The program links `spp_runtime`. It does not link `spp_backend`. `ENABLE_EXPORTS` is ON.
The library resolves host and runtime symbols from the executable.
Each program depends on the library target of its variant, so the library
exists before the test runs.

`module_from_library` also makes a private copy of the library in the temp folder for the core
module (`spp-core-<pid>-<n>-<name>`). See "Core copies" in [Embedding](../../doc/embedding.md).

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`.
- `main.cpp`: the program. `argv[1]` is the path of the library. The program loads it and
  calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the S++ tree and as a separate project.

## Build and run

### In the S++ tree

The option `SPP_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_precompiled_spp sample_precompiled_cpp
ctest --preset linux-clang-files-dbg -R '^sample_precompiled_(spp|cpp)$'
```

Each test has the same name as its program. To run a program, give the path of its library:

```
cmake-build/linux-clang-files/bin/Debug/sample_precompiled_spp cmake-build/linux-clang-files/spp_scripts/sample_precompiled_spp/wolf.g1.so
cmake-build/linux-clang-files/bin/Debug/sample_precompiled_cpp cmake-build/linux-clang-files/spp_scripts/sample_precompiled_cpp/wolf.g1.so
```

### As a separate project

Use the S++ source tree in `SPP_ROOT`. The default value is the tree that holds this folder.
The build fetches the S++ dependencies with CPM. It needs `clang`, `libclang` and Ninja.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

To use another S++ tree, add `-DSPP_ROOT=/path/to/spp`. The `CMakeLists.txt` of the sample calls
`add_subdirectory` for `SPP_ROOT` only when the target `sppc` does not exist yet.

The test runs the program with the library path as `argv[1]`. To run the program yourself, give
the path of a `wolf.g1.so` file. It is under `spp_scripts/sample_precompiled_spp/` or
`spp_scripts/sample_precompiled_cpp/` of the build folder.

## Adapt it to your project

1. Call `spp_add_scripts(TARGET <name> SCRIPTS <your .hot files> INCLUDES <absolute folders> FLAGS <host flags> DEPENDS <your headers>)`.
   Get the host flags from `separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")`,
   as `CMakeLists.txt` does. A sanitizer or another runtime flag must be the same in the host and in the library.
2. Link your program to `spp_runtime`, set `ENABLE_EXPORTS ON` and add `add_dependencies(<program> <name>)`.
3. Take the library paths from `SPP_SCRIPTS_<name>_LIBRARIES`. Give them to the program in
   a way that suits you (an argument, a config file or a fixed folder).
4. Call `hot::module_from_library(path)`. Check the `LoadResult`. Give the module to `Loader::load`.
5. For a later generation, build another library with `SPPC_ARGS --gen 2` and give it to
   `request_reload`. See the sample [hot-reload](../hot-reload/) for the reload calls.

## Expected output

```
[wolf] wolf: bites=1 hp=99
[wolf] wolf: bites=2 hp=98
[wolf] wolf: bites=3 hp=97
```

Both variants print the same lines. The tests `sample_precompiled_spp` and `sample_precompiled_cpp` pass when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the function `spp_add_scripts`.
