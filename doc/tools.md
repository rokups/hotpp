---
layout: default
title: Tools
nav_order: 14
---

`spp-ls` is the language server. The VS Code extension starts it.

## spp-ls features

- Diagnostics from the parser and from clangd, at the script position.
- Completion for locals, fields, enums, classes, native members and keywords.
- Signature help, hover, go to definition and references.
- Rename of script symbols in all `.spp` files of the workspace.
- Document symbols, workspace symbols and semantic tokens.
- Formatting, and quick fixes for a missing `;` and for a typo.

## Setup

`spp-ls` serves only `.spp` files. Use the clangd extension for the C++ files. `clangd` must be in `PATH`. CMake writes the script host files, so you do not pass `-I` flags.

```cmake
spp_script_host(NAME game TARGET game_engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

Add `.cache/` to the ignore list of the project. Without a host, give the options in `spp.server.args`.

## sppc format flag

`sppc --format=std|fmt` picks the format library for `${}` in the C++ text. The default is `std`. The flag is for build systems: `spp_add_scripts` sets it. `fmt` output also compiles as C++17. In build mode (without `--emit-cpp` and `--aot`), the build of `sppc` fixes the library, and a different `--format` is an error. See doc/embedding.md.

## VS Code

The extension is in `editors/vscode/`. It gives highlighting, the language server client and breakpoints in `.spp` files.

| Setting | Default |
|---|---|
| `spp.server.path` | `spp-ls` |
| `spp.server.args` | `["--stdio"]` |
| `spp.server.enable` | `true` |

The command `S++: Show Script Position` maps a place in a generated `.cpp` file to the script. The command `S++: Restart Language Server` reads the settings again.

## Debugger helpers

`tools/lldb` has formatters for state blocks and handles. `tools/natvis/spp.natvis` does the same for Visual Studio. Both are untested in a real debugger session.
