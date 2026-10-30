---
layout: default
title: Tools
nav_order: 15
---

`spp-ls` is the language server. The VS Code extension starts it.

## spp-ls features

- Diagnostics from the parser and from clangd, at the script position.
- The `sppc` errors of a `.cpp` script, as diagnostics (see [A `.cpp` script in the editor](#a-cpp-script-in-the-editor)).
- Completion for locals, fields, enums, classes, native members and keywords.
- Signature help, hover, go to definition and references.
- Rename of script symbols in all `.hot` files of the workspace.
- Document symbols, workspace symbols and semantic tokens.
- Formatting, and quick fixes for a missing `;` and for a typo.

## Setup

`spp-ls` serves all features for `.hot` files. For a `.cpp` script, `spp-ls` gives only the errors of `sppc`. The clangd extension serves all other features of a C++ file. `clangd` must be in `PATH`. CMake writes the script host files, so you do not pass `-I` flags.

```cmake
spp_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

Add `.cache/` to the ignore list of the project. Without a host, give the options in `spp.server.args`.

## A `.cpp` script in the editor

A `.cpp` script is a C++ file (see [C++ scripts](cpp-scripts.html)). The editor shows the errors of `sppc` for it as diagnostics. The source of these diagnostics is `spp`.

- `spp-ls` treats a `.cpp` file as a script only when a host lists it. The host lists the file in `SCRIPTS`, or in a directory of `SCRIPT_DIRS`. See `spp_script_host` in [CMake](cmake.html). Put no other `.cpp` file in a script directory: `spp-ls` treats it as a script too.
- `spp-ls` ignores any other `.cpp` file. It gives no diagnostic and no answer for it.
- The diagnostics come from the same checks as `sppc` does at build time. They appear when you open, edit or save the file.
- For a script, `spp-ls` gives no completion, hover, definition, references, rename, symbols or semantic tokens. The answer is empty. clangd owns these features. `spp-ls` does not add the script to the index of `.hot` symbols.
- The VS Code extension does not register `.cpp` as the language `spp`. It sends the `.cpp` documents to `spp-ls`. It keeps all features other than the diagnostics on the `spp` documents.

### clangd setup

clangd must see the aids of a script (`SPP_ALIAS`, `spp::OldState`). `sppc` injects the header `spp/script.h`, but clangd does not. Add the header and the include directory of S++ to the flags of clangd. A script without aids needs no setting.

Put this in a `.clangd` file at the root of the project. Replace `<spp>` with the path of the S++ directory.

```yaml
CompileFlags:
  Add: [-include, spp/script.h, -I<spp>/include]
```

Or add the same flags to the entry of the script in `compile_commands.json`:

```json
{
  "directory": "/project/build",
  "file": "/project/scripts/player.cpp",
  "arguments": ["c++", "-std=c++17", "-include", "spp/script.h", "-I<spp>/include", "-c", "/project/scripts/player.cpp"]
}
```

## sppc format flag

`sppc --format=std|fmt` picks the format library for `${}` in the C++ text. The default is `std`. The flag is for build systems: `spp_add_scripts` sets it. `fmt` output also compiles as C++17. In build mode (without `--emit-cpp` and `--aot`), the build of `sppc` fixes the library, and a different `--format` is an error. See [CMake](cmake.html).

## VS Code

The extension is in `editors/vscode/`. It gives highlighting, the language server client and breakpoints in `.hot` files. For a `.cpp` script, the extension shows the errors of `sppc` as diagnostics. It does not register `.cpp` as the language `spp`. It gives no other `spp` feature on a `.cpp` file.

| Setting | Default |
|---|---|
| `spp.server.path` | `spp-ls` |
| `spp.server.args` | `["--stdio"]` |
| `spp.server.enable` | `true` |

The command `S++: Show Script Position` maps a place in a generated `.cpp` file to the script. The command `S++: Restart Language Server` reads the settings again.

## Debugger helpers

`tools/lldb` has formatters for state blocks and handles. `tools/natvis/spp.natvis` does the same for Visual Studio. Both are untested in a real debugger session.
