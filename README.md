# spp

Prototype of a hot-reload scripting runtime. This is step 3 (R0, R1 and F1).
The design is in `scriptlang-design/scriptlang-design.md`.

## Layout

- `engine/` has the executable `minengine`. It has `Actor`, `World` and the test driver.
- `runtime/` has the script runtime: class records, shells, dispatch, reflection and the loader.
- `poc/r0/gen1.cpp` is hand-written code. The translator `spc` makes code with the same structure.
- `translator/spc.cpp` is the translator `spc` (step F1). `scripts/` has the example scripts. `tools/build_gen.sh` translates and compiles one script. `tests/f1_tests.cpp` has the GoogleTest cases.
- `cmake/CPM.cmake` fetches a dependency when the system has no copy of it.
- `poc/r1/` has generation 2 (`gen2.cpp`: new `Tick`, `hp` renamed to `health`, new `shield`), the rejected generation 4 (`gen4_base.cpp`: other shell layout) and `gen3_bad.cpp` (does not compile; not in the build).

## Build

    cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
    ninja -C build

For an AddressSanitizer build, use `-DCMAKE_CXX_FLAGS=-fsanitize=address` and the directory `build-asan`.

## Run

    build/minengine --r0 build/gen1.so

The command loads `gen1.so` and makes one `Player`. It ticks the world 3 times and calls both `TakeDamage` overloads. It exits with 0 on success.

    build/minengine --r1 build/gen1.so build/gen2.so build/gen4_base.so build/gen3_missing.so

The R1 command spawns one `Player` and binds a delegate to `heal` through a thunk. It reloads gen 2 at the frame boundary and checks the address, the migrated state, the new `Tick` and the delegate. It then tries gen 4 and a missing library. Both must be rejected, and gen 2 must keep running.

`poc/r1/make_bad.sh [build-dir]` compiles `gen3_bad.cpp`, shows that the compile fails and runs R1 with the missing library.

## Translator

    build/spc scripts/player_v1.script --gen 1 -o out.cpp -I engine -I runtime --slots player.slots
    tools/build_gen.sh scripts/player_v1.script 1 out.so player.slots

`spc` reads the script and the imported headers. It asks libclang (C API) for the virtual methods of the native base class. It writes the shell (one stub for each virtual), the state struct, the reflection table, the generation functions, the thunks and the record. It writes `#line` directives, so a Clang error points to the script line. It reports a missing pure virtual override and an override of a `final` method.

The script subset: `import`, `class N : Base`, fields with `@was("old")`, `override`, `fn`, `let`, `const`, `this.`, `super.`, `null`, float literals, `Math.Min` as `Math::Min`, `if`, `while`, `for`, `return`. The body rewrite works on tokens. The translator does not parse expressions. A pointer needs `->` in a body, except a field or a parameter that has a pointer type.

The slot file (`--slots`) keeps the slot order of the `fn` methods stable between generations. A generation can add a `fn`, but it cannot remove one.

## Test

CMake uses CPM. It takes GoogleTest from the system, and fetches it only when it is missing. libclang must come from the system.


    ninja -C build && ctest --test-dir build

The cases `F1Reload` and `F1Diag` are GoogleTest cases in `f1_tests`. `F1Reload` repeats the R1 checks with the libraries that `spc` made from `scripts/player_v1.script` and `scripts/player_v2.script`. `F1Diag` compiles `scripts/bad.script`. The Clang error must name `bad.script` and the right line.

## Reload

`Loader::request_reload` validates a library with no side effect: `script_register`, base class, shell layout hash, slot count. `Loader::commit_pending` runs at the frame boundary (the `World` hook). It allocates all new state blocks, migrates, swaps the table and layout, sets `active_generation` and frees the old blocks.

## Not done yet

The `onReload` hook, the `convert` hook, generation unload, closures, string interpolation, script inheritance and constructors in scripts.
