# Sample: clang-files-backend

This sample shows a program that builds `hot::ClangFilesBackend` itself.
The backend translates the script in memory, runs an external `clang++` and loads the `.so`.
The host needs a C++ compiler at run time.

`HOT_BACKEND=CLANG_FILES` selects this backend for `make_default_backend`. A program that
builds it itself sets the compiler, the flags and the work folder.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the S++ syntax.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md).

The build makes one program for each variant from the same `main.cpp`:

- `sample_clang_files_hot` reads the `.hot` script.
- `sample_clang_files_cpp` reads the `.cpp` script.

Both variants are in the folder `script`. The file extension picks the variant.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `clang_files_backend_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## How it works

1. The program fills `hot::ClangFilesOptions` (see the table below).
2. `hot::ClangFilesBackend backend(options)` makes the backend. It is declared before the loader.
3. `backend.load(source, 1)` translates the script, writes a `.cpp` in `work_dir`, runs the compiler
   to make a `.so`, and loads it. It returns a `hot::LoadResult`.
4. `hot::Loader::load` takes the module. The program makes a `Wolf` and calls `Tick` three times.
5. At exit, `remove_work_dir_at_exit` removes the work folder.

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`.
- `main.cpp`: the program. It fills `hot::ClangFilesOptions`, compiles the script, loads it
  and calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the S++ tree and as a separate project.

## Options

| Field | Value in the sample |
|---|---|
| `compiler` | `CMAKE_CXX_COMPILER` |
| `include_dirs` | the include dir of S++ and the folder of the sample |
| `extra_flags` | `CMAKE_CXX_FLAGS` (the flags of the host, for example a sanitizer) |
| `resource_dir` | the output of `<compiler> -print-resource-dir` |
| `work_dir` | `hot_work_hot` or `hot_work_cpp` in the build folder (one for each program) |
| `remove_work_dir_at_exit` | `true` |

The sample does not use `HOT_DEFAULT_CXXFLAGS`. The compile works with the flags above.
The C++ standard is empty in the options, so the backend uses the standard of the host.
The other fields (`slots_file`, `session`, `strict_borrowed`, `raw_diagnostics`) keep their defaults.
See the table of the options in [Embedding](../../doc/embedding.md).

## Build and run

### In the S++ tree

The option `HOT_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_clang_files_hot sample_clang_files_cpp
ctest --preset linux-clang-files-dbg -R '^sample_clang_files_(hot|cpp)$'
```

Each test has the same name as its target. To run a program, use the folder `bin/Debug` of the build tree:

```
cmake-build/linux-clang-files/bin/Debug/sample_clang_files_hot
cmake-build/linux-clang-files/bin/Debug/sample_clang_files_cpp
```

### As a separate project

Use the S++ source tree in `HOT_ROOT`. The default value is the tree that holds this folder.
The build fetches the S++ dependencies with CPM. It needs `clang`, `libclang` and Ninja.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

To use another S++ tree, add `-DHOT_ROOT=/path/to/spp`. The `CMakeLists.txt` of the sample calls
`add_subdirectory` for `HOT_ROOT` only when the target `sppc` does not exist yet.

## Link

- The program links `hot_clang_backend`. It brings `hot_runtime` and `sppc_lib`.
- `ENABLE_EXPORTS` is ON. The generation modules resolve host and runtime symbols from the
  executable.

## Adapt it to your project

1. Link your program to `hot_clang_backend` and set `ENABLE_EXPORTS ON`.
2. Pass values from CMake as compile definitions, as `CMakeLists.txt` does: the compiler, the flags,
   the include folders of S++ (`HOT_INCLUDE_DIR`) and of your headers, and the resource folder.
3. Use a work folder that only this backend uses when you set `remove_work_dir_at_exit` to `true`.
4. Keep the compiler flags of the host, such as a sanitizer, in `extra_flags`.
5. The compiler must exist on the machine that runs the program.

## Expected output

```
[wolf] wolf: bites=1 hp=99
[wolf] wolf: bites=2 hp=98
[wolf] wolf: bites=3 hp=97
```

Each test (`sample_clang_files_hot` and `sample_clang_files_cpp`) passes when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the function `hot_add_scripts`.
