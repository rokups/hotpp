# S++ for VS Code

This extension gives support for S++ scripts (`.spp`, language id `spp`).

## Features

- Syntax highlighting with a TextMate grammar. It covers the keywords, the builtin types,
  the numeric suffixes, the attributes (`[native, handle("...")]`), the comments and the
  string interpolation `${expr}` and `${expr:spec}`.
- Brackets, comments and auto-close pairs (`language-configuration.json`).
- A thin client of the S++ language server `spp-ls`. It runs `spp-ls --stdio`.
  The server gives diagnostics, completion, hover, go-to-definition, references,
  document symbols, workspace symbols, semantic tokens and rename.
  Rename and references also cover the other `.spp` files of the workspace.
  See "Language server" in the top `README.md`.

## Commands

| Command | Meaning |
| --- | --- |
| `S++: Show Script Position` (`spp.showScriptPosition`) | Run it in a generated `.cpp` file (for example `player.g1.cpp`). It sends `spp/toScript` for the cursor and opens the script at that place. |

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `spp.server.path` | `spp-ls` | Path of the server binary. |
| `spp.server.args` | `["--stdio"]` | Arguments of the server. |
| `spp.server.enable` | `true` | Start the server. |

If the binary is missing, the extension shows one warning. Highlighting still works.

The build makes `build/spp-ls`. It is a proxy in front of a stock `clangd` (see `DECISIONS.md`, item 13).
`clangd` must be in `PATH`, or you give its path with `--clangd`.
Put the server arguments in `spp.server.args`. For example:

```json
"spp.server.args": ["--stdio", "-I", "/home/me/game/include",
                    "--compile-commands-dir", "/home/me/game/build/spp_cdb"]
```

| Argument | Meaning |
| --- | --- |
| `--clangd path` | The clangd binary. `""` turns clangd off: only the analyze features stay. |
| `-I dir` | An include dir for `using "x.h"` and for clangd. |
| `--resource-dir dir` | The Clang resource dir. |
| `--cxxflag flag` | A flag for the compile command of the generated C++. |
| `--compile-commands-dir dir` | The output dir of `sppc --compile-commands`. It starts the clangd background index. The server also reads the `-I`, `-isystem` and `-D` flags of the first entry. |
| `--clangd-log file` | The stderr of clangd, with `--log=info`. |

The client can also give `includeDirs`, `clangd` and `compileCommandsDir` in `initializationOptions`.
The extension expands `${workspaceFolder}` in `spp.server.path` and in `spp.server.args`.
It is the path of the first workspace folder. For example: `"-I", "${workspaceFolder}/include"`.

## Install for development

1. Run `npm install` in this folder.
2. Open this folder in VS Code and press F5.

You can also link the folder into `~/.vscode/extensions/`.
Without the server, set `files.associations` to map `*.spp` to `cpp`. The grammar does not apply then.

## Test

Run `node test/grammar.test.js`. CTest runs it as `VscodeGrammar` when `node` is found.

- The test always checks the JSON files and the keyword and builtin type coverage.
  It reads the builtin types from `src/translator/builtin_types.h`.
- `node test/expand.test.js` checks the expansion of `${workspaceFolder}` (CTest: `VscodeExpand`).
- After `npm install` it also tokenizes every `example/scripts/*.spp` file
  and compares `test/sample.spp` with `test/sample.snap`.
- Without `vscode-textmate` the tokenizer part is skipped and the test prints a notice.
- Run `node test/grammar.test.js --update` to accept a new snapshot.

The build and the test never use the network.
