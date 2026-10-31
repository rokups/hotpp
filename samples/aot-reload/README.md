# Sample: aot-reload

This sample shows AOT scripts and run-time overrides in one program. The build compiles the
script `wolf` into the binary. The program also has a backend, so it can replace the script from
source while it runs, and go back to the compiled-in code.

JIT, Clang files and AOT can all be in one host. AOT is the start state. The program switches
each script at run time.

## What the program does

One live `Wolf` object goes through three phases. Each phase ticks the object two times.

1. **aot.** `Loader::load_aot(hot::aot_table)` registers the compiled-in script. The object runs the AOT code (`[aot]`).
2. **override.** The backend compiles `script/wolf_override` at run time. The program sets `Module::unit` to `wolf`, then calls `request_reload` and `commit_pending`. The object keeps `bites` and gets the field `howls` (`[override]`).
3. **revert.** `Loader::revert_unit("wolf")` and `commit_pending` give the AOT code back. The object keeps `bites` and drops `howls` (`[aot]`).

Before `load_aot`, the program calls `backend->seed_slots(hot::aot_table.slots)`. The override then
keeps the slot numbers of the AOT code.

## Files

- `CMakeLists.txt`: `hot_add_host(... MODE AOT_RELOAD ...)`. Two variants: `sample_aot_reload_hot` and `sample_aot_reload_cpp`.
- `main.cpp`: the program.
- `Creature.h`: the host class.
- `script/wolf.hot` and `script/wolf.cpp`: the AOT script. The build compiles it into the binary.
- `script/wolf_override.hot` and `script/wolf_override.cpp`: the override. The program compiles it at run time. It is not in `SCRIPTS`.

## Build and run

    cmake --build build --target sample_aot_reload_hot sample_aot_reload_cpp
    ctest --test-dir build -R '^sample_aot_reload_(hot|cpp)$'

The program takes the script folder from `argv[1]`, else from the define `HOT_SCRIPT_DIR`. The
optional `argv[2]` is the extension (`.hot` or `.cpp`).

## Limits

- The override needs a compiler at run time. It must use the same includes and flags as the AOT build.
- One `Loader` for each process.
- `MODE AOT_RELOAD` links `hot::backend`. Never link `hot::aot` in the same program.
