---
layout: default
title: Tools
nav_order: 15
---

`hot-ls` is the language server. The VS Code extension starts it.

## hot-ls features

- Diagnostics from the parser and from clangd, at the script position.
- The `hotc` errors of a `.cpp` script, as diagnostics (see [A `.cpp` script in the editor](#a-cpp-script-in-the-editor)).
- Completion for locals, fields, enums, classes, native members and keywords.
- Signature help, hover, go to definition and references.
- Rename of script symbols in all `.hot` files of the workspace.
- Document symbols, workspace symbols and semantic tokens.
- Formatting, and quick fixes for a missing `;` and for a typo.

## Setup

`hot-ls` serves all features for `.hot` files. For a `.cpp` script, `hot-ls` gives only the errors of `hotc`. The clangd extension serves all other features of a C++ file. `clangd` must be in `PATH`. CMake writes the script host files, so you do not pass `-I` flags.

```cmake
hot_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

Add `.cache/` to the ignore list of the project. Without a host, give the options in `spp.server.args`.

## A `.cpp` script in the editor

A `.cpp` script is a C++ file (see [C++ scripts](cpp-scripts.html)). The editor shows the errors of `hotc` for it as diagnostics. The source of these diagnostics is `hot`.

- `hot-ls` treats a `.cpp` file as a script only when a host lists it. The host lists the file in `SCRIPTS`, or in a directory of `SCRIPT_DIRS`. See `hot_script_host` in [CMake](cmake.html). Put no other `.cpp` file in a script directory: `hot-ls` treats it as a script too.
- `hot-ls` ignores any other `.cpp` file. It gives no diagnostic and no answer for it.
- The diagnostics come from the same checks as `hotc` does at build time. They appear when you open, edit or save the file.
- For a script, `hot-ls` gives no completion, hover, definition, references, rename, symbols or semantic tokens. The answer is empty. clangd owns these features. `hot-ls` does not add the script to the index of `.hot` symbols.
- The VS Code extension does not register `.cpp` as the language `hot`. It sends the `.cpp` documents to `hot-ls`. It keeps all features other than the diagnostics on the `hot` documents.

### clangd setup

clangd must see the aids of a script (`HOT_ALIAS`, `hot::OldState`). `hotc` injects the header `hot/script.h`, but clangd does not. Add the header and the include directory of S++ to the flags of clangd. A script without aids needs no setting.

Put this in a `.clangd` file at the root of the project. Replace `<hotpp>` with the path of the S++ directory.

```yaml
CompileFlags:
  Add: [-include, hot/script.h, -I<hotpp>/include]
```

Or add the same flags to the entry of the script in `compile_commands.json`:

```json
{
  "directory": "/project/build",
  "file": "/project/scripts/player.cpp",
  "arguments": ["c++", "-std=c++17", "-include", "hot/script.h", "-I<hotpp>/include", "-c", "/project/scripts/player.cpp"]
}
```

## hotc format flag

`hotc --format=std|fmt` picks the format library for `${}` in the C++ text. The default is `std`. The flag is for build systems: `hot_add_scripts` sets it. `fmt` output also compiles as C++17. In build mode (without `--emit-cpp` and `--aot`), the build of `hotc` fixes the library, and a different `--format` is an error. See [CMake](cmake.html).

## VS Code

The extension is in `editors/vscode/`. It gives highlighting, the language server client and breakpoints in `.hot` files. For a `.cpp` script, the extension shows the errors of `hotc` as diagnostics. It does not register `.cpp` as the language `hot`. It gives no other `spp` feature on a `.cpp` file.

| Setting | Default |
|---|---|
| `spp.server.path` | `hot-ls` |
| `spp.server.args` | `["--stdio"]` |
| `spp.server.enable` | `true` |

The command `S++: Show Script Position` maps a place in a generated `.cpp` file to the script. The command `S++: Restart Language Server` reads the settings again.

## Debugger helpers

`tools/lldb` has formatters for state blocks and handles. `tools/natvis/hot.natvis` does the same for Visual Studio. Both are untested in a real debugger session.
