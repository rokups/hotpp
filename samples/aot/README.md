# Sample: aot

This sample shows the AOT mode. The build translates the script to C++.
The C++ is part of the program. The program has no loader and no compiler at run time.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the S++ syntax. `Tick` uses string interpolation.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md). A `.cpp` script has no string interpolation, so `Tick` uses `snprintf`.

The build makes one AOT library and one program for each variant:

- `sample_aot_spp_scripts` and `sample_aot_spp` use the `.hot` script.
- `sample_aot_cpp_scripts` and `sample_aot_cpp` use the `.cpp` script.

Each variant has its own `spp_add_scripts` call and its own `OUT_DIR`, so the generated files
(`wolf.aot.cpp` and `spp_aot_registry.cpp`) do not clash.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `aot_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## How it works

1. `spp_add_scripts(TARGET sample_aot_spp_scripts AOT SCRIPTS script/wolf.hot ...)` runs `sppc --aot`
   at build time. It makes C++ files and a table `spp::aot_table`. It builds them as a static library.
2. The library links `spp_aot`. This library has `spp::AotRegistry` and does not have the loader.
3. `main.cpp` makes `spp::AotRegistry registry(spp::aot_table)`. The constructor registers
   the classes of all scripts.
4. `registry.create_as<Creature>("Wolf")` makes an object and returns the native pointer.
   The program owns the object and calls `Tick` three times.

The program does not call `dlopen`. It has no `ENABLE_EXPORTS`. A script change needs a new build.

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`. The `.hot` `Tick` uses string interpolation (`"${...}"`), so the build checks the format library and the C++ standard of the AOT library.
- `main.cpp`: the program. It makes a `Wolf` with `spp::AotRegistry` and calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the S++ tree and as a separate project.

## Build and run

### In the S++ tree

The option `SPP_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_aot_spp sample_aot_cpp
ctest --preset linux-clang-files-dbg -R '^sample_aot_(spp|cpp)$'
```

Each test has the same name as its target. To run a program, use the folder `bin/Debug` of the build tree:

```
cmake-build/linux-clang-files/bin/Debug/sample_aot_spp
cmake-build/linux-clang-files/bin/Debug/sample_aot_cpp
```
This sample has a test for the separate build too. Set `-DSPP_SAMPLES_STANDALONE_TEST=ON`
when you configure the S++ tree. The test is `samples_standalone_aot`. It builds both variants and runs both tests.

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

### With FetchContent

Replace the `SPP_ROOT` and `add_subdirectory` lines of `CMakeLists.txt` with this text.
The rest of the file stays the same.

```cmake
include(FetchContent)
FetchContent_Declare(spp GIT_REPOSITORY <the URL of the S++ repository> GIT_TAG <a tag or a commit>)
FetchContent_MakeAvailable(spp)
```

S++ is not the main project in this case. Its tests and samples are OFF.
The function `spp_add_scripts` is then available.

## Link

- `sample_aot_spp_scripts` and `sample_aot_cpp_scripts` are the libraries from `spp_add_scripts(... AOT ...)`.
  Each has the generated C++ and the table `spp::aot_table`. It links `spp_aot` (PUBLIC).
- Each program links only the library of its variant. It does not link `spp_runtime`.

## Adapt it to your project

1. Add S++ to your project with `add_subdirectory` or FetchContent (see above).
2. Call `spp_add_scripts(TARGET <name> AOT SCRIPTS <your .hot files> INCLUDES <absolute folders of your headers> DEPENDS <your headers>)`.
   The build does not track the headers by itself, so list them in `DEPENDS`.
3. Link your program to `<name>`.
4. Include `spp/aot.h`. Make one `spp::AotRegistry` on the main thread. It must live as long as the objects.
5. Make objects with `create_as<YourBase>("ClassName")`. Class names must be unique over all scripts.

An AOT build has no hot reload. If you need hot reload in a development build, use another sample
for that build. See the section "AOT" in [Embedding](../../doc/embedding.md).

## Expected output

```
[wolf] wolf: bites=1 hp=099
[wolf] wolf: bites=2 hp=098
[wolf] wolf: bites=3 hp=097
```

Both variants print the same lines. The tests `sample_aot_spp` and `sample_aot_cpp` pass when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the function `spp_add_scripts`.
