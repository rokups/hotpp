# Hot++ for JetBrains IDEs

A JetBrains IDE gets two things for `.hot` files: the colors from a TextMate bundle, and the language server.

## Colors: TextMate bundle

The VS Code extension in `editors/vscode` holds the TextMate grammar. The IDE reads this folder as a bundle, so no copy is necessary.

1. Open `Settings > Editor > TextMate Bundles`.
2. Press `+` and select the folder `editors/vscode` of the Hot++ source tree.
3. Press `OK`. The IDE now colors `*.hot` files.

If the IDE does not know the extension `.hot`, add it in `Settings > Editor > File Types > TextMate`.

## Language server: LSP4IJ

Use the plugin LSP4IJ (Red Hat). It works in the Community and Ultimate editions. The server `hot-ls` must be in `PATH`.

1. Install LSP4IJ from the marketplace.
2. Open the view `Language Servers` and press `+` (`New Language Server`).
3. In the dialog, press `Import from template` and select the folder `editors/jetbrains/lsp4ij`. The folder has `template.json`.
4. Or enter the values by hand. Name: `Hot++`. Command: `hot-ls --stdio`. Mappings, `File name patterns`: `*.hot` with the language id `hot`.
5. Press `OK`. Open a `.hot` file. The language server starts on the first file.

The command can have more flags, for example `--private-clangd-dir` or `--no-native-tokens`. See `hot-ls --help`.

## Notes

- The template format follows the documentation of LSP4IJ. It is not tested with a real IDE in this repository.
- The native LSP support of the IDE (Ultimate) has no file-based setup for a custom server. Use LSP4IJ.
- The test `LanguageServer.EditorKeywordListsMatchTheParser` checks the TextMate grammar that this bundle uses.
