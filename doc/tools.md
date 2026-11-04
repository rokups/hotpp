---
layout: default
title: Tools
nav_order: 42
---

`hot-ls` is the language server. The VS Code extension starts it. For the inner workings see [Language server internals](tools-ls.html).

## `hot-ls` features

- Diagnostics from the parser and from `clangd`, at the script position.
- The `hotc` errors of a `.cpp` script, as diagnostics (see [A `.cpp` script in the editor](#a-cpp-script-in-the-editor)).
- Completion for locals, fields, enums, classes, native members and keywords.
- Signature help, hover, go to definition and references.
- Rename of script symbols in all `.hot` files of the workspace.
- Document symbols, workspace symbols and semantic tokens.
- Inlay hints: the parameter names at the calls of script methods and constructors.
- Code lens: the number of references above each script class, method and constructor.
- Call hierarchy: the incoming and outgoing calls of a script method or constructor.
- Many workspace folders: the symbol index and the `clangd` cache cover all folders. A folder that the editor adds or removes (`workspace/didChangeWorkspaceFolders`) takes effect at once.
- Formatting (the whole file or a range), and quick fixes for a missing `;` and for a typo.

## Setup

`hot-ls` serves only `.hot` files and `.cpp` scripts. A C++ header (`.h`, `.hpp`) belongs to the C++ tools, for example `clangd`. `hot-ls` does not analyze a header, does not publish errors for it, and gives an empty answer to a request for it. F12 from a script can jump into a header: the C++ tools serve it there.

`hot-ls` serves all features for `.hot` files. For a `.cpp` script, `hot-ls` gives only the errors of `hotc`. The `clangd` extension serves all other features of a C++ file. `clangd` must be in `PATH`. CMake writes the script host files, so you do not pass `-I` flags.

```cmake
hot_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

Add `.cache/` to the ignore list of the project. Without a host, give the options in `hot.server.args`.

## Go to definition on a native symbol

In a `.hot` file, F12 on a symbol of a C++ header jumps to the header. This works for a class, a struct, a free function, a method, a field, an enum, an enum value, a type alias and a macro. It works also for a type in a base class list, after `new`, after `as` and `is`, in a template argument and in a pointer type. The editor opens the header at the declaration.

F12 needs a host. The host tells `hot-ls` the include directories of the header. Make the host with `hot_add_host` or `hot_script_host` (see [CMake](cmake-host.html) and [CMake](cmake-scripts.html#hot_script_host)), and run CMake again. The script must be in a script directory of the host.

When no host matches the script, `hot-ls` uses the nearest host in the workspace. The nearest host is the host whose script directory shares the most path parts with the script. If it finds no host, it uses the default include directories of the Hot++ build. The headers of your engine are then not found. When F12 finds nothing on a symbol, the editor shows this message. It shows it one time for each file, and again after the host set changes:

```text
hot-ls: No script host matches <script>, so F12 may not find native symbols. Add hot_add_host(...) or hot_script_host(...) in CMake and run CMake again.
```

If F12 does not jump, check these points:

- The script has a `using "header.h";` line for the header, and the header is in an include directory of the host.
- The host file is in `.cache/hot/hosts/` of the workspace, and its build directory still exists.
- The script has no error that stops the translation. Fix the errors in the Problems panel first.

For a `.cpp` script, the `clangd` extension gives F12. `hot-ls` gives no answer for it. F12 works when `clangd` reads the compile database of the project (`compile_commands.json`), and when the `clangd` flags of [clangd setup](#clangd-setup) are set.

## A `.cpp` script in the editor

A `.cpp` script is a C++ file (see [C++ scripts](cpp-scripts.html)). The editor shows the errors of `hotc` for it as diagnostics. The source of these diagnostics is `hot`.

- `hot-ls` treats a `.cpp` file as a script only when a host lists it. The host lists the file in `SCRIPTS`, or in a directory of `SCRIPT_DIRS`. See `hot_script_host` in [CMake](cmake-scripts.html#hot_script_host). Put no other `.cpp` file in a script directory: `hot-ls` treats it as a script too.
- `hot-ls` ignores any other `.cpp` file. It gives no diagnostic and no answer for it.
- The diagnostics come from the same checks as `hotc` does at build time. They appear when you open, edit or save the file.
- For a script, `hot-ls` gives no completion, hover, definition, references, rename, symbols or semantic tokens. The answer is empty. `clangd` owns these features. `hot-ls` does not add the script to the index of `.hot` symbols.
- The VS Code extension does not register `.cpp` as the language `hot`. It sends the `.cpp` documents to `hot-ls`. It keeps all features other than the diagnostics on the `hot` documents.

### `clangd` setup

`clangd` must see the aids of a script (`HOT_ALIAS`, `hot::OldState`). `hotc` injects the header `hot/script.h`, but `clangd` does not. Add the header and the include directory of Hot++ to the flags of `clangd`. A script without aids needs no setting.

Put this in a `.clangd` file at the root of the project. Replace `<hotpp>` with the path of the Hot++ directory.

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

## Format a script file

`tools/format-hot.sh [--check] files...` formats `.hot` files with `clang-format`. It reads the `CSharp` section of the `.clang-format` file in the Hot++ repository. `--check` reports a difference and changes no file. The script works from any directory. The first run changes K&R braces in old scripts. `hot-ls` formats natively with no `clang-format`: it keeps the braces where the author put them (see [Formatter rules](tools-ls.html#formatter-rules)).

## Language server limits

- The formatter of `hot-ls` works on tokens. It does not parse and it does not move braces or break lines.
- Diagnostics can be missing after an edit that gives the same C++ text.
- The index folder is the first workspace folder only.

## `hotc` format flag

`hotc --format=std|fmt` picks the format library for `${}` in the C++ text. The default is `std`. The flag is for build systems: `hot_add_scripts` sets it. `fmt` output also compiles as C++17. In build mode (without `--emit-cpp` and `--aot`), the build of `hotc` fixes the library, and a different `--format` is an error. See [CMake](cmake-options.html#the-format-library).

## VS Code

The extension is in `editors/vscode/`. It gives highlighting, the language server client and breakpoints in `.hot` files. For a `.cpp` script, the extension shows the errors of `hotc` as diagnostics. It does not register `.cpp` as the language `hot`. It gives no other `hot` feature on a `.cpp` file.

| Setting | Default |
|---|---|
| `hot.server.path` | `hot-ls` |
| `hot.server.args` | `["--stdio"]` |
| `hot.server.enable` | `true` |

The command `Hot++: Show Script Position` maps a place in a generated `.cpp` file to the script. The command `Hot++: Restart Language Server` reads the settings again.

## Other editors

Each editor below gets highlighting for `.hot` files and the language server `hot-ls --stdio`. `hot-ls` is the same server for all editors. By default, it shares the index of `clangd` with the C++ `clangd` of the project. Add `--private-clangd-dir` to the command to keep a private index. Add `--no-native-tokens` to remove the semantic tokens for C++ names. The test `LanguageServer.EditorKeywordListsMatchTheParser` checks that the keyword lists of all editor files equal the keywords of the parser.

### Vim and Neovim

The folder `editors/vim` has `ftdetect`, `ftplugin` and `syntax` files. Add the folder to the runtime path. Then give Neovim the server:

```lua
vim.lsp.config('hot_ls', {
  cmd = { 'hot-ls', '--stdio' },
  filetypes = { 'hot' },
  root_markers = { '.cache/hot', 'compile_commands.json', 'CMakeLists.txt', '.git' },
})
vim.lsp.enable('hot_ls')
```

The README in the folder has the setup for older Neovim (`nvim-lspconfig`) and for `vim-lsp`.

### Tree-sitter

The folder `editors/tree-sitter-hot` has a loose grammar for highlighting and `queries/highlights.scm`. The grammar does not check the language. Run `tree-sitter generate` in the folder to make the parser. The repository has no generated parser. The README in the folder shows the setup for `nvim-treesitter`.

### JetBrains IDEs

Add the folder `editors/vscode` in `Settings > Editor > TextMate Bundles` for the colors. Install the plugin LSP4IJ, and import the template `editors/jetbrains/lsp4ij` for the server. The setup is not tested with a real IDE. The README in `editors/jetbrains` has the steps.

## Debugger helpers

`tools/lldb` has formatters for state blocks and handles. `tools/natvis/hot.natvis` does the same for Visual Studio. Both are untested in a real debugger session.
