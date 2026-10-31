# Hot++ samples

Each sample is a small project. It shows one way to host Hot++ in a program.
A sample builds in the Hot++ source tree. Most samples also build as a separate project.

| Sample | Situation | Key API | Link | State |
|---|---|---|---|---|
| [aot](aot/) | The C++ of the scripts is in the binary. No loader. No compiler at run time. | `hot::AotRegistry`, `hot_add_host(... MODE AOT ...)` | `hot_add_host` links the script library. It links `hot::aot`. | ready |
| [aot-reload](aot-reload/) | AOT scripts start in the binary. The program overrides one script from source at run time, then goes back to the AOT code. It needs a compiler for the override. | `hot::Loader::load_aot`, `hot::Backend::seed_slots`, `hot::Loader::revert_unit`, `hot_add_host(... MODE AOT_RELOAD ...)` | `hot_add_host` links the script library and `hot::backend` (not `hot::aot`). | ready |
| [runtime-compile](runtime-compile/) | The program compiles a script at run time and loads it. | `hot::make_default_backend`, `hot::DefaultBackendOptions`, `hot::Loader` | `hot_backend` | ready |
| [precompiled-library](precompiled-library/) | The build makes a script library. The program loads it. | `hot::module_from_library`, `hot::Loader`, `hot_add_host(... MODE PRECOMPILED ...)` | `hot::runtime` | ready |
| [hot-reload](hot-reload/) | The program reloads a script and keeps the live object. | `hot::Loader::request_reload`, `commit_pending`, `frame_boundary` | `hot_backend` | ready |
| [clang-files-backend](clang-files-backend/) | The backend runs an external `clang++` and loads the `.so`. | `hot::ClangFilesBackend`, `hot::ClangFilesOptions` | `hot_clang_backend` | ready |
| [jit-backend](jit-backend/) | The backend compiles in memory with the Clang library. It needs `HOT_BACKEND=JIT`. | `hot::JitBackend`, `hot::JitOptions` | `hot_jit_backend` | ready |
| [imgui](imgui/) | A Dear ImGui window whose user interface a script draws. It reloads the script. It needs SDL3 and CMake 3.24. It builds nothing without them. | `hot::ClangFilesBackend`, `hot::Loader::request_reload` | `hot_runtime`, `hot_clang_backend` | ready |

Copy your host from these samples. The folder `tests/engine` is a test host for the Hot++ tests. It is not a model for a user project.

## Build in the source tree

The option `HOT_BUILD_SAMPLES` is ON for the main project. The target names start with `sample_`.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_aot_hot sample_aot_cpp
ctest --preset linux-clang-files-dbg -R '^sample_aot_(hot|cpp)$'

The sample `aot-reload` has the targets `sample_aot_reload_hot` and `sample_aot_reload_cpp`.
```

## Build as a separate project

Each sample has a `CMakeLists.txt` that works in the Hot++ tree and as a separate project. The
README of each sample has the commands. Use `-DHOT_ROOT=/path/to/hot` to select the Hot++ tree.

The option `HOT_SAMPLES_STANDALONE_TEST` adds one test, `samples_standalone_aot`. The test
configures, builds and tests the sample `aot` in a new build directory under `samples_standalone/`.
The other samples have no such test.

## More information

- [Embedding](../doc/embedding.md): which situation fits, and the backend interface.
- [CMake](../doc/cmake.md): the options, `hot_add_host`, `hot_add_scripts` and `hot_script_host`.
- Each sample README has the sections "How it works", "Files", "Build and run" and "Adapt it to your project".
