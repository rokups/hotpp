# Sample: runtime-compile

This sample shows a program that compiles a script at run time and loads it.
The program is a normal executable. It does not rebuild when the script changes.
The host needs a C++ compiler at run time.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the S++ syntax.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md).

The build makes one program for each variant from the same `main.cpp`:

- `sample_runtime_compile_spp` reads the `.hot` script.
- `sample_runtime_compile_cpp` reads the `.cpp` script.

Both variants are in the folder `script`. The file extension picks the variant.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `runtime_compile_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## How it works

1. `spp::make_default_backend(options)` makes the backend. `spp::DefaultBackendOptions`
   adds `include_dirs` (here the folder of `Creature.h`) and `extra_flags` to the build defaults.
2. `backend->load({path, text, {}}, 1)` translates and compiles the script. It returns a
   `spp::LoadResult`. On failure, `error` has the compiler messages.
3. `spp::Loader::load` takes the module. `Loader::find("Wolf")` returns the class entry.
   `create()` makes an object.
4. The program owns the object and calls `Tick`. The backend and the loader are declared first,
   so they are destroyed last. The object must die before the loader unloads the code.

`make_default_backend` uses the compiler and the flags of the build of S++. The backend is the one
that `SPP_BACKEND` selects (default `CLANG_FILES`). The compiler must stay available on the
machine that runs the program.

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`.
- `main.cpp`: the program. It reads the script, compiles it, loads it and calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the S++ tree and as a separate project.

## Build and run

### In the S++ tree

The option `SPP_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_runtime_compile_spp sample_runtime_compile_cpp
ctest --preset linux-clang-files-dbg -R '^sample_runtime_compile_(spp|cpp)$'
```

Each test has the same name as its target. To run a program, use the folder `bin/Debug` of the build tree:

```
cmake-build/linux-clang-files/bin/Debug/sample_runtime_compile_spp
cmake-build/linux-clang-files/bin/Debug/sample_runtime_compile_cpp
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

## Link

- The program links `spp_backend`. It brings `spp_runtime` and the backend library.
- `ENABLE_EXPORTS` is ON. The generation modules resolve host and runtime symbols from the
  executable.
- `SAMPLE_DIR` is a compile definition. It is the folder of `Creature.h`.
- `SAMPLE_SCRIPT_DIR` and `SAMPLE_SCRIPT_EXT` are compile definitions. `SAMPLE_SCRIPT_DIR` is the folder `script`. `SAMPLE_SCRIPT_EXT` (`.hot` or `.cpp`) picks the variant.

## Adapt it to your project

1. Link your program to `spp_backend` and set `ENABLE_EXPORTS ON` on it.
2. Fill `spp::DefaultBackendOptions`. Put the folders of your headers in `include_dirs`.
   Put extra compiler flags in `extra_flags`. They come after the build defaults.
3. Read the script text yourself and give it to `backend->load({name, text, {}}, generation)`.
4. Check the `LoadResult` and the `ReloadResult` of `loader.load`. Show `error` to the user.
5. Declare the backend and the loader before your script objects. Delete the objects first.

For a different `SPP_BACKEND` configure with `-DSPP_BACKEND=JIT`. The code does not change.
To set the compiler and the work folder yourself, see the samples
[clang-files-backend](../clang-files-backend/) and [jit-backend](../jit-backend/).
For reload, see [hot-reload](../hot-reload/).

## Expected output

```
[wolf] wolf: bites=1 hp=99
[wolf] wolf: bites=2 hp=98
[wolf] wolf: bites=3 hp=97
```

Each test (`sample_runtime_compile_spp` and `sample_runtime_compile_cpp`) passes when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the function `spp_add_scripts`.
