# Sample: imgui

This sample shows a Dear ImGui window whose user interface a script draws.
The program builds `hot::ClangFilesBackend` itself and reloads the script when the file changes.

The sample needs SDL3 and CMake 3.24. Without SDL3 the configure prints
`sample_imgui: skipped (SDL3 not found)` and the sample adds no target.

## Two script variants

The sample has one program (`main.cpp`) and two variants of the same script. Both are in the folder `script`:

- `script/demo.hot`: the script in the Hot++ syntax.
- `script/demo.cpp`: the script as plain C++. See [C++ scripts](../../doc/cpp-scripts.md).

The build makes one program for each variant from the same `main.cpp`:

- `sample_imgui_hot` reads `script/demo.hot`.
- `sample_imgui_cpp` reads `script/demo.cpp`.

The file extension picks the variant. Both programs show the same window. The build also compiles the
`.cpp` script with the normal compiler (the object library `imgui_cpp_scripts_plain`).
This shows that a `.cpp` script compiles unchanged.

The language server serves `.hot` scripts only. It does not serve `demo.cpp`.

## How it works

1. The script `script/demo.hot` has the class `Demo`. It derives from the host class `UiApp` and
   overrides `Frame`. It includes the real `imgui.h` (`using "imgui.h";`) and calls the ImGui API as it is.
2. A pointer parameter takes the address of a field, for example `ImGui.Checkbox("c", &on)`.
3. `main.cpp` fills `hot::ClangFilesOptions` and loads the script. The host calls `Frame` once for each frame.
4. The host checks the modification time of the script at each frame. When it changes, the host
   compiles a new generation and starts a reload. The commit happens at the frame boundary.
   The object keeps its field values.
5. If the compile or the reload fails, the window "Script error" shows the text. The old generation keeps running.
6. CPM fetches Dear ImGui. The static library `imgui_sdl3` has the SDL3 and SDL_Renderer backends.
   The program links it as a whole archive, so a script can call every ImGui function.

## Files

- `UiApp.h`, `UiApp.cpp`: the host base class. It has the virtual function `Frame`.
- `script/demo.hot` and `script/demo.cpp`: the script in two variants. The class `Demo` derives from `UiApp`.
- `main.cpp`: the program. It makes the window, the backend and the loader. It reloads the script.
- `CMakeLists.txt`: the build. It works in the Hot++ tree and as a separate project.

## Build and run

The program opens a window, so the sample has no test.

### In the Hot++ tree

```
cmake --preset linux-clang-files
cmake --build --preset linux-clang-files-dbg --target sample_imgui_hot sample_imgui_cpp
cmake-build/linux-clang-files/bin/Debug/sample_imgui_hot [script]
cmake-build/linux-clang-files/bin/Debug/sample_imgui_cpp [script]
```

The default script comes from the definition `HOT_SCRIPT_DIR` (a development build only). In a portable build, give the script as `argv[1]`.
The default script of `sample_imgui_hot` is `script/demo.hot`.
The default script of `sample_imgui_cpp` is `script/demo.cpp`.

### As a separate project

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target sample_imgui_hot sample_imgui_cpp
```

To use another Hot++ tree, add `-DHOT_ROOT=/path/to/hot`.

## Adapt it to your project

1. Link your program to `hot::runtime` and `hot::clang_backend`. Set `ENABLE_EXPORTS ON`.
2. Link a library that the scripts call (here `imgui_sdl3`) as a whole archive.
   The linker then keeps all objects.
3. Fill the options from `hot::default_paths()` (`hot/paths.h`): it gives the compiler, the resource folder,
   the work folder and the include folder of Hot++. Add the folder of the host headers and the folder of
   the library headers to `include_dirs`. In a portable build (`HOT_PORTABLE=ON`) this sample reads the
   ImGui include folder from the environment variable `HOT_IMGUI_INCLUDE_DIR`.
4. Call `hot_script_host` so that the language server knows the script folder.
5. Gap: a native array field such as `float rgb[3];` gives "expected a member name". Use `ImVec4 col`
   and `&col.x`, or three float fields.

## More information

- [Hot reload](../../doc/hot-reload.md): the reload steps and the frame boundary.
- [Embedding](../../doc/embedding.md): the hosting situations and the backend interface.
