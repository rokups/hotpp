# Sample: hot-reload

This sample shows a program that replaces the code of a live script object.
One process loads generation 1 of a script and makes an object. Then it loads generation 2.
The same object runs the new code and keeps its field values.
The host needs a C++ compiler at run time.

## How it works

1. The program loads `wolf_v1.spp` as generation 1 with `backend->load` and `loader.load`.
   It makes a `Wolf` and ticks twice.
2. `loader.request_reload(backend->load(source, 2))` validates generation 2. It has no effect on
   the live objects. On an error the old generation goes on. The result has a reason and an error text.
3. `loader.commit_pending()` makes generation 2 live. The host calls it at a safe point, when no
   script code runs.
4. `loader.frame_boundary()` unloads the retired generation 1 when the code is not in use.
   It waits at least one frame boundary after the commit.
5. The program ticks twice more. The same object runs the code of generation 2.

The generation number must grow with each load. A lower number gives the reason `GenerationNotNewer`.
The program owns the object and calls `Tick`. The backend and the loader are declared first, so they
are destroyed last. The object must die before the loader unloads the code.

See [../../doc/hot-reload.md](../../doc/hot-reload.md) for the changes that a reload accepts
and for the full reload.

## A real host

This sample reloads one time, in a straight line. A real host calls `request_reload` from a
file watcher when a script file changes. It calls `commit_pending` and `frame_boundary` one time
for each frame. `request_reload` can run on any thread. `commit_pending` and `frame_boundary`
run on the main thread.

## Files

- `Creature.h`: the host class. It is header-only.
- `wolf_v1.spp`: generation 1. The class `Wolf` prints the tag `[v1]`, `bites` and `hp`.
- `wolf_v2.spp`: generation 2. It adds the field `howls` and prints the tag `[v2]`, `hp` and `howls`.
- `main.cpp`: the program. It ticks twice, reloads and ticks twice again.
- `CMakeLists.txt`: the build. It works in the S++ tree and as a separate project.

## Build and run

### In the S++ tree

The option `SPP_BUILD_SAMPLES` is ON for the main project.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_hot_reload
ctest --preset linux-clang-files-dbg -R '^sample_hot_reload$'
```

The test has the same name as the target.

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

## Adapt it to your project

1. Link your program to `spp_backend` and set `ENABLE_EXPORTS ON`, as in the sample [runtime-compile](../runtime-compile/).
2. Load the first generation with `Loader::load`. Use the generation number 1.
3. When a script file changes, read it and call `request_reload(backend->load({name, text, {}}, next_generation))`.
   Increase `next_generation` each time. Do not send a reload while another is pending (the reason is `Busy`).
4. Call `commit_pending()` and then `frame_boundary()` at one safe point of each frame.
5. Look at `status` and `reason` of each `ReloadResult`. For `FullReloadRequired`, do a full reload
   with `Loader::reset` and `LoadMode::Fresh`. See "Full reload" in [Embedding](../../doc/embedding.md).

## Expected output

```
[v1] wolf: bites=1 hp=99
[v1] wolf: bites=2 hp=98
[v2] wolf: hp=97 howls=1
[v2] wolf: hp=96 howls=2
```

The `hp` value continues from generation 1. The test `sample_hot_reload` passes when the
output has the lines `[v1]` then the lines `[v2]`.

## More information

- [Embedding](../../doc/embedding.md): the six hosting situations and the backend interface.
- [CMake](../../doc/cmake.md): the options and the function `spp_add_scripts`.
