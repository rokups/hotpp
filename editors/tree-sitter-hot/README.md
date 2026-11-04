# tree-sitter-hot

A loose tree-sitter grammar for Hot++ scripts (`.hot`). It is for highlighting only. It does not check the language. It finds comments, strings with `${}`, raw strings (`R"(text)"` without a delimiter), numbers, keywords, builtin types, declarations (class, struct, interface, enum, function, field, parameter), attributes and calls.

Files:

- `grammar.js` is the grammar. The lists `KEYWORDS` and `TYPES` follow the parser.
- `queries/highlights.scm` is the highlight query.
- `test/check.js` generates the parser and parses the example scripts.

The repository has no generated parser. Generate it with the `tree-sitter` tool:

```sh
cd editors/tree-sitter-hot
tree-sitter generate
tree-sitter parse test/sample.hot
tree-sitter highlight test/sample.hot
```

## Neovim

With `nvim-treesitter` (the `main` branch):

```lua
vim.api.nvim_create_autocmd('User', {
  pattern = 'TSUpdate',
  callback = function()
    require('nvim-treesitter.parsers').hot = {
      install_info = { path = '/path/to/hotpp/editors/tree-sitter-hot', generate = true },
      tier = 3,
    }
  end,
})
vim.filetype.add({ extension = { hot = 'hot' } })
```

Run `:TSInstall hot`. Copy `queries/highlights.scm` to `queries/hot/highlights.scm` in the runtime path.

## Tests

- `test/check.js` needs `node` and `tree-sitter`. It exits with the code 77 (CTest shows "Skipped") when `tree-sitter` does not run. CTest runs it as `TreeSitterGrammar`.
- The test `LanguageServer.EditorKeywordListsMatchTheParser` compares the lists in `grammar.js` and `queries/highlights.scm` with the keywords of the parser.
