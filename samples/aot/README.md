# Sample: aot

This sample shows the AOT mode. The build translates the script to C++.
The C++ is part of the program. The program has no loader and no compiler at run time.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the Hot++ syntax. `Tick` uses string interpolation. The script imports `script/den.hot` (`using "den.hot";`), so the build has two units. `Wolf` makes a `Den`, and `Tick` calls the free function `Bite` of `den.hot`.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md). A `.cpp` script has no string interpolation, so `Tick` uses `snprintf`.

The build makes one AOT library and one program for each variant (and, see below, one more pair for each variant with `OPTIMIZED`):

- `sample_aot_hot_scripts` and `sample_aot_hot` use the `.hot` script.
- `sample_aot_cpp_scripts` and `sample_aot_cpp` use the `.cpp` script.

Each variant has its own `hot_add_host` call, so the generated files
(`wolf.aot.cpp` and `hot_aot_registry.cpp`) do not clash. Each call writes to its own directory.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `aot_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## Optimized AOT

The sample also builds each variant as optimized AOT code (`hot_add_host(... MODE AOT OPTIMIZED ...)`):

- `sample_aot_hot_opt` and `sample_aot_hot_opt_scripts` use the `.hot` script.
- `sample_aot_cpp_opt` and `sample_aot_cpp_opt_scripts` use the `.cpp` script.

The program source is the same. The output is the same. In optimized code, a call from the host into the script (`Tick`) goes to the script function directly, so it is as fast as a native virtual call. The code cannot be overridden or reloaded at run time. Use `OPTIMIZED` for a shipped build. See [Optimized AOT](../../doc/embedding-backends.md#optimized-aot).

## How it works

1. `hot_add_host(TARGET sample_aot_hot MODE AOT SCRIPTS script/wolf.hot ...)` runs `hotc --aot`
   at build time. It makes C++ files and a table `hot::aot_table`. It builds them as a static library.
2. `hot_add_host` links `hot::aot` to the library. `hot::aot` has `hot::AotRegistry` and does not have the loader.
3. `main.cpp` makes `hot::AotRegistry registry(hot::aot_table)`. The constructor registers
   the classes of all scripts.
4. `registry.create_as<Creature>("Wolf")` makes an object and returns the native pointer.
   The program owns the object and calls `Tick` three times.

The program does not call `dlopen`. It has no `ENABLE_EXPORTS`. A script change needs a new build.

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`. The `.hot` `Tick` uses string interpolation (`"${...}"`), so the build checks the format library and the C++ standard of the AOT library.
- `script/den.hot`: the second unit of the `.hot` variant. It has the free function `Bite`, the module variable `den_bites` and the class `Den`. The importer `wolf.hot` uses them.
- `main.cpp`: the program. It makes a `Wolf` with `hot::AotRegistry` and calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the Hot++ tree and as a separate project.

## Build and run

### In the Hot++ tree

The option `HOT_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_aot_hot sample_aot_cpp sample_aot_hot_opt sample_aot_cpp_opt
ctest --preset linux-clang-files-dbg -R '^sample_aot_(hot|cpp)(_opt)?$'
```

Each test has the same name as its target. To run a program, use the folder `bin/Debug` of the build tree:

```
cmake-build/linux-clang-files/bin/Debug/sample_aot_hot
cmake-build/linux-clang-files/bin/Debug/sample_aot_cpp
cmake-build/linux-clang-files/bin/Debug/sample_aot_hot_opt
cmake-build/linux-clang-files/bin/Debug/sample_aot_cpp_opt
```
This sample has a test for the separate build too. Set `-DHOT_SAMPLES_STANDALONE_TEST=ON`
when you configure the Hot++ tree. The test is `samples_standalone_aot`. It builds the four programs and runs the four tests.

### As a separate project

Use the Hot++ source tree in `HOT_ROOT`. The default value is the tree that holds this folder.
The build fetches the Hot++ dependencies with CPM. It needs `clang`, `libclang` and Ninja.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

To use another Hot++ tree, add `-DHOT_ROOT=/path/to/hot`. The `CMakeLists.txt` of the sample calls
`add_subdirectory` for `HOT_ROOT` only when the target `hotc` does not exist yet.

### With `FetchContent`

Replace the `HOT_ROOT` and `add_subdirectory` lines of `CMakeLists.txt` with this text.
The rest of the file stays the same.

```cmake
include(FetchContent)
FetchContent_Declare(hot GIT_REPOSITORY <the URL of the Hot++ repository> GIT_TAG <a tag or a commit>)
FetchContent_MakeAvailable(hot)
```

Hot++ is not the main project in this case. Its tests and samples are `OFF`.
The functions `hot_add_host` and `hot_add_scripts` are then available.

## Link

- `sample_aot_hot_scripts` and `sample_aot_cpp_scripts` are the libraries from `hot_add_host(... MODE AOT ...)`. The program links its library (PRIVATE).
  Each has the generated C++ and the table `hot::aot_table`. It links `hot::runtime_core` (`PUBLIC`). The program links `hot::aot`.
- Each program links only the library of its variant. It does not link `hot_runtime`.

## Adapt it to your project

1. Add Hot++ to your project with `add_subdirectory` or `FetchContent` (see above).
2. Make your program with `add_executable(<name> ...)`. Then call `hot_add_host(TARGET <name> MODE AOT SCRIPTS <your .hot files> INCLUDES <absolute folders of your headers> DEPENDS <your headers>)`.
   The build does not track the headers by itself, so list them in `DEPENDS`.
3. The call links the script library `<name>_scripts` to your program.
4. Include `hot/aot.h`. Make one `hot::AotRegistry` on the main thread. It must live as long as the objects.
5. Make objects with `create_as<YourBase>("ClassName")`. Class names must be unique over all scripts.

To build optimized code, add `OPTIMIZED` to the call: `hot_add_host(TARGET <name> MODE AOT OPTIMIZED SCRIPTS ...)`.

An AOT build has no hot reload. If you need hot reload in a development build, use another sample
for that build. See the section "AOT" in [Embedding](../../doc/embedding-backends.md#aot).

## Expected output

```
[wolf] wolf: bites=1 hp=099
[wolf] wolf: bites=2 hp=098
[wolf] wolf: bites=3 hp=097
```

All four programs print the same lines. The tests `sample_aot_hot`, `sample_aot_cpp`, `sample_aot_hot_opt` and `sample_aot_cpp_opt` pass when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the functions `hot_add_host` and `hot_add_scripts`.
