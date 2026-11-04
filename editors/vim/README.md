# Hot++ for Vim and Neovim

Files for `.hot` scripts:

- `ftdetect/hot.vim` sets the file type `hot` for `*.hot`.
- `ftplugin/hot.vim` sets the comment string and the format options.
- `syntax/hot.vim` is the syntax file. It colors comments, strings with `${}`, numbers, keywords, builtin types, attributes, declarations and calls.

## Install

Add this folder to the runtime path. With a plugin manager, give the folder `editors/vim` of the Hot++ source tree. Or copy the three folders to `~/.vim` (Vim) or `~/.config/nvim` (Neovim).

```vim
set runtimepath+=/path/to/hotpp/editors/vim
```

## Language server (Neovim)

`hot-ls` must be in `PATH`. For a `.cpp` script, `hot-ls` shows only the `hotc` errors. `clangd` serves the other features. The root markers are the places where a Hot++ project keeps its files. `.cache/hot` is the folder of the host files that CMake writes.

Neovim 0.11 and later:

```lua
vim.lsp.config('hot_ls', {
  cmd = { 'hot-ls', '--stdio' },
  filetypes = { 'hot' },
  root_markers = { '.cache/hot', 'compile_commands.json', 'CMakeLists.txt', '.git' },
})
vim.lsp.enable('hot_ls')
```

Neovim 0.10 with `nvim-lspconfig`:

```lua
local configs = require('lspconfig.configs')
local lspconfig = require('lspconfig')

if not configs.hot_ls then
  configs.hot_ls = {
    default_config = {
      cmd = { 'hot-ls', '--stdio' },
      filetypes = { 'hot' },
      root_dir = lspconfig.util.root_pattern('.cache/hot', 'compile_commands.json', 'CMakeLists.txt', '.git'),
      single_file_support = true,
    },
  }
end
lspconfig.hot_ls.setup({})
```

Add flags to `cmd` when you need them. By default, `hot-ls` shares the index of `clangd` with the C++ `clangd` of the project. Add `--private-clangd-dir` to keep a private index. Add `--no-native-tokens` to remove the semantic tokens for C++ names.

## Language server (Vim)

With `vim-lsp`:

```vim
au User lsp_setup call lsp#register_server({
  \ 'name': 'hot-ls',
  \ 'cmd': {server_info -> ['hot-ls', '--stdio']},
  \ 'allowlist': ['hot'],
  \ })
```

## Tree-sitter

Neovim can use the grammar in `editors/tree-sitter-hot`. See the README of that folder.

## Test

The test `LanguageServer.EditorKeywordListsMatchTheParser` compares the `syn keyword` lines of `syntax/hot.vim` with the keywords of the parser.
