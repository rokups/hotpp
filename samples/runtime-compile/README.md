# Sample: runtime-compile

This sample shows a program that compiles a script at run time and loads it.
The program is a normal executable. It does not rebuild when the script changes.
The host needs a C++ compiler at run time.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the Hot++ syntax.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md).

The build makes one program for each variant from the same `main.cpp`:

- `sample_runtime_compile_hot` reads the `.hot` script.
- `sample_runtime_compile_cpp` reads the `.cpp` script.

Both variants are in the folder `script`. The file extension picks the variant.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `runtime_compile_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## How it works

1. `hot::make_default_backend(options)` makes the backend. `hot::DefaultBackendOptions`
   adds `include_dirs` (here the folder of `Creature.h`) and `extra_flags` to the build defaults.
2. `backend->load({path, text, {}}, 1)` translates and compiles the script. It returns a
   `hot::LoadResult`. On failure, `error` has the compiler messages.
3. `hot::Loader::load` takes the module. `Loader::find("Wolf")` returns the class entry.
   `create()` makes an object.
4. The program owns the object and calls `Tick`. The backend and the loader are declared first,
   so they are destroyed last. The object must die before the loader unloads the code.

`make_default_backend` uses the compiler and the flags of the build of Hot++. The backend is the clang files
backend (or the one that the environment variable `HOT_BACKEND` selects: `jit` or `files`). The compiler must stay available on the
machine that runs the program.

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`.
- `main.cpp`: the program. It reads the script, compiles it, loads it and calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the Hot++ tree and as a separate project.

## Build and run

### In the Hot++ tree

The option `HOT_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_runtime_compile_hot sample_runtime_compile_cpp
ctest --preset linux-clang-files-dbg -R '^sample_runtime_compile_(hot|cpp)$'
```

Each test has the same name as its target. To run a program, use the folder `bin/Debug` of the build tree:

```
cmake-build/linux-clang-files/bin/Debug/sample_runtime_compile_hot
cmake-build/linux-clang-files/bin/Debug/sample_runtime_compile_cpp
```

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

## Link

- `hot_add_host(... MODE HOT_RELOAD ...)` links `hot::backend`. It brings `hot::runtime` and the backend library.
- The call sets `ENABLE_EXPORTS` to ON. The generation modules resolve host and runtime symbols from the
  executable.
- The compiler, the resource directory and the work directory come from `hot::default_paths()`
  (`hot/paths.h`). The build of the sample is not portable, so they are the build values.
- `HOT_SCRIPT_DIR` is a compile definition from `hot_add_host`. It is the folder `script`.
  `argv[1]` replaces it. `HOT_SAMPLE_EXT` (`.hot` or `.cpp`) picks the variant. `argv[2]` replaces it.
- The folder of `Creature.h` is the parent of the script folder. The program gives it to the backend.

## Adapt it to your project

1. Make your program with `add_executable`. Call `hot_add_host(TARGET <program> MODE HOT_RELOAD SCRIPTS <your scripts> INCLUDES <your header folders>)`.
   It links `hot::backend` and sets `ENABLE_EXPORTS ON`.
2. Fill `hot::DefaultBackendOptions`. Put the folders of your headers in `include_dirs`.
   Put extra compiler flags in `extra_flags`. They come after the build defaults.
3. Read the script text yourself and give it to `backend->load({name, text, {}}, generation)`.
4. Check the `LoadResult` and the `ReloadResult` of `loader.load`. Show `error` to the user.
5. Declare the backend and the loader before your script objects. Delete the objects first.

For the JIT backend run the program with `HOT_BACKEND=jit` (the build needs `HOT_ENABLE_JIT`). The code does not change.
To set the compiler and the work folder yourself, see the samples
[clang-files-backend](../clang-files-backend/) and [jit-backend](../jit-backend/).
For reload, see [hot-reload](../hot-reload/).

## Expected output

```
[wolf] wolf: bites=1 hp=99
[wolf] wolf: bites=2 hp=98
[wolf] wolf: bites=3 hp=97
```

Each test (`sample_runtime_compile_hot` and `sample_runtime_compile_cpp`) passes when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the functions `hot_add_host` and `hot_add_scripts`.
