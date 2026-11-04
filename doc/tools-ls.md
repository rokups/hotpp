---
layout: default
title: Language server internals
nav_order: 45
---

This page describes how `hot-ls` works inside, and how an editor uses it with `clangd`. For the setup steps see [Tools](tools.html).

`hot-ls` is a stdio LSP 3.17 server. It serves `.hot` files only. A `.cpp` script is a C++ file for `clangd` (see [C++ scripts](cpp-scripts.html)). The code is in `src/ls/`. The extension in `editors/vscode/` starts it.

## Editor setup: two servers

An editor uses two language servers on one project:

- The official `clangd` extension serves the C++ files. It reads the `compile_commands.json` of the project build.
- The `hot` extension and `hot-ls` serve all features for the `.hot` files. For a `.cpp` script that a host lists, they show only the errors of `hotc` as diagnostics. `hot-ls` is not a transparent `clangd` proxy. It does not answer for C++ files.

Steps:

1. Install the `clangd` extension (`llvm-vs-code-extensions.vscode-clangd`) and `clangd`. Set it up for the C++ build as usual.
2. Build `hot-ls` and install the extension in `editors/vscode/`. Put the `hot-ls` arguments in `hot.server.args`. The include dirs come from the script hosts (next subsection). The flags `-I` and `--cxxflag` still work as overrides.
3. Optional, for workspace symbols and the background index of scripts: run `hotc <file.hot>... --gen N --out-dir DIR --compile-commands CDIR`. Give `--compile-commands-dir CDIR` to `hot-ls`.
4. Add `.cache/` to the ignore list of the project (for example in `.gitignore`).

Both servers can use the same project and the same `.cache/` directory. They do not disturb each other:

- The C++ `clangd` stores its index in `<dir of compile_commands.json>/.cache/clangd/index/`.
- The `clangd` child of `hot-ls` uses the same `--compile-commands-dir` by default. So it uses the same index directory, `<dir of compile_commands.json>/.cache/clangd/index/`, and the project is indexed once. `clangd` writes a shard to a temporary file and renames it, so two `clangd` processes can write the directory. The shadow files of the scripts are virtual and need no entry in `compile_commands.json`. The child writes nothing else in the directory of the database.
- The option `--private-clangd-dir` (or `privateClangdDir` in `initializationOptions`) gives the child its own database directory `.cache/hot/clangd/` in the first workspace folder. The directory has a link to the real `compile_commands.json` (a copy on Windows). So its index is in `.cache/hot/clangd/.cache/clangd/index/`, and the project is indexed twice. Use it if the shared index causes a problem. `XDG_CACHE_HOME` does not move this index.
- The symbol index of `hot-ls` is in `.cache/hot/index/`.
- The C++ `clangd` does not get an open `.hot` document, and `hot-ls` does not get a C++ document. A diagnostic of one server never shows in the files of the other.

`hot-ls` supports many workspace folders. The index directory and the `clangd` directory are in the first folder that `initialize` gives. Each is one flat directory for all folders, so a shard of any folder is in it. The `clangd` child keeps its directory for the whole session. If `initialize` gives no folder, the first folder that the editor adds gives the index directory. The editor can add or remove a folder (`workspace/didChangeWorkspaceFolders`). The server then updates the folder list and reloads the host files. For a removed folder, it deletes the shards of the files of that folder. It keeps a shard of a file that another folder still holds.

A workspace symbol request also lists the classes and members of the `.hot` files of all folders. Cost rule: the request never analyzes a closed file. It lists a closed file only if the index has a valid shard for it. A shard comes from a rename or references request that scans the file, and from a document that closes with the text of the file on disk. An open document uses its own analysis. An empty query lists no script symbol.

## Automatic setup (script hosts)

A script host tells `hot-ls` how the engine compiles its scripts. CMake writes the host files. So the user does not pass `-I` flags.

```cmake
include(cmake/HotScripts.cmake)
# hot_add_scripts calls hot_script_host by themselves.
# Call it directly for a host without these functions:
hot_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

- At generate time, `hot_script_host` writes `${HOT_HOSTS_DIR}/<NAME>.json`. The default is `<source dir>/.cache/hot/hosts`. The file has the explicit include dirs, the script dirs and the scripts, with a full JSON escape, and `sourceDir` (the source directory of the call). The defines, options and include dirs of `TARGET` go to the side files `<NAME>.defines`, `<NAME>.options` and `<NAME>.includes`, one entry on each line. The JSON file names them in `definesFile`, `optionsFile` and `includesFile`. A relative path is relative to `sourceDir`, and `hot-ls` makes it absolute and normalizes it. `hot-ls` drops a repeated include dir. The order is the project include dir, the dirs of `TARGET`, then the explicit `INCLUDES`. Every valid include path works: spaces, quotes, backslashes, non-ASCII characters, `$`, `#`, brackets, `,`, `>`, `|`, `=`, a leading `-`, `..`, a trailing slash, relative paths, and dirs from imported targets or generator expressions. Two cases do not work. A path with a newline cannot be in a one-line-per-entry file. A path with a semicolon is a CMake list separator, so CMake splits it in two entries before `hot_script_host` sees it. The server passes each flag to `clangd` as one item of an argument array, so no shell escape is needed.
- `hot-ls` reads the host files from `<root>/.cache/hot/hosts`. The argument `--hosts-dir` (init option `hostsDir`) sets another directory.
- `hot-ls` gives the prelude files of the host (`PRELUDES` of `hot_script_host`) to each script of the host. A host without preludes uses the init option `preludes`, a list of files. An open prelude file does not get its own text as a prelude.
- `hot-ls` also gives the user preludes. These are the files with the suffix `PRELUDE_SUFFIX` of `hot_script_host` in the script dirs of the host. A host without a suffix uses the init option `preludeSuffix`, and then searches the workspace folders. The search runs at each analysis, so a new prelude file works at once. The user preludes come after the other preludes, sorted by path.
- A script matches the host with the exact `scripts` entry. Else it matches the host with the longest `scriptDirs` prefix. A tie goes to the lower name.
- The effective options are the host values, then the explicit `-I` and `--cxxflag` values. So the flags override the host.
- A host is skipped when its `binaryDir/CMakeCache.txt` is missing. The server checks it again later.
- With no host, the old behavior stays. The server also shows an Information diagnostic at 0:0. It does not show it when `-I` or `--compile-commands-dir` is set.
- The server reloads the host files when the mtime of the hosts dir changes. The VS Code extension watches `**/*.hot` and the host files.
- Limits: many build dirs share one hosts dir, so the last configure wins. The server does not canonicalize symlinked workspace paths. Multi-Config generators are not tested.

How it works:

- The server owns the text of each open `.hot` document. On each change it runs `hotc::analyze()`. It gets the parse errors, the facts (classes, members), the generated C++ and the source map.
- It starts a stock `clangd` as a child process. It gives the generated C++ to `clangd` as the virtual file `<script>.hot.cpp`. The compile command comes from `hot::compile_command`, through `workspace/didChangeConfiguration`.
- It maps each position between the script and the generated C++ with the source map. A line without a segment uses the `#line` directives.
- `hotc::rewrite_generated_names` and `hotc::rewrite_clang_message` change generated names to script names in hover text, diagnostics and completion labels.
- JSON comes from `nlohmann_json` (CPM).

Features:

- Diagnostics: the parse errors of analyze, and the `clangd` errors at the script position.
  - A rename warning (severity Warning) shows at the new name of a field. It shows when a field of the open text is gone and a new field has no `[alias]`. The message is `field 'old' is renamed to 'new' without [alias]; the value is lost at hot reload`. The fields at `didOpen` are the baseline, and a save does not refresh them. A field with `[alias("old")]` removes the warning. A new field without a removed field gives no warning.
- Completion works on any identifier prefix, with no trigger needed. The trigger characters are `.` and `::` (the client sends `:`, and the server ignores a single `:`). The list has: locals and parameters in scope, module variables, script enums, classes and structs, the enumerators after `Mode.`, the script members (own and inherited from script bases) after `this.` or after a variable of a script class, the native types, functions and members from `clangd`, and the Hot++ keywords and builtin types from a fixed list. All names are in script form (`std.string`). The helpers of the translator (thunks, `SLOT_`, `_Shell`, plumbing) are not shown. The list is filtered by the typed prefix.
  - The script facts come from a token scan of the text. It works on a script with syntax errors. For `clangd`, the server puts the typed prefix and the placeholder `__hot_complete` in place of the word at the cursor. It closes the open brackets of an incomplete statement and translates again with the tolerant parse. Then it asks `clangd` at the placeholder.
  - If the translation fails, the list has the keywords and the script facts, without `clangd`.
  - `hot/indexStats` also gives `completions`, the number of completion translations. A test uses it to check the reuse.
  - The server keeps the last `clangd` answer. While the user types more characters of the same word, and the rest of the text does not change, it filters the answer and does not translate again. A failed translation is kept the same way.
- A `clangd` error in the code of an imported script is published for the imported script, at its own line and column. The importing document does not get it. An open script keeps its own diagnostics.
- Signature help (`(` and `,` trigger) asks `clangd` at the generated call. The active parameter is the number of commas before the cursor. The labels have script names.
- Formatting (`textDocument/formatting` and `textDocument/rangeFormatting`) is native. See "Formatter rules" below. The result is one edit, or none when the text is already formatted.
- Code actions (quick fix): insert a missing `;` for the error `expected ';'`, and replace a word with the name from a `clangd` `did you mean '...'` message.
- Hover, go-to-definition and references. A script symbol goes to the script. A native symbol goes to the native header.
  - A name with no map segment (a type name, a base class, a name after `new`, a field initializer) has no position in the generated C++. For hover, definition and references, the server then looks for the same word in the generated lines of the script line. It asks `clangd` at the first place that answers. A word in a comment or in a string gets no hover.
  - References of a native symbol give the uses in the script and the places in C++ files. A use with no map segment is found by the word in its script line: the k-th use on the generated line is the k-th use on the script line, so a parameter with the same name is skipped. The declaration and the definition in the header are added, because `clangd` has them only in its index.
- Document symbols come from analyze. Semantic tokens come from a small lexer and the facts.
  - Native names (types, functions, methods, fields, enumerators) get a token from `clangd` (`textDocument/semanticTokens/full` on the shadow document). A native name has no map segment, so the match is by the name and the script line: a token of the generated lines of a script line is a token for the same name in that script line. Only a name that the script lexer does not classify gets a token. The type maps to the stable legend: class, struct, enum, interface to `class`; type alias to `type`; function and method to `method`; field and enumerator to `property`. A name that `clangd` classifies in two ways in those lines (a parameter with the name of a type) gets no token. A macro, a local and a word in a string or a comment get no token. The server keeps the answer of `clangd` for each version of the shadow document, so a second request for the same text does not ask `clangd` again. A request that has no answer for its version waits at most 300 ms. Then it gives the script tokens and the native tokens of the last answer, but only the tokens whose name is still at the same place. A late answer goes to the cache for the next request, so the server thread never waits for the full time. The worker call to `clangd` has a limit of 3 seconds. The server reads at most 50000 tokens. Without `clangd`, or with the switch off, the tokens are the script tokens. The switch is `nativeSemanticTokens` in `initializationOptions` (default `true`) or the argument `--no-native-tokens`.
- Inlay hints (`textDocument/inlayHint`) show the parameter name before each argument of a call of a script method or constructor. The kind is Parameter. The server finds the callee by name in the methods and constructors of the facts, and it reads the parameter names from the declaration. When more than one member has the name, a hint shows only if all members that fit the argument count have the same parameter name at that place. An argument that is the plain name of its parameter has no hint. The request `range` limits the answer. A call of a native function, method or constructor gets hints from `clangd`: see below. A call on a native object with the name of a script method can get a wrong hint.
  - A call with a name that no script method has asks `clangd` for the signature (`textDocument/signatureHelp`). The place is the end of the generated arguments, so the arguments choose the overload. When the generated call does not end on its line, the place is after the `(`. Then a hint shows only if all signatures with enough parameters have the same names. The server takes the parameter names of the signature that `clangd` marks active. A parameter with a type only, a name that starts with `_`, a call with more arguments than parameters, and a declaration have no hint. The same rules as for script calls apply to the argument. The native calls of one request share a time budget of 3 seconds and the limit of 64 calls, so a slow `clangd` does not hold the editor.
  - Limit: `new X(...)` of a native class with a `handle("std::shared_ptr<$T>")` handle is `std::make_shared<X>(...)` in the generated C++. Its signature has no parameter names, so this call has no hint.
- Code lens (`textDocument/codeLens`) shows one lens above each script class, method and constructor of the open document. A native symbol has no declaration in a script, so it has no lens. Its callers are in the call hierarchy. The title is `N references` (`1 reference` for one). The lens has no command, so the title is all. A destructor has no lens.
  - The count is a name scan of the open document: each identifier with the name, except the declarations. Comments and strings do not count. The scan does not use `clangd` and does not read the other scripts, so the request is quick. A use in another script is not in the count.
  - The server does not know the type of an object. So the same name in two classes, or a local with the name of a method, counts too. The overloads of a method have the same count.
  - The lens of a constructor counts every use of the class name (for example `new Tool(1)`), as the lens of the class does. The name of a constructor or a destructor in its own declaration is not a use.
- Call hierarchy (`textDocument/prepareCallHierarchy`, `callHierarchy/incomingCalls`, `callHierarchy/outgoingCalls`) works for the methods and constructors of the facts. It is name based. For script methods it works in one document. For native callables see the last point of this list.
  - An incoming call is a call with the name of the method in the body of a method or constructor of the same document. The calls are grouped by the method that holds them, and `fromRanges` has the places of the names. A recursive method is its own caller.
  - An outgoing call is a call in the body of the method with the name of a method or constructor of the document. A call to a native function or to a local with the same name can show as an outgoing call. The overloads of a method share their calls.
  - A destructor is not an item, and the calls in its body are not listed. A call in a field initializer has no caller. A call in a comment or in a string is not a call.
  - A request with a malformed item gets an empty list. A `.cpp` document gets no answer for these requests.
  - Native callables: `prepareCallHierarchy` on a name that no script method has asks `clangd` at the generated place. The item is the native function, method or constructor, with its place in the C++ header. The item holds the item of `clangd`, so the next requests can give it back. `incomingCalls` of a native item lists the script callers, then the C++ callers that `clangd` has in its index. The script callers are a name scan of all open `.hot` documents (a closed document is not scanned). A call outside a method or a constructor (a free function, a field initializer) has no caller item. A caller in a generated file is not listed. `outgoingCalls` of a script method also lists the native callees: the server asks `clangd` once for each name that is not a script method (at most 16 names, 3 seconds). `outgoingCalls` of a native item is the answer of `clangd`. The request to `clangd` for `incomingCalls` and `outgoingCalls` of a native item waits at most 3 seconds. The match of the script callers is by name only: a document that has a script method or constructor with the name is not scanned for a native item with this name, and a local with the name of a native function counts as a call. The C++ callers depend on the index of `clangd`: they can be missing until the index is ready.
- Rename changes the script symbols (class, member, field) in all `.hot` files of the workspace. The server scans the workspace folders from `initialize` (it skips hidden folders and `node_modules`). A class or a member with the same name in another script (for example the versions of a script) gets the new name there too. An open document uses its editor text. A file on disk goes to `clangd` only for the request. A class name has no segment in the source map, so the semantic tokens find its places. References also look in the other `.hot` files. Rename of an imported C++ symbol (a native type, function, method, field or enumerator) goes through `clangd` (`textDocument/rename`) at the place in the shadow document. The place has a map segment, or it comes from the word (see references). The answer is a `WorkspaceEdit`:
  - An edit in a shadow document maps back to a script range. An edit with no map segment uses the same k-th use rule as references. An edit in generated-only code (for example the copy of a parameter) is dropped. An edit in a real C++ file passes through unchanged. The declaration and the definition places in the workspace are added when `clangd` does not give them (a header that no indexed translation unit includes). Uses in C++ files that `clangd` does not index are not changed.
  - The server refuses with an error, and changes nothing, in these cases: a declaration or a definition is in a file outside the workspace folders (a system header, a library); an edit changes a script or a C++ file outside the workspace folders; `clangd` gives an error or no edit; a changed range is not the old name (the server checks the text of each range in the generated code and in each C++ file); a changed range has no clear place in the script; a script line has more than one word with the name and the k-th use rule does not give the place (for example `int count = c.count;` with a type or a local of the same name that the rule cannot match); or two edits of one generated line map to one place in a script line with more than one such word. The declaration or definition place is not added when it overlaps an edit of `clangd`. The calls share a time budget of 10 seconds.
  - `textDocument/prepareRename` agrees with rename: it gives the range of the word for a renameable symbol (script or native), else an error with the same message. For a native symbol it runs the check of rename with a probe name.
- The shard digest covers the text of the file and the texts of the scripts that it imports (also the imports of the imports). A changed import makes the shard stale. The saved symbol index speeds up rename and references on closed `.hot` files. There is no limit on the number of files. It has one shard for each `.hot` file. All shards are flat in `.cache/hot/index/` in the first workspace folder. `--index-dir dir` sets another directory. The shard name is a hash of the file path, so a new shard replaces the old shard and the index does not grow when a file changes. A shard holds a format version, the content digest, the classes and members, and the places of each class. The places of a member come from `clangd`. The server adds them to the shard at the first rename. The server loads a shard only if the version and the digest match. Else it analyzes the file again and writes the shard again. A write goes to a temporary file, then a rename puts it in place. At `initialize` and at each scan the server deletes the shards of missing files. The index is filled when a rename or a references request scans a closed file. It is not filled at startup. Add `.cache/` to the ignore list of the project. The request `hot/indexStats` gives the counts (`analyzed`, `loaded`, `stored`).
- Workspace symbols come from the `clangd` index. Give `--compile-commands-dir` with the output dir of `hotc --compile-commands`. Then `clangd` runs its background index on the generated files. A symbol of a generated file maps to its script member. The helpers of the translator (thunks, slot functions) are not shown.

The `#line` check: `clangd` reports the real line of the generated file, not the `#line` line. So the generated C++ keeps its `#line` directives. No translator option is necessary.

From C++ to the script:

- In a `.cpp` file from `hotc --compile-commands`, the `#line` directives name the `.hot` file. A debugger and the compiler use them. `clangd` does not use them for navigation.
- The custom request `hot/toScript` (params: `textDocument`, `position`) gives the script location of a position in a generated file. It uses the source map for an open script, and the `#line` directives for a file on disk.
- The test `LanguageServer.GeneratedFileOnDiskMapsBackToTheScript` checks it.

Limits:

- After a translation error, `clangd` keeps the last good generated C++. The server keeps the script text of that C++ and maps each position through the edit (common prefix and suffix). A position inside the edit has no mapping. The old `clangd` diagnostics are dropped until the next good translation.
- `clangd` does not publish again when a new version has the same diagnostics. The server keeps the last raw `clangd` diagnostics of each document. After each edit it maps them again to the new script text. A new publish of `clangd` replaces them.
- A `#line` fallback has the line only. The column is the column of the same word in the script line, or the first non-blank column.
- Two open scripts that import the same script publish diagnostics for it. The last publish wins.

The tests are in `tests/integration/ls/` (`LanguageServer.*`). They run the server on a thread with an LSP client over pipes. The `clangd` tests are skipped when `clangd` is missing. Set `HOT_LS_CLANGD_LOG=file` to get the `clangd` log of a test.

## Formatter rules

The formatter of `hot-ls` works on a token stream. It keeps the comments and the literals as tokens. It does not parse the script. The code is `src/ls/format.cpp`. The function `format_hot` is pure. It is idempotent and it never changes a token: only white space changes. Tests check this on all example scripts.

- The line breaks stay as the author put them. The formatter does not move a brace and does not join or split a line.
- The indent follows the open `{`, `(` and `[`. A line in open parentheses or brackets gets one more level. A line that continues an expression (after `=`, `+`, `&&`, `.` ... or before `&&`, `?`, `.` ...) gets one more level. The body of an `if`, `for`, `while`, `else` or `do` on its own line gets one more level. A `case` or `default` label is one level inside the `switch`. Its body is one more level. The width is the `tabSize` of the request (default 4). `insertSpaces: false` gives one tab for each level.
- One space goes around a binary operator (`=`, `==`, `+`, `-`, `/`, `&&`, `||`, `=>`, `??` and the compound assignments). `*`, `&` and `&&` get it when the author used the same spacing on both sides. A unary operator takes no space. `<`, `>`, `?` and `:` keep the spacing of the author, because they can be a generic, a nullable type or a label.
- No space before `,` and `;`. One space after them. No space inside `( )` and `[ ]`. No space before the `(` of a call. One space between `if`, `for`, `foreach`, `while`, `switch`, `catch`, `using`, `lock`, `return` and the `(`. No space around `.`, `?.` and `::`. No space in `operator+(`.
- One space before `{` and inside `{ }` on one line.
- A run of blank lines becomes one blank line. The text has no leading blank line, no trailing white space and one new line at the end. A file with CRLF keeps CRLF.
- The text of a string, of a char, of a `${...}` and of a block comment is byte exact, also the inner lines and their trailing white space. A line comment loses its trailing white space only. All comments keep their place. A line comment keeps the white space before it.
- A token sequence that the rules do not know keeps the spacing of the author (one space for a run of spaces).

Range formatting runs the formatter on the whole text. The edit holds the new text of the lines in the range only. A line inside a string or a block comment does not change.
