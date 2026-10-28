# spp

Prototype of a hot-reload scripting runtime. This is step 3 (R0, R1 and F1) with the backend interface.
The design is in `scriptlang-design/scriptlang-design.md`.

## Layout

- `engine/` has the executable `minengine`. It has `Actor`, `World` and a small main program.
- `runtime/` has the script runtime: class records, shells, dispatch, reflection and the loader.
- `poc/r0/gen1.cpp` is hand-written code. The translator `spc` makes code with the same structure.
- `translator/spc_lib.cpp` is the translator library `spc_lib` (step F1). `translator/spc.cpp` is the thin file wrapper `spc`. `scripts/` has the example scripts.
- `runtime/backend.h`, `runtime/clang_files_backend.cpp` and `runtime/default_backend.cpp` are the script backends. `tools/sppc.cpp` is the tool `sppc`. `cmake/SppScripts.cmake` has `spp_add_scripts`.
- `tests/` has the GoogleTest cases (`r0_tests.cpp`, `r1_tests.cpp`, `backend_tests.cpp`), the F1 scenario `f1_scenario.h` and the shared header `test_support.h`.
- `cmake/CPM.cmake` fetches a dependency when the system has no copy of it.
- `poc/r1/` has generation 2 (`gen2.cpp`: new `Tick`, `hp` renamed to `health`, new `shield`), the rejected generation 4 (`gen4_base.cpp`: other shell layout) and `gen3_bad.cpp` (does not compile; not in the build).

## Build

    cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
    ninja -C build

For an AddressSanitizer build, use `-DCMAKE_CXX_FLAGS=-fsanitize=address` and the directory `build-asan`.

## Run

    build/minengine build/gen1.so

The command loads `gen1.so` and makes one `Player`. It ticks the world 3 times. It exits with 0 on success.

`poc/r1/make_bad.sh [build-dir]` compiles `gen3_bad.cpp` and shows that the compile fails. No library is made. The test `R1` uses the path of this missing library.

## Translator

    build/spc scripts/player_v1.script --gen 1 -o out.cpp -I engine -I runtime --slots player.slots

`spc` is a thin wrapper. It reads the files and calls `spc::translate` from `spc_lib` (`translator/spc_lib.h`). The function takes strings: script text, script name for `#line`, generation, slot file content, include dirs and resource dir. It returns strings: C++ text, new slot file content and diagnostics. It does not write a file. The translator reads the script and the imported headers. It asks libclang (C API) for the virtual methods of the native base class. It writes the shell (one stub for each virtual), the state struct, the reflection table, the generation functions, the thunks and the record. It writes `#line` directives, so a Clang error points to the script line. It reports a missing pure virtual override and an override of a `final` method.

The script subset: `import`, `class N : Base`, fields with `@was("old")`, `override`, `fn`, `let`, `const`, `this.`, `super.`, `null`, float literals, `Math.Min` as `Math::Min`, `if`, `while`, `for`, `return`. The body rewrite works on tokens. The translator does not parse expressions. A pointer needs `->` in a body, except a field or a parameter that has a pointer type.

The slot file (`--slots`) keeps the slot order of the `fn` methods stable between generations. A generation can add a `fn`, but it cannot remove one.

## Backends

A backend turns script source into a loadable `Module`. A `Module` has the `script_register` function and an owner of the code memory (for a `.so`: a `dlopen` handle). The `Loader` takes a `Module` in `load` and in `request_reload`. The class `Backend` (`runtime/backend.h`) has the method `load(source, generation)`. It returns a `Module` or an error text with the compiler diagnostics.

The CMake cache option `SPP_BACKEND` selects the backend at build time. `make_default_backend()` returns it.

- `CLANG_FILES` (default). `ClangFilesBackend` calls `spc::translate` in the process, writes the `.cpp` in a work directory, runs an external `clang++` to make a `.so`, and loads it. The options are the compiler command, the include dirs, the resource dir, the extra flags and the work directory.
- `JIT`. Not implemented yet. CMake stops with the error "JIT backend not implemented yet".

Example: `cmake -S . -B build -DSPP_BACKEND=CLANG_FILES`.

## Precompiled scripts

The tool `sppc` compiles scripts ahead of time. This is not a backend. Any build can load the result. Call `script::module_from_library(path)` and give the `Module` to the `Loader`. This call needs no backend object.

    sppc player_v1.script --gen 1 -I engine -I runtime --out-dir out/
    sppc player_v2.script --gen 2 -I engine -I runtime --out-dir out/

Command line:

    sppc <script>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]
         [--cxx command] [--cxxflag flag]... [--slots file]

- `<script>...` is one or more script files. Each script gives one library.
- `--gen N` is the generation number (1 or more).
- `--out-dir DIR` is the output directory. For a script `player_v1.script`, `sppc` writes `DIR/player_v1.g1.cpp` and `DIR/player_v1.g1.so`.
- `-I dir` is an include dir. The translator and the compiler use it.
- `--resource-dir dir` is the Clang resource dir. The default is the one that the build found.
- `--cxx command` is the compiler command. The default is `clang++`.
- `--cxxflag flag` adds one flag to the compile command. Repeat it for more flags.
- `--slots file` is the slot file. The default is `DIR/slots.txt`. It keeps the slot order of the `fn` methods between generations. Build the generations of one class in order.

The exit code is 0 for success, 1 for an error and 2 for a wrong command line. The tool prints the compiler errors at the script line. It does not need CMake.

The CMake function `spp_add_scripts` (`cmake/SppScripts.cmake`) only runs `sppc`:

    spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS scripts/player_v1.script
                    INCLUDES engine runtime FLAGS ${flags} DEPENDS ${headers})
    spp_add_scripts(TARGET pre_gen2 GEN 2 SCRIPTS scripts/player_v2.script AFTER pre_gen1 ...)

`TARGET` is a custom target that makes the libraries in `OUT_DIR` (default `${CMAKE_BINARY_DIR}/spp_scripts`). `AFTER` orders a generation after the earlier one.

## Test

CMake uses CPM. It takes GoogleTest from the system, and fetches it only when it is missing. libclang must come from the system.


    ninja -C build && ctest --test-dir build

All tests are GoogleTest cases. CMake registers them with `gtest_discover_tests`.

- `R0` (`tests/r0_tests.cpp`) loads `gen1.so` and makes one `Player`. It ticks the world 3 times and calls both `TakeDamage` overloads. It checks the type name, the position and the field `hp` by reflection.
- `R1` (`tests/r1_tests.cpp`) binds a delegate to `heal` through a thunk. It reloads gen 2 at the frame boundary and checks the address, the migrated state, the new `Tick` and the delegate. It then tries gen 4 and a missing library. Both must be rejected, and gen 2 must keep running.
- `BackendTests` (`tests/backend_tests.cpp`) use the backend that `SPP_BACKEND` selected. They run the F1 reload scenario (`tests/f1_scenario.h`): generation 1 then generation 2, the same address, `hp` becomes `health`, `shield` is 25 and a delegate calls the new `heal`. A bad script (`scripts/bad.script`) must give an error that names `bad.script` and the line. The runtime must keep running after the error.
- `PrecompiledTests` load the libraries that `spp_add_scripts` made with `sppc`, through `module_from_library`. They run the same F1 scenario. They run for each `SPP_BACKEND` value. One test runs `sppc` on `bad.script` and checks the exit code and the error line.

## Reload

`Loader::request_reload` validates a `Module` with no side effect: `script_register`, base class, shell layout hash, slot count. `Loader::commit_pending` runs at the frame boundary (the `World` hook). It allocates all new state blocks, migrates, swaps the table and layout, sets `active_generation` and frees the old blocks.

## Not done yet

The `onReload` hook, the `convert` hook, generation unload, closures, string interpolation, script inheritance and constructors in scripts.
