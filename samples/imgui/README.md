# Sample: imgui

This sample shows a Dear ImGui window whose user interface a script draws.
The program builds `spp::ClangFilesBackend` itself and reloads the script when the file changes.

The sample needs SDL3 and CMake 3.24. Without SDL3 the configure prints
`sample_imgui: skipped (SDL3 not found)` and the sample adds no target.

## How it works

1. The script `demo.spp` has the class `Demo`. It derives from the host class `UiApp` and
   overrides `Frame`. It includes the real `imgui.h` (`using "imgui.h";`) and calls the ImGui API as it is.
2. A pointer parameter takes the address of a field, for example `ImGui.Checkbox("c", &this.on)`.
3. `main.cpp` fills `spp::ClangFilesOptions` and loads the script. The host calls `Frame` once for each frame.
4. The host checks the modification time of the script at each frame. When it changes, the host
   compiles a new generation and starts a reload. The commit happens at the frame boundary.
   The object keeps its field values.
5. If the compile or the reload fails, the window "Script error" shows the text. The old generation keeps running.
6. CPM fetches Dear ImGui. The static library `imgui_sdl3` has the SDL3 and SDL_Renderer backends.
   The program links it as a whole archive, so a script can call every ImGui function.

## Files

- `UiApp.h`, `UiApp.cpp`: the host base class. It has the virtual function `Frame`.
- `demo.spp`: the script. The class `Demo` derives from `UiApp`.
- `main.cpp`: the program. It makes the window, the backend and the loader. It reloads the script.
- `CMakeLists.txt`: the build. It works in the S++ tree and as a separate project.

## Build and run

The program opens a window, so the sample has no test.

### In the S++ tree

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_imgui
cmake-build/linux-clang-files/bin/Debug/sample_imgui [script]
```

The default script is `demo.spp`.

### As a separate project

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target sample_imgui
```

To use another S++ tree, add `-DSPP_ROOT=/path/to/spp`.

## Adapt it to your project

1. Link your program to `spp_runtime` and `spp_clang_backend`. Set `ENABLE_EXPORTS ON`.
2. Link a library that the scripts call (here `imgui_sdl3`) as a whole archive.
   The linker then keeps all objects.
3. Put the include folders of the script in `include_dirs`: the include folder of S++,
   the folder of the host headers and the folder of the library headers.
4. Call `spp_script_host` so that the language server knows the script folder.
5. Gap: a native array field such as `float rgb[3];` gives "expected a member name". Use `ImVec4 col`
   and `&this.col.x`, or three float fields.

## More information

- [Hot reload](../../doc/hot-reload.md): the reload steps and the frame boundary.
- [Embedding](../../doc/embedding.md): the hosting situations and the backend interface.
