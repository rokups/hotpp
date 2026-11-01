# Hot++

Hot++ is a scripting layer for C++ programs. You write a class in a script. The program compiles the script to native code and loads it. When you save the script, the running program reloads it. Live objects keep their state.

A script can derive from a native C++ class. It calls the C++ API directly. There are no bindings and no wrapper code.

## Use cases

- Rapid prototyping: change the code and see the result in the running program.
- Run-time code reload with state transfer: objects stay alive, and their field values move to the new code.
- Scripting without bindings: a script has full access to the C++ API of the host.
- Games: the target is a game or an engine with a frame loop and native classes.

## Example

A `.hot` script. `Wolf` derives from the host class `Creature` (see [samples/aot](samples/aot/)):

```hot
using "Creature.h";
using <string>;

class Wolf : Creature {
    int bites = 0;

    override void Tick(float dt) {
        bites = bites + 1;
        Hp = Hp - 1;
        std.string line = "${Name}: bites=${bites} hp=${Hp:03d}";
        std.printf("[wolf] %s\n", line.c_str());
    }
}
```

The host loads generation 1, makes an object and reloads (trimmed from [samples/hot-reload](samples/hot-reload/)):

```cpp
hot::Loader loader;
auto backend = hot::make_default_backend(options);      // JIT or clang files
loader.load(std::move(*backend->load({path, text_v1, {}}, 1).module));
std::unique_ptr<Creature> wolf(static_cast<Creature*>(loader.find("Wolf")->create().native));
wolf->Tick(1.0f / 60.0f);

loader.request_reload(backend->load({path, text_v2, {}}, 2));   // validate
loader.commit_pending();                                        // swap, at a safe point
wolf->Tick(1.0f / 60.0f);                                       // same pointer, new code
```

## How it works

1. Parse: `lexy` reads a `.hot` script. `libclang` reads a `.cpp` script.
2. Check: `libclang` reads the native headers. Hot++ finds the base classes, the virtual methods and the types.
3. Translate to C++: `hotc` writes a state struct, a shell class that derives from the native base, and a slot table.
4. Compile: Clang compiles the C++ in the process (JIT), through `clang++` (clang files), or ahead of time (AOT).
5. Load and swap: the loader moves the fields to the new generation and swaps the slot table. The shell keeps its address.

See [Introduction](https://rokups.github.io/hotpp/intro.html) for the generated code. What Hot++ uses:

- [lexy](https://github.com/foonathan/lexy): the parser of the `.hot` syntax.
- `libclang` (C API): reads the native headers and the `.cpp` scripts.
- Clang: compiles the generated C++, as a library (JIT) or as `clang++` (files).
- LLVM ORC (`LLJIT`): loads the compiled code in the process and unloads it.
- [fmt](https://github.com/fmtlib/fmt): string interpolation `${}` in C++17. C++20 can use `std::format`.
- [nlohmann_json](https://github.com/nlohmann/json) and `clangd`: the language server `hot-ls`.
- CMake with `CPM.cmake`: the build, and the fetch of missing packages. GoogleTest runs the tests.

## Three forms of a script

- `.hot`: a C#-like syntax with handles, properties, `foreach`, lambdas and string interpolation.
- `.cpp`: a plain C++ file. A normal build compiles it, and `hotc` translates it for reload.
- Native C++: the host code, and the reference for speed.

The numbers come from `bench_backends` (Release build, median of 3 runs). See [Benchmarks](https://rokups.github.io/hotpp/benchmarks.html).

- A loop inside a script runs at native speed (0.94x to 1.0x), for `.hot` and for `.cpp`.
- A call from the host into a script costs 2.2x to 2.4x of a native virtual call in `files` and `aot` (about 2 ns more). In `jit` it costs 2.9x.
- `new` and free of a `.hot` object cost 47 to 50 ns (4.4x to 4.6x of `std::make_shared`).

## Quick start

Hot++ needs CMake 3.20 and a Clang C++ compiler with `libclang`. The JIT also needs the LLVM and Clang development packages.

```cmake
set(CMAKE_CXX_STANDARD 17)
set(HOT_ROOT "/path/to/hotpp" CACHE PATH "The Hot++ source tree")
add_subdirectory(${HOT_ROOT} hot)
add_executable(myapp main.cpp)
hot_add_host(TARGET myapp MODE HOT_RELOAD SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
```

`MODE` is `AOT`, `AOT_RELOAD`, `HOT_RELOAD` or `PRECOMPILED`. See [CMake](https://rokups.github.io/hotpp/cmake.html). To build Hot++ and run its tests, use `cmake --preset linux-clang-files`, then `cmake --build --preset linux-clang-files-dbg` and `ctest --preset linux-clang-files-dbg`.

## Documentation

The documentation is at <https://rokups.github.io/hotpp/>: [Introduction](https://rokups.github.io/hotpp/intro.html), [Getting started](https://rokups.github.io/hotpp/getting-started.html), [Hot reload](https://rokups.github.io/hotpp/hot-reload.html), [the `.hot` language](https://rokups.github.io/hotpp/types.html), [C++ scripts](https://rokups.github.io/hotpp/cpp-scripts.html), [Embedding](https://rokups.github.io/hotpp/embedding.html), [Tools](https://rokups.github.io/hotpp/tools.html) and [Development notes](https://rokups.github.io/hotpp/development.html).

Each folder in [samples/](samples/) shows one way to host Hot++: [aot](samples/aot/), [aot-reload](samples/aot-reload/), [runtime-compile](samples/runtime-compile/), [precompiled-library](samples/precompiled-library/), [hot-reload](samples/hot-reload/), [clang-files-backend](samples/clang-files-backend/), [jit-backend](samples/jit-backend/) and [imgui](samples/imgui/).

## Status

- Linux: tested with Clang, also under ASan and TSan.
- Windows: a MinGW host loads and hot-reloads script modules. The tests pass under wine. Visual Studio is untested. See [Windows](https://rokups.github.io/hotpp/windows.html).
- macOS: untested.
- Coroutines: `async` methods and `await` work, and they need C++20. See [Async and await](https://rokups.github.io/hotpp/async.html).

## License

Hot++ uses the MIT license. See [`LICENSE`](LICENSE).
