# Sample: jit-backend

This sample shows a program that builds `hot::JitBackend` itself.
The backend translates the script in memory, compiles it in memory with the Clang library
and loads the code with LLVM ORC. No process starts. The backend writes no file.

The sample needs the system LLVM and Clang development packages. It builds only when
the JIT backend is on (`HOT_ENABLE_JIT` is `ON`, or `AUTO` with the packages). Else the configure prints `sample_jit: skipped` and the
sample adds no target and no test.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script:

- `script/wolf.hot`: the script in the Hot++ syntax.
- `script/wolf.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md).

The build makes one program for each variant from the same `main.cpp`:

- `sample_jit_hot` reads the `.hot` script.
- `sample_jit_cpp` reads the `.cpp` script.

Both variants are in the folder `script`. The file extension picks the variant.
Both programs print the same output. The build also compiles the `.cpp` script with the normal
compiler (the object library `jit_backend_cpp_scripts_plain`). This shows that a `.cpp` script compiles unchanged.

## How it works

1. The program fills `hot::JitOptions` (see the table below).
2. `hot::JitBackend backend(options)` makes the backend. It is declared before the loader.
3. `backend.load(source, 1)` translates the script, compiles it in memory and loads the code with
   LLVM ORC. Each call makes a new JITDylib. It returns a `hot::LoadResult`.
4. `hot::Loader::load` takes the module. The program makes a `Wolf` and calls `Tick` three times.
5. The module keeps the JIT alive, so the code stays valid until the module goes.

## Files

- `Creature.h`: the host class. It is header-only.
- `script/wolf.hot` and `script/wolf.cpp`: the script in two variants. The class `Wolf` derives from `Creature` and overrides `Tick`.
- `main.cpp`: the program. It fills `hot::JitOptions`, compiles the script, loads it
  and calls `Tick` three times.
- `CMakeLists.txt`: the build. It works in the Hot++ tree and as a separate project.

## Options

| Field | Value in the sample |
|---|---|
| `compiler` | `hot::default_paths().compiler` |
| `include_dirs` | `hot::default_paths().include_dirs` and the folder of `Creature.h` (the parent of the script folder) |
| `resource_dir` | `hot::default_paths().resource_dir` |
| `extra_flags` | `hot::default_paths().cxxflags` (the flags of the host, for example a sanitizer) |

`hot::default_paths()` (`hot/paths.h`) takes the values from the build tree in a development build.
A portable build (`HOT_PORTABLE=ON`) finds them at run time (see [CMake](../../doc/cmake-sdk.md#portable-binaries)).
The script folder is `argv[1]`, else the definition `HOT_SCRIPT_DIR`. The definition
`HOT_SAMPLE_EXT` (`.hot` or `.cpp`) picks the variant. `argv[2]` replaces it.

The C++ standard (`std_version`) is empty, so the backend uses the standard of the host.
The other fields (`session`, `pch_path`, `strict_borrowed`, `raw_diagnostics`, `debug_flag`,
`debugger_support`) keep their defaults. See the table of the options in [Embedding](../../doc/embedding-interface.md#options-structs).

## Build and run

### In the Hot++ tree

Configure with the preset for the JIT backend. The option `HOT_BUILD_SAMPLES` is `ON` for the main project.

```
cmake --preset linux-clang-jit
cmake --build --preset linux-clang-jit-dbg --target sample_jit_hot sample_jit_cpp
ctest --preset linux-clang-jit-dbg -R '^sample_jit_(hot|cpp)$'
```

Each test has the same name as its target. To run a program, use the folder `bin/Debug` of the build tree:

```
cmake-build/linux-clang-jit/bin/Debug/sample_jit_hot
cmake-build/linux-clang-jit/bin/Debug/sample_jit_cpp
```

### As a separate project

Use the Hot++ source tree in `HOT_ROOT`. The default value is the tree that holds this folder.
The build fetches the Hot++ dependencies with CPM. It needs `clang`, `libclang`, the LLVM and
Clang development packages, and Ninja.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DHOT_ENABLE_JIT=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To use another Hot++ tree, add `-DHOT_ROOT=/path/to/hot`. The `CMakeLists.txt` of the sample sets
Hot++ builds the JIT backend when `HOT_ENABLE_JIT` is `AUTO` and CMake finds LLVM and Clang.

## Link

- The program links `hot::jit_backend`. It brings `hot::runtime`, `hotc_lib`, `clang-cpp` and `LLVM`.
- `ENABLE_EXPORTS` is `ON`. The JIT code resolves host and runtime symbols from the executable.

## Adapt it to your project

1. Configure Hot++ with `HOT_ENABLE_JIT=ON`. LLVM and Clang must be installed. CPM does not fetch them.
2. Link your program to `hot::jit_backend` and set `ENABLE_EXPORTS ON` (`hot_add_host` does this).
3. Fill `compiler` and `resource_dir` from `hot::default_paths()`. The backend uses the compiler path to find the toolchain.
4. Keep the compiler flags of the host, such as a sanitizer, in `extra_flags`.
5. To cut the translate time, share one `TranslatorSession` between loads. To cut the compile time,
   use `make_pch` and `pch_path`.

## Expected output

```
[wolf] wolf: bites=1 hp=99
[wolf] wolf: bites=2 hp=98
[wolf] wolf: bites=3 hp=97
```

Each test (`sample_jit_hot` and `sample_jit_cpp`) passes when the program returns 0.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the functions `hot_add_host` and `hot_add_scripts`.
