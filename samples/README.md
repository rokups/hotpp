# S++ samples

Each sample is a small project. It shows one way to host S++ in a program.
A sample builds in the S++ source tree. Most samples also build as a separate project.

| Sample | Situation | Key API | Link | State |
|---|---|---|---|---|
| [aot](aot/) | The C++ of the scripts is in the binary. No loader. No compiler at run time. | `spp::AotRegistry`, `spp_add_scripts(... AOT ...)` | the library of `spp_add_scripts`. It links `spp_aot`. | ready |
| [runtime-compile](runtime-compile/) | The program compiles a script at run time and loads it. | `spp::make_default_backend`, `spp::DefaultBackendOptions`, `spp::Loader` | `spp_backend` | ready |
| [precompiled-library](precompiled-library/) | The build makes a script library. The program loads it. | `spp::module_from_library`, `spp::Loader`, `spp_add_scripts` | `spp_runtime` | ready |
| [hot-reload](hot-reload/) | The program reloads a script and keeps the live object. | `spp::Loader::request_reload`, `commit_pending`, `frame_boundary` | `spp_backend` | ready |
| [clang-files-backend](clang-files-backend/) | The backend runs an external `clang++` and loads the `.so`. | `spp::ClangFilesBackend`, `spp::ClangFilesOptions` | `spp_clang_backend` | ready |
| [jit-backend](jit-backend/) | The backend compiles in memory with the Clang library. It needs `SPP_BACKEND=JIT`. | `spp::JitBackend`, `spp::JitOptions` | `spp_jit_backend` | ready |
| [imgui](imgui/) | A Dear ImGui window whose user interface a script draws. It reloads the script. It needs SDL3 and CMake 3.24. It builds nothing without them. | `spp::ClangFilesBackend`, `spp::Loader::request_reload` | `spp_runtime`, `spp_clang_backend` | ready |

Copy your host from these samples. The folder `tests/engine` is a test host for the S++ tests. It is not a model for a user project.

## Build in the source tree

The option `SPP_BUILD_SAMPLES` is ON for the main project. The target names start with `sample_`.

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_aot
ctest --preset linux-clang-files-dbg -R '^sample_aot$'
```

## Build as a separate project

Each sample has a `CMakeLists.txt` that works in the S++ tree and as a separate project. The
README of each sample has the commands. Use `-DSPP_ROOT=/path/to/spp` to select the S++ tree.

The option `SPP_SAMPLES_STANDALONE_TEST` adds one test, `samples_standalone_aot`. The test
configures, builds and tests the sample `aot` in a new build directory under `samples_standalone/`.
The other samples have no such test.

## More information

- [Embedding](../doc/embedding.md): which situation fits, and the backend interface.
- [CMake](../doc/cmake.md): the options, `spp_add_scripts` and `spp_script_host`.
- Each sample README has the sections "How it works", "Files", "Build and run" and "Adapt it to your project".
