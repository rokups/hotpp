# S++ for VS Code

This extension gives support for S++ scripts (`.spp`, language id `spp`).

## Features

- Syntax highlighting with a TextMate grammar. It covers the keywords, the builtin types,
  the numeric suffixes, the attributes (`[native, handle("...")]`), the comments and the
  string interpolation `${expr}` and `${expr:spec}`.
- Brackets, comments and auto-close pairs (`language-configuration.json`).
- A thin client of the S++ language server. It runs `spp-ls --stdio`.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `spp.server.path` | `spp-ls` | Path of the server binary. |
| `spp.server.args` | `["--stdio"]` | Arguments of the server. |
| `spp.server.enable` | `true` | Start the server. |

If the binary is missing, the extension shows one warning. Highlighting still works.
The server is a fork of clangd. It is not built here (see `DECISIONS.md`, item 3).

## Install for development

1. Run `npm install` in this folder.
2. Open this folder in VS Code and press F5.

You can also link the folder into `~/.vscode/extensions/`.
Without the server, set `files.associations` to map `*.spp` to `cpp`. The grammar does not apply then.

## Test

Run `node test/grammar.test.js`. CTest runs it as `VscodeGrammar` when `node` is found.

- The test always checks the JSON files and the keyword and builtin type coverage.
  It reads the builtin types from `src/translator/builtin_types.h`.
- After `npm install` it also tokenizes every `example/scripts/*.spp` file
  and compares `test/sample.spp` with `test/sample.snap`.
- Without `vscode-textmate` the tokenizer part is skipped and the test prints a notice.
- Run `node test/grammar.test.js --update` to accept a new snapshot.

The build and the test never use the network.
