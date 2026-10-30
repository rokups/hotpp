# Hot++ for VS Code

This extension gives support for Hot++ scripts (`.hot`, language id `hot`).

## Features

- Syntax highlighting with a TextMate grammar. It covers the keywords, the builtin types,
  the numeric suffixes, the attributes (`[native, handle("...")]`), the comments and the
  string interpolation `${expr}` and `${expr:spec}`.
- Brackets, comments and auto-close pairs (`language-configuration.json`).
- A thin client of the Hot++ language server `hot-ls`. It runs `hot-ls --stdio`.
  The server gives diagnostics, completion, hover, go-to-definition, references,
  document symbols, workspace symbols, semantic tokens and rename.
  Rename and references also cover the other `.hot` files of the workspace.
  See "Language server" in the top `README.md`.

## Commands

| Command | Meaning |
| --- | --- |
| `Hot++: Show Script Position` (`hot.showScriptPosition`) | Run it in a generated `.cpp` file (for example `player.g1.cpp`). It sends `hot/toScript` for the cursor and opens the script at that place. |
| `Hot++: Restart Language Server` (`hot.restartServer`) | Stops `hot-ls` and starts it again. It reads the settings again. Use it after you change `hot.server.path` or `hot.server.args`, or after you rebuild `hot-ls`. |

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `hot.server.path` | `hot-ls` | Path of the server binary. |
| `hot.server.args` | `["--stdio"]` | Arguments of the server. |
| `hot.server.enable` | `true` | Start the server. |

If the binary is missing, the extension shows one warning. Highlighting still works.

The build makes `build/hot-ls`. It uses a stock `clangd` as a child process (see `DECISIONS.md`, items 13 and 18).
`clangd` must be in `PATH`, or you give its path with `--clangd`.
Put the server arguments in `hot.server.args`. For example:

```json
"hot.server.args": ["--stdio", "-I", "/home/me/game/include",
                    "--compile-commands-dir", "/home/me/game/build/hot_cdb"]
```

| Argument | Meaning |
| --- | --- |
| `--clangd path` | The clangd binary. `""` turns clangd off: only the analyze features stay. |
| `-I dir` | An include dir for `using "x.h"` and for clangd. |
| `--resource-dir dir` | The Clang resource dir. |
| `--cxxflag flag` | A flag for the compile command of the generated C++. |
| `--compile-commands-dir dir` | The output dir of `hotc --compile-commands`. It starts the clangd background index. The server also reads the `-I`, `-isystem` and `-D` flags of the first entry. |
| `--clangd-log file` | The stderr of clangd, with `--log=info`. |

The client can also give `includeDirs`, `clangd` and `compileCommandsDir` in `initializationOptions`.
The extension expands `${workspaceFolder}` in `hot.server.path` and in `hot.server.args`.
It is the path of the first workspace folder. For example: `"-I", "${workspaceFolder}/include"`.

## Use with the clangd extension

This extension and `hot-ls` serve all features for `.hot` files. For a `.cpp` script that a host lists, they show only the errors of `hotc` as diagnostics. The official clangd extension serves the C++ files, with all other features.
Install both extensions. They run side by side on one project.

1. Install the clangd extension and `clangd`. Set it up for the C++ build as usual.
2. Install this extension and set `hot.server.path` and `hot.server.args`.
3. Optional: run `hotc <file.hot>... --gen N --out-dir DIR --compile-commands CDIR`.
   Add `"--compile-commands-dir", "CDIR"` to `hot.server.args`.
4. Add `.cache/` to the ignore list of the project.

The two servers can share the `.cache/` directory. The index of the clangd extension is in
`.cache/clangd/index/` next to its `compile_commands.json`. The clangd child of `hot-ls` has its own
index in `.cache/hot/clangd/.cache/clangd/index/`. The index of `hot-ls` is in `.cache/hot/index/`.
See "Language server" in the top `README.md`.

## Host files and file watchers

The CMake function `hot_script_host` writes the host files to `.cache/hot/hosts/*.json`, and the side files `*.defines`, `*.options` and `*.includes`. They tell
`hot-ls` the include directories and flags of each script. The extension watches `**/*.hot` and
`.cache/hot/hosts/*` and sends the events to the server. A new, changed or deleted script, or a
new or changed host file, takes effect without a restart of the server.

Every valid include path works: spaces, quotes, backslashes, non-ASCII characters, `$`, `#`, brackets, relative paths and paths from imported targets. Two cases do not work. A path with a newline cannot be in a side file. A path with a semicolon is a CMake list separator, so CMake splits it before `hot_script_host` sees it.

## Install for development

1. Run `npm install` in this folder.
2. Open this folder in VS Code and press F5.

You can also link the folder into `~/.vscode/extensions/`.
Without the server, set `files.associations` to map `*.hot` to `cpp`. The grammar does not apply then.

## Test

Run `node test/grammar.test.js`. CTest runs it as `VscodeGrammar` when `node` is found.

- The test always checks the JSON files and the keyword and builtin type coverage.
  It reads the builtin types from `src/translator/builtin_types.h`.
- `node test/expand.test.js` checks the expansion of `${workspaceFolder}` (CTest: `VscodeExpand`).
- After `npm install` it also tokenizes every `tests/scripts/*.hot` file
  and compares `test/sample.hot` with `test/sample.snap`.
- Without `vscode-textmate` the tokenizer part is skipped and the test prints a notice.
- Run `node test/grammar.test.js --update` to accept a new snapshot.

The build and the test never use the network.
