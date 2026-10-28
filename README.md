# spp

Prototype of a hot-reload scripting runtime. This is step 2 (R0 and R1).
The design is in `scriptlang-design/scriptlang-design.md`.

## Layout

- `engine/` has the executable `minengine`. It has `Actor`, `World` and the test driver.
- `runtime/` has the script runtime: class records, shells, dispatch, reflection and the loader.
- `poc/r0/gen1.cpp` is hand-written code. A translator will make this code later.
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

## Test

    ninja -C build && ctest --test-dir build

## Reload

`Loader::request_reload` validates a library with no side effect: `script_register`, base class, shell layout hash, slot count. `Loader::commit_pending` runs at the frame boundary (the `World` hook). It allocates all new state blocks, migrates, swaps the table and layout, sets `active_generation` and frees the old blocks.

## Not done yet

The `onReload` hook, the `convert` hook, generation unload, closures and the translator.
