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
4. Alternative to step 3: let the program write the database. Set the option `compile_commands_dir` of the backend, or the environment variable `HOT_COMPILE_COMMANDS_DIR=<dir>`. The runtime then writes `<dir>/compile_commands.json` after each good build. Give `--compile-commands-dir <dir>` to `hot-ls`. Each entry points to the generated `.cpp` in the object cache (`<cache_dir>/src/<unit>.cpp`). So the IDE features (`clangd`, workspace symbols) work on the scripts. The entries of the old runs stay while their `.cpp` exists. The runtime needs a cache dir (`HOT_CACHE_DIR`).
5. Add `.cache/` to the ignore list of the project (for example in `.gitignore`).

Both servers can use the same project and the same `.cache/` directory. They do not disturb each other:

- The C++ `clangd` stores its index in `<dir of compile_commands.json>/.cache/clangd/index/`.
- The `clangd` child of `hot-ls` uses the same `--compile-commands-dir` by default. So it uses the same index directory, `<dir of compile_commands.json>/.cache/clangd/index/`, and the project is indexed once. `clangd` writes a shard to a temporary file and renames it, so two `clangd` processes can write the directory. The shadow files of the scripts are virtual and need no entry in `compile_commands.json`. The child writes nothing else in the directory of the database.
- The option `--private-clangd-dir` (or `privateClangdDir` in `initializationOptions`) gives the child its own database directory `.cache/hot/clangd/` in the first workspace folder. The directory has a link to the real `compile_commands.json` (a copy on Windows). So its index is in `.cache/hot/clangd/.cache/clangd/index/`, and the project is indexed twice. Use it if the shared index causes a problem. `XDG_CACHE_HOME` does not move this index.
- The symbol index of `hot-ls` is in `.cache/hot/index/`.
- The digest of a shard holds a build identity of the translator: the size and the mtime of the running `hot-ls` executable. No version string changes for each build, so the executable is the simple stable source. A new build gives a new digest, and the server rejects the old shards. `ServerOptions::build_id` overrides the identity (tests).
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

- At generate time, `hot_script_host` writes `${HOT_HOSTS_DIR}/<NAME>.json`. The default is `<source dir>/.cache/hot/hosts`. The file has the explicit include dirs, the script dirs and the scripts, with a full JSON escape, and `sourceDir` (the source directory of the call). The defines, options and include dirs of `TARGET` go to the side files `<NAME>.defines`, `<NAME>.options` and `<NAME>.includes`, one entry on each line. The JSON file names them in `definesFile`, `optionsFile` and `includesFile`. The side file `<NAME>.roots` has the source dirs of the build, one absolute path on each line. A line with the prefix `!` is a SYSTEM dir. The JSON file names it in `sourceRootsFile` and has the build dir in `binaryDir`. A hand-written host file can use the arrays `sourceRoots` and `systemDirs`. A relative path is relative to `sourceDir`, and `hot-ls` makes it absolute and normalizes it. `hot-ls` drops a repeated include dir. The order is the project include dir, the dirs of `TARGET`, then the explicit `INCLUDES`. Every valid include path works: spaces, quotes, backslashes, non-ASCII characters, `$`, `#`, brackets, `,`, `>`, `|`, `=`, a leading `-`, `..`, a trailing slash, relative paths, and dirs from imported targets or generator expressions. Two cases do not work. A path with a newline cannot be in a one-line-per-entry file. A path with a semicolon is a CMake list separator, so CMake splits it in two entries before `hot_script_host` sees it. The server passes each flag to `clangd` as one item of an argument array, so no shell escape is needed.
- `hot-ls` reads the host files from `<root>/.cache/hot/hosts`. The argument `--hosts-dir` (init option `hostsDir`) sets another directory.
- `hot-ls` gives the prelude files of the host (`PRELUDES` of `hot_script_host`) to each script of the host. A host without preludes uses the init option `preludes`, a list of files. An open prelude file does not get its own text as a prelude.
- `hot-ls` also gives the user preludes. These are the files with the suffix `PRELUDE_SUFFIX` of `hot_script_host` in the script dirs of the host. A host without a suffix uses the init option `preludeSuffix`, and then searches the workspace folders. The search runs at each analysis, so a new prelude file works at once. The user preludes come after the other preludes, sorted by path.
- A script matches the host with the exact `scripts` entry. Else it matches the host with the longest `scriptDirs` prefix. A tie goes to the lower name. A host with no `preludes` and no `preludeSuffix` is incomplete. When the winner is incomplete and another matching host has `preludes` or a `preludeSuffix`, the best of those hosts wins instead, also with a lower score. Else the normal rule applies.
- The effective options are the host values, then the explicit `-I` and `--cxxflag` values. So the flags override the host.
- A host is skipped when its `binaryDir/CMakeCache.txt` is missing. The server checks it again later.
- With no host, the old behavior stays. The server also shows an Information diagnostic at 0:0. It does not show it when `-I` or `--compile-commands-dir` is set.
- The server reloads the host files when the mtime of the hosts dir changes. The VS Code extension watches `**/*.hot` and the host files.
- Limits: many build dirs share one hosts dir, so the last configure wins. The server does not canonicalize symlinked workspace paths. Multi-Config generators are not tested.

How it works:

- The server owns the text of each open `.hot` document. On each change it runs `hotc::analyze()`. It gets the parse errors, the facts (classes, members), the generated C++ and the source map.
- It starts a stock `clangd` as a child process. It gives the generated C++ to `clangd` as the virtual file `<script>.hot.cpp`. The compile command comes from `hot::compile_object_command`, through `workspace/didChangeConfiguration`.
- It maps each position between the script and the generated C++ with the source map. A line without a segment uses the `#line` directives.
- `hotc::rewrite_generated_names` and `hotc::rewrite_clang_message` change generated names to script names in hover text, diagnostics and completion labels.
- JSON comes from `nlohmann_json` (CPM).

Features:

- Diagnostics: the parse errors of analyze, and the `clangd` errors at the script position.
  - A rename warning (severity Warning) shows at the new name of a field. It shows when a field of the open text is gone and a new field has no `[alias]`. The message is `field 'old' is renamed to 'new' without [alias]; the value is lost at hot reload`. The fields at `didOpen` are the baseline, and a save does not refresh them. A field with `[alias("old")]` removes the warning. A new field without a removed field gives no warning.
  - A translator warning (severity Warning) shows too, for example `a cast to the same type has no effect`. That warning has the quick fix `Remove the cast`.
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
  - Hover of a static method, a static field or a class constant of a script class shows the declaration line.
    - The line comes from the table of the resolved names.
    - The server uses the table first.
    - It uses a text scan of the lenient facts only when the table gives no result.
    - The scan serves hover and definition of a static member, and references of a script class.
    - The table has gaps for a broken script:
      - G1: a use inside the error statement and in the rest of its body has no table entry. A list of references can miss such a use.
      - G2: an import with an error loses its `using` line, so its names are not in the table.
      - G3: more than 4 error regions give an empty table.
    - Remove the text scan when G1, G2 and G3 are closed.
  - A name with no map segment (a type name, a base class, a name after `new`, a field initializer) has no position in the generated C++. For hover, definition and references, the server then looks for the same word in the generated lines of the script line. It asks `clangd` at the first place that answers. A word in a comment or in a string gets no hover.
  - References of a native symbol give the uses in the script and the places in C++ files. A use with no map segment is found by the word in its script line: the k-th use on the generated line is the k-th use on the script line, so a parameter with the same name is skipped. The scan of the other scripts skips a closed script when its stored table (cache or index shard) shows that each code word with the symbol name is a resolved script symbol. A closed script with no stored table is analyzed as before. The scan stores the table of each closed script that it analyzes. The scan has its own time budget: `reference_budget_ms` in `ServerOptions` (default 10 seconds). The rename budget is also 10 seconds. A cancel stops the scan (see Threads). When the budget stops the scan, or an ask to `clangd` gets no answer, the server writes `native references: scan stopped by the time budget or by a failed ask, N of M scripts` (N scripts are scanned) to stderr and sends `window/showMessage` (warning) with the same fact. The list of references is then partial. The declaration and the definition in the header are added, because `clangd` has them only in its index.
- Document symbols come from analyze. Semantic tokens come from a small lexer and the facts. The postfix `!` (non-null assertion) is a token of the type `operator`; `!=` and the prefix `!` are not.
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
- Rename changes the script symbols (class, member, field) in all `.hot` files of the workspace. The server scans the workspace folders from `initialize` (it skips hidden folders and `node_modules`). A class or a member with the same name in another script (for example the versions of a script) gets the new name there too. An open document uses its editor text. A file on disk goes to `clangd` only for the request. A class name has no segment in the source map, so the semantic tokens find its places. References also look in the other `.hot` files. Rename of an imported C++ symbol (a native type, function, method, field or enumerator) goes through `clangd` (`textDocument/rename`) at the place in the shadow document. The place has a map segment, or it comes from the word (see references). The server asks `clangd` once for each script that uses the symbol. These are the open scripts, and the closed scripts that the server finds in the workspace folders, in the script dirs of the hosts, and in the include dirs. A closed script goes through a temporary shadow document. The server merges the answers by uri. The answer is a `WorkspaceEdit`:
  - An edit in a shadow document maps back to a script range. An edit with no map segment uses the same k-th use rule as references. An edit in generated-only code (for example the copy of a parameter) is dropped. An edit in a real C++ file passes through unchanged. A header in a workspace folder is allowed. A header outside the workspace folders is allowed only when all of these hold: it is in an include dir of the host, it is in a source dir of the build (`${HOT_HOSTS_DIR}/<NAME>.roots`), it is not in a SYSTEM dir, and it is not in the build dir of the host. The host of the script gives these dirs. A script that no host matches uses the nearest host only when the script is in a workspace folder. Vendored code in a source tree stays editable, unless its dir has the CMake property `SYSTEM`. A header in the build dir of the host is refused also inside the workspace. A host file from an older CMake run has no source roots, so it refuses each such header until the next CMake run. The setting `renameHostHeaders` in `initializationOptions` (default `true`) or the argument `--no-rename-host-headers` turns off the edit of such a header. A system header (for example below `/usr/include`) stays refused. The declaration and the definition places in the workspace are added when `clangd` does not give them (a header that no indexed translation unit includes). Uses in C++ files that `clangd` does not index are not changed. When the rename changes the name of a class, the server adds the names of the constructors and of the destructor in the declaring header (also with a macro or an attribute after `struct`/`class`). It also adds the `Old::Old(` and `Old::~Old(` definitions in that header. A definition in a C++ file that `clangd` does not index is not changed.
  - The server refuses with an error, and changes nothing, in these cases: a declaration or a definition is in a file outside the workspace folders and not an allowed header of the host (a system header, a library, a file in a SYSTEM dir or in the build dir); an edit changes a script outside the workspace folders, or a C++ file that is outside the workspace folders and not an allowed header of the host; an edit changes a generated file `<stem>.g<N>.cpp` of `hotc --compile-commands`; `clangd` gives an error or no edit; a changed range is not the old name (the server checks the text of each range in the generated code and in each C++ file); a changed range has no clear place in the script; a script line has more than one word with the name and the k-th use rule does not give the place (for example `int count = c.count;` with a type or a local of the same name that the rule cannot match); or two edits of one generated line map to one place in a script line with more than one such word. The declaration or definition place is not added when it overlaps an edit of `clangd`. The server also refuses in these cases: `clangd` gives an error or no answer for one script; the time is over for one script; a closed script does not translate; a script outside the workspace folders has a use of the symbol. The server never gives a partial edit. The calls of one rename share a time budget of 10 seconds. A script that uses an other symbol with the old name in a C++ file that the first answer does not change (an unrelated class with the same method name, an override, a template specialization) also gives a refusal. A rename probe at the word could not tell these cases apart: for an override, a diamond and a template specialization the probe gives an edit that is a partial rename. So the refusal stays. The server starts `clangd` with `--rename-file-limit=0`, so `clangd` does not refuse a rename that changes more than 50 files.
  - `textDocument/prepareRename` agrees with rename: it gives the range of the word for a renameable symbol (script or native), else an error with the same message. For a native symbol it runs the check of rename with a probe name. The check covers the start script and the declaration only, not the other scripts.
  - A rename of a script member checks the members of the native base classes of its class family. The server asks the workspace index of `clangd` for `<Base>::<new name>`. A hit refuses the rename: `cannot rename to X: the native class B has a member X`. The check fails closed: when `clangd` is not ready, gives an error or does not answer in time, the rename is refused with `the members of the native base B are unknown`. A definitive empty answer lets the rename go on. A class family without a native base has no such check. Limits: only the direct native base of a script class is checked (the server does not walk to the base of a native base), and the index of `clangd` must know the header of the native base.
  - A rename of a class with an attribute, or of a field or a property with a tag other than `alias`, gives the edits and one `window/showMessage` warning. A host can use the name as a string, and rename does not change strings in assets or code.
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

## Markup documents with script blocks (HOT-65, step S1)

A host JSON file can have the key `"embedded"`: `[{"suffix": ".rml", "tag": "script", "prefixFile": "<path>"}]`.
A relative path is relative to `sourceDir`. hot-ls then serves the files with this suffix.
hot-ls reads the prefix file at each analysis. It removes one final line end.
It rejects a prefix with a line end inside or a non-ASCII byte. It logs one line and publishes an empty list.
The next change of the document reads the prefix file again. A fixed file works then.
The unit text is `mask_embedded_scripts(raw, tag, &found, prefix)`: the same text as at run time.
The server converts the columns on the raw text, so the ranges are in the markup coordinates.
A document without a `<script>` block gets an empty diagnostics list.
In step S4 `textDocument/hover` and `textDocument/definition` work for a position inside a `<script>` block.
A position in the markup or the prefix answers null. A definition in the prefix text also answers null.
A definition in a `.hot` file is a normal location. A name in the block gives a range in the markup coordinates.
All other requests answer null.
A document that opens before a host has the rule is not served. Open it again after the host file changes.

## Workspace index

The server has a background index of the closed `.hot` scripts of the workspace (ticket HOT-34). It is on by default.

Settings:

- The index is on by default. `--no-background-index` turns it off. The server then analyzes a closed script inline when a request needs it, and uses the shard index as before.
- `--index-threads N` sets the number of worker threads. The default (0) is half of the cores, at least 1. The workers run at low priority.
- `--index-dir dir` sets the directory of the saved shards (see above). The index uses the shards to avoid a new analysis after a restart.
- The index needs a workspace folder. Without a folder, the server does not start it.

What the index covers. Each script has one entry. An entry holds the text, the table of the resolved names, the classes and members, the identifier words and the words that no resolved name claims. The server uses the entries for:

- script references and rename (also `prepareRename`) of a script symbol: the candidate files, the places and the conflict check;
- workspace symbols of the closed scripts;
- native references and native rename: a closed script is skipped when no word of the entry can be the native name;
- the import graph: an entry keeps the stamp of each script in the import closure of its script.

Invariants:

- The index describes the files on disk. An open document never uses an entry: it uses its own text and its own analysis.
- An entry is valid only when the size and the mtime of the script and of each script in its import closure are the same as at the analysis.
- Rename and references also compare the disk text with the text of the entry. A difference drops the entry, and the request reads the disk. The rename compares the text of each closed file with an edit again before it builds the edits. A change during the rename gives the error `... changed on disk during the rename`. The server never sends an edit for a text that it did not read.
- A request asks the index only for the scripts that it needs. A script without the searched word on disk gets no lookup, so a cold index does not delay a request for files that cannot contribute.

Costs:

- CPU: each script is analyzed once in the background, at the start and after each change of the file or of an import. The workers use `--index-threads` threads at low priority.
- Memory: one entry for each script. In the benchmark workspace the server uses about 100 MB more than without the index for 311 scripts, and about 135 MB more for 1561 scripts.
- Disk: the shards of `.cache/hot/index/` (see above).

Limits that the design accepts:

- A path that fails to read or to analyze is tried again at each request. It has no negative cache, so a broken script can add time to each request that needs it.
- The server sees a new user prelude (a file with `PRELUDE_SUFFIX`) only through a watched-files event. A prelude that appears with no event is not used until the next event.
- The server reads a prelude file twice: once for the index job and once for the document analysis.

Measured times: see "Index benchmark" in the Benchmark section.

### Native answer memo (HOT-35)

The scan for the references of a native symbol asks `clangd` for the declaration and the definition at each word of each closed script. This is slow. The shard of a closed script now keeps these answers.

- The shard (format version 5) has the key `n`. It holds the targets of each asked offset and one stamp (size and mtime) for each target file. An empty list is an answer too.
- `clangd` stays the source of the truth. The server records only what `clangd` answered.
- The scan uses the memo when each asked offset has an entry and no stamp changed. It then makes no analysis and no temporary document. The scan compares the stored targets with the target of the symbol at the cursor, which `clangd` gives each time.
- A changed target file drops the memo of the script. A new script digest also drops it. The digest covers the script text, the imports and the translator version.
- The scan stores nothing for a script with a failed ask or a missed wait. A scan that the time budget cuts stores nothing for the scripts that it did not reach.
- `hot/indexStats` shows `nativeMemoHits` and `nativeMemoMisses`. A miss is one script that the scan asked `clangd` for (one temporary document).
- An index job can write the table of the same script again. This can erase the stored answers on disk. The scan then asks `clangd` again. It stores the answers again.
- An empty answer has no target file, so no target stamp covers it. The shard also keeps a guard of the script: the include names of the shell header, the stamp of these headers (time and size of each header, also the nested ones), and a host key. The host key is a hash of the include dirs, the flags, the resource dir, the standard, the `clangd` program and the compile commands dir. The memo is valid only when the header stamp and the host key are the same.
- A header that a script reaches only through `#include MACRO` is not tracked by the stamp.
- A `.clangd` config file is not part of the host key.
- The memo guard is computed before the asks. Old answers are dropped when the guard changed during the analyze of the script.
- A script with no shell header, or with an include graph that is too large for the stamp, gets no memo.
- An answer of `clangd` with an `error` is no answer. The scan stores nothing for that script.
- The analysis of a closed script reads its imports from disk, as the digest does. An open unsaved import buffer changes neither of them.

Residual risk: the memo checks only the target files. A change in a header that is not a stored target can change the overload resolution of a call. The server does not see this change until the script digest changes.

## Threads, cancel and debounce

The server keeps one state thread. This thread owns all state: the documents, the table cache, the index and the `clangd` child. The state has no locks for this reason.

A reader thread reads and parses the framed messages from the input. It pushes them on a queue with a mutex. The state thread takes them in order and handles them. The end of the input or `exit` ends the run. The state thread handles the messages that are in the queue first, so no reply is lost.

The reader thread handles `$/cancelRequest` at once, before the queue. It records the id of the request in a set of cancelled ids. Only the id of a request that is queued or runs counts. The set has at most 1024 ids. The state thread removes the id when the reply is sent.

A request in the queue with a cancelled id gets the error code -32800 (`RequestCancelled`), and the handler does not run. A long handler checks the cancel flag in its loops: the scan of the other scripts for references, the places of a rename, and the scan of the rename. When the flag is set, the handler stops and the server sends -32800. The server sends no partial result. The handler does not serve other requests while it runs.

The server joins a series of `didChange` messages for one URI. The state thread takes a `didChange` and then takes the next `didChange` messages of the same URI from the queue. It keeps only the last text, because the sync is full. The analysis then runs once. The server does not reorder other messages and uses no timer.

The tests are in `tests/integration/ls/` (`LanguageServer.*`). They run the server on a thread with an LSP client over pipes. The `clangd` tests are skipped when `clangd` is missing. Set `HOT_LS_CLANGD_LOG=file` to get the `clangd` log of a test.

## The generated unit for clangd (split shell and lite document)

The translator can write a marker line `// @hot-shell-end` in the generated C++ (`TranslateOptions::split_shell`, only the language server sets it). The text before the marker has the imported blocks, the imported function bodies and the module variable accessors. It does not depend on the own bodies.

The server writes this text to a header file. It gives clangd the unit as `#include "<header>"`, then empty lines, then the own part. The unit keeps the lines and columns of the full text, so no position mapping is needed. A place in the header maps to the same place in the full text (`ClangdProcess::set_uri_alias`). The header name has the hash of its content. Each server has its own directory `<cache>/hot-ls/<pid>-<n>` (mode 0700, `XDG_CACHE_HOME` or `~/.cache`). The server removes it at exit. At start it removes the directories of dead processes.

The translator also writes `// @hot-body-begin` and `// @hot-body-end` around each own function body. A body with a deduced return type (`auto`) has `// @hot-body-begin keep`: the server never makes it empty. For hover, definition and completion in a body, the server uses a second document (the path of the unit plus `.lite.cpp`). This document has the bodies of the other own functions as empty lines. The diagnostics, the references, the rename and the other requests use the full document. The diagnostics of the lite document never reach the client. The server keeps at most two lite documents open. A place outside a body uses the full document too.

An edit of the shell (an import edit, a new class) changes the header. The header has one stable name for each document. The server writes the new text to the same file (an atomic write) and sends only `didChange` for the unit. The include line of the unit has a stamp comment, so the text of the unit changes. clangd finds the old preamble out of date. It builds a new preamble in the background and answers from the old preamble meanwhile. The first completion after such an edit does not wait for the new preamble. On the BiteSizedWar script GameCore.hot with a new class for each edit, the median latency of the first completion falls from about 3700 ms to about 1900 ms (plain name) and 980 ms (after a dot). The warm values are 1270 ms and 900 ms.

For at least 5 seconds after a shell edit (the hold, `kShellHoldMs`) the replies of the completion have `isIncomplete: true`. The server does not store them in a cache. The client must ask again to get the new names. VS Code asks again only after more typing. A user who stops typing sees an incomplete list until the next key. After the hold, the server sends one `documentSymbol` request to clangd. The request returns when the new preamble is ready. Until it returns in time, the replies stay incomplete and the server does not store them (this also holds for the cache of the `this` members). Then the server stores the replies as before. Limit: during the hold a hover or a definition can use the old header. A removed declaration can still show a hover, and a definition location in the header can be a line of the old text. The answer is right after the new preamble is ready. The first open of a document, a change of the host and a restart of clangd still wait for the preamble. If the write to the stable file fails (a file lock on Windows), the server uses a name with the hash of the content and opens the document again. `HOT_LS_STABLE_SHELL=0` turns the stable name off.

An error in the body of an imported script is in the header. clangd reports it at the `#include` line of the unit, and the server does not show it for the importer (the script itself shows it). The full unit has a second error for the importer that comes from the first one (`No matching function for call to 'forward'` at the line of a class). With the split this second error is gone. The other diagnostics are the same. A test checks this.

The corpus check `tools/lsbench/lite_corpus.py` asks hover and completion at places in all scripts of a workspace with `HOT_LS_LITE_CHECK=1` and prints each difference of the lite document and the full document.

Result on the BiteSizedWar scripts (182 scripts, 636 places, hover and completion): 1506 lite hits, 256 fallbacks and 8 differences (4 places, each one twice). All 8 are hover texts of a field of `Vector3`, where the full document adds a body text (`{ }`) to the declaration. Because the differences are not zero, the lite document stays off by default.

Environment variables:

- `HOT_LS_SPLIT_SHELL=0` turns the header off. clangd then gets one unit. The split is on by default.
- `HOT_LS_STABLE_SHELL=0|1` turns the stable name of the header off or on. The default is 1.
- `HOT_LS_LITE_DOC=1` turns the lite document on. It is off by default.
- `HOT_LS_IMPORT_CACHE=0|1` turns the import cache of the translator off or on. The variable wins over the setting `importCache` in `initializationOptions`. The setting wins over the arguments `--import-cache` and `--no-import-cache`. The cache is on by default. On GameCore.hot it cuts the median of `analyzeMs` from about 395 ms to about 330 ms. It cuts `importParseMs` from 69 ms to 0.
- `HOT_LS_THIS_CACHE=0|1` turns the cache of the `this` members off or on. The default is 1.
  - A completion of a plain name in a method asks clangd for the native members of the base classes. With the cache, the server asks once with an empty prefix. It filters the answer by the prefix.
  - The key holds the document, the head of the class, the host options and the hash of the shell header. It holds also a stamp: the time and the size of each header that the shell header includes, and of the headers that these include. The server checks the stamp at each request. A header edit on disk needs no event of the client.
  - A watched header (`.h`, `.hpp`, `.hh`, `.inl`) goes to clangd too, so it builds a new preamble. The VS Code extension watches these files. The cache stores no answer for 5 s after such an event, because clangd can answer from the old preamble in that time. A preamble that takes longer than 5 s can still leave an old list in the cache. The stamp does not see a header that only an `#include` with a macro name or `#include_next` reaches.
  - A new clangd, a host change, a file that is not a script and `didChangeConfiguration` drop the cache.
  - `hot/indexStats` has the counts `thisAnalyzes` and `thisHits`.
  - On a miss, the analysis of the `this` text runs at the same time as the first probe. The clangd calls stay in order, because they patch the one text of the document.
- `HOT_LS_SPECULATE=0|1` turns the speculative clangd call of a completion off or on. The default is 1.
  - A completion that misses the caches analyzes a patched text of the script. The call to clangd waits for this analysis. With the switch on, the server keeps the generated text of the last patched text (the memo). A new request puts the new word in the memo text and sends it to clangd at once. The call runs while the analysis runs.
  - The server uses the answer only when the generated text of the analysis equals the memo text with the new word, and the place of the word is the same. Then the items are the same as without the speculation. If the texts differ, the server cancels the call (`$/cancelRequest`), waits for the response of clangd, drops the answer and runs the normal path. The calls to clangd stay in order, one at a time for a document.
  - The server makes no speculation during the hold of a shell header change, while a shell header is unconfirmed, for a cold document (no preamble or no first completion yet), or when the document waits for the deferred update.
  - `hot/indexStats` shows `holeSpecTried`, `holeSpecEqual` and `holeSpecPrefixEqual` (the memo text against the real text of the first probe), `holeSpecUsed` (answers used) and `holeSpecDropped` (answers dropped).
- `HOT_LS_DEFER_UPDATE=0|1` turns the deferred live update off or on. The default is 1.
  - With 0, a `didChange` runs the analysis of the script at once, before the next message.
  - With 1, a `didChange` stores the text and sets a flag on the document. The analysis runs later. This sends the text to clangd and publishes the diagnostics. The epoch grows at the `didChange`, so a `references` request of the slow worker that began before the edit still gets `-32801`.
  - The analysis runs before each message, except these: `didChange`, a completion that can use the old facts, `$/` notifications, `shutdown` and `exit`. It runs also when no message comes for 5 ms (`HOT_LS_FLUSH_IDLE_MS` changes this time, for tests). So the diagnostics follow the typing as before.
  - A completion reads the facts of the last analysis for the classes: the class at the cursor, its members and its bases. It skips the analysis only when `class_facts_hold` is true (`src/ls/script_text.cpp`). The old text and the new text must differ in one line, inside the body of a method, a constructor or a destructor. The changed bytes must be plain: words, spaces, `( ) [ ]`, `.` `,` `:` and the operator signs. No brace, `;`, quote, comment sign, `#` or newline. A byte next to the change must not be `/`, `*`, a quote, `\` or `#`. Other edits (a new line, a member, a class header, a brace, a `;`) run the analysis first.
  - The script facts of the completion that are not class facts (locals, fields, statics, imports) come from the new text, not from the facts.
  - The probe texts of a completion are always analyzed from the new text. After the probes the server puts the old generated text back in clangd. The analysis that follows sends the new one.
  - A `didClose` and every other request run the analysis first.
  - `hot/indexStats` has the counts `updateRuns` and `dirtyDocs`. The parameter `noFlush` keeps the dirty documents dirty (for tests).
- `HOT_LS_LITE_CHECK=1` asks the full document too for hover, completion and definition. The server logs each difference. `hot/indexStats` has the counts `liteHits`, `liteFallback` and `liteMismatch`.
- `HOT_LS_SHELL_DIR=<dir>` sets the directory of the header files (the tests use it).
- `HOT_LS_TIMING=<file>` appends the phases of a completion to the file. It writes one `PHASE` line for each phase: `completion total`, `script facts`, `full sync`, `full clangd call`, `restore`, `post items` and `live update`. It writes one `PROBE` line for each probe text, with the passes, the parses and the translate times.
- `HOT_LS_CLANGD_J=<n>` sets the `-j=` value of `clangd`. The default is 2.

## Import cache

The translator session of the server keeps the parsed imports in memory (`TranslateOptions::import_cache`, 256 entries, least recently used first). All analyses of the server (open scripts, probes and the background index) share one session. The key of an entry is the file, the unit, the base position, the array template, the std namespace and the hash of the text. An edit of an imported script changes the hash, so that script is parsed again. The other imports of the same importer hit the cache. `hot/indexStats` has the counters `importCacheHits` and `importCacheMisses` in `lastUpdate`. A test checks that the results with the cache are the same as without it.

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

## Benchmark

The driver `tools/lsbench/lsbench.py` measures `hot-ls` through the real protocol. It uses only the Python standard library. Run it with `python3 -I`.

```sh
python3 -I tools/lsbench/lsbench.py --hot-ls cmake-build/ls-rel/bin/RelWithDebInfo/hot-ls --preset bsw --json out.json
```

- Build a Release server first: `cmake --preset linux-clang-both -B cmake-build/ls-rel`, then `cmake --build cmake-build/ls-rel --config RelWithDebInfo --target hot-ls`.
- The preset `bsw` sets the workspace `BiteSizedWar`, the class `WeaponHolder` and the native class `Log`. Use `--root`, `--class-file`, `--class-pos`, `--hover-pos`, `--native-file` and `--native-pos` for an other workspace. Lines and columns start at 1.
- `--only NAME` runs one scenario. The names are `analyze`, `refs`, `native`, `typing`, `hover` and `cancel`. The scenarios run one after the other. Each scenario starts a new server with an empty index directory.
- Each line has the form `name: ms` and the counters of the request `hot/indexStats` (`translations`, `translateMs`, `loaded`, `loadMs`, `analyzed`, `tableHits`, `tableAnalyses`). `--json` writes the same rows to a file. The file also holds the load of the machine.
- Run on a quiet machine. The baseline below ran with a load average of 25 on 32 CPUs. The numbers vary a lot between runs.
- `tools/lsbench/symbolinfo_probe.py` checks that `clangd` gives a non-empty `usr` in `textDocument/symbolInfo`, also in generated files.

Baseline of the server before the redesign (BiteSizedWar, 88 scripts, times in ms, second run). The files are `tools/lsbench/baseline-release.json` and `baseline-debug.json`. The Debug server does not have `loadMs`.

| Scenario | Release | Debug | Target (Release) |
|---|---|---|---|
| analyze: median time to the first diagnostics of a script (10 scripts) | 1480 | 1343 | none |
| script-class references, cold | 34567 | 61194 | none |
| script-class references, warm, same session | 96 | 11547 (1 new analysis) | < 200 |
| script-class references, warm, new process | 6916 | 7690 | none |
| native references (`Log`), cold | 16785 | 20781 | none |
| native references, warm | 10089 | 10246 | < 500 |
| typing: last change to diagnostics, median (p95) | 29 (66) | 42 (108) | p95 < 500 |
| hover reply while references run | 54518 | 55261 | < 100 |
| reply after `$/cancelRequest` | 49580 | 38017 | < 100 |
| index ready (all scripts) | not measured | not measured | < 30 s |

- The server answers the requests one after the other. So a hover waits for the references request. The hover reply is null in both runs.
- The server ignores `$/cancelRequest`. The cancel time is the time of the whole request.
- The native scan of the Release run stopped at the time budget (5 of 179 scripts) and showed a `window/showMessage`. The number of places differs between runs for this reason.
- The Debug server is not slower than the Release server here. The time is in the translator and in `clangd`, not in the code of the server.

### Index benchmark

The tool `tools/lsbench/index_bench.py` measures the server over a generated workspace. It does not use files of a real project. The workspace has 10 groups of 30 scripts (units), a library script for each group, a shared script `common/base.hot` and the native header `Actor.h` of the test host. Each unit imports the base, its library and `Actor.h`. It has a class, a field and two methods. The tool runs the server with the index on and with `--no-background-index`. It measures the time until the index is ready, one cold request of references and rename (the first request after the start), and 30 warm samples of each request kind.

```sh
python3 -I tools/lsbench/index_bench.py --hot-ls cmake-build/linux-clang-both/bin/RelWithDebInfo/hot-ls --json out.json
```

- `--edit-bench` measures typing: 12 one-character edits of `--edit-file` (default `g3/unit90.hot`). It prints the time to the diagnostics, to a hover after them and to a hover sent right after the `didChange`. `--edit-root` and `--edit-cdb` select an other workspace.
- `--edit-bench --edit-kinds` measures the time from `didChange` to the diagnostics for 4 kinds of edit, 12 runs each: a comment (a), a literal (b), an unknown name (c, with the fix back) and a new native member (d). `--edit-root`, `--edit-cdb` and `--edit-file` select a real workspace. The edit file needs a body line of the form `name = number;`. `EDIT_KINDS=b` runs one kind. `EDIT_REQ=1` also times a hover after the diagnostics and a completion after a second `didChange`. The tool prints the median and the p95 of each time and the phases of the server.
- `--groups N` makes a workspace of N groups. `--mode on|off|both` and `--samples N` choose the runs.
- Each warm request runs after one warm-up request of the same kind. `completion` has no warm-up: its first sample is the first completion of the document (about 420 ms, the first translation and the answer of `clangd`). The other samples are under 1 ms.
- The numbers below are from a machine with other jobs (load average 9 to 15 on 32 CPUs, marked LOADED). Release build (RelWithDebInfo). Times in ms.

311 scripts. Each row is median (p95).

| Request | Index on | Index off |
|---|---|---|
| index ready | 3252 | not applicable |
| cold references of a library class (31 files) | 1352 | 1666 |
| cold rename of a library class (31 files) | 1353 | 1964 |
| hover (script member) | 0.8 (1.4) | 0.7 (0.9) |
| hover (native member) | 0.5 (0.7) | 0.5 (0.5) |
| definition | 0.1 (0.1) | 0.0 (0.1) |
| references, 30 importers | 3.9 (5.9) | 3.5 (4.3) |
| references, 300 importers | 21.6 (24.9) | 18.8 (31.0) |
| prepareRename, 300 importers | 25.4 (27.5) | 28.5 (29.6) |
| rename, 30 importers | 6.3 (6.6) | 6.8 (6.9) |
| rename, 300 importers | 35.8 (38.4) | 38.3 (43.7) |
| workspace symbols (query with 555 results) | 14.9 (19.9) | 31.7 (34.8) |
| diagnostics after an edit | 2.8 (3.3) | 2.8 (2.9) |
| server RSS after the index is ready | 124.6 MB | 27.1 MB |

- With the index off, the warm numbers come from the in-memory table cache of the same session. The first references request of a 300-file symbol took 3033 ms (analysis of the closed scripts). With the index on, the first request is also fast: the worker has analyzed the scripts.
- 1561 scripts (`--groups 50`, index on, load 9): index ready in 16.7 s, server RSS 162 MB, references with 1500 importers 118 ms, rename with 1500 importers 188 ms, workspace symbols (3055 results) 81 ms.
- All requests are far below the target of 500 ms with the index. Warm script references (30 files) take 4 ms, the target is 200 ms. The hover time with the index on is the same as with the index off.


### Edit benchmark: phases and the import origin

Method: the tool opens `GameCore.hot` of BiteSizedWar (2275 lines, 82 imported classes and 67 own classes) with the compile commands of the project. Then it changes the text in memory and waits for the diagnostics. The phases come from `hot/indexStats`, the field `lastUpdate`. Times in ms, median of 12 runs, kind b (literal).

| Phase | Before | After |
|---|---|---|
| front (preludes, imports, type model, libclang setup) | 166 | 163 |
| check (class loop) | 400 | 170 |
| emit | 12 | 13 |
| total, didChange to diagnostics | 647 | 411 to 433 (p95 up to 436, one run of d 706) |
| class loop, imported classes (82) | 250 | 66 |
| class loop, own classes (67) | 129 | 93 |

What changed in the class loop. The parameter analysis context did not build its maps of all classes again for each class: the loop keeps the field list of each class and one map of the script functions. An imported static function builds no lowering context, because it has no body. `default_arg` copies the context only when it lowers a default value. An imported class builds no state struct text, and a field initializer builds its context only when it lowers an initializer.

Front, ms: preludes 9, import loop 136 (read 14, structure parse 67, view 6, the rest of the loop about 49), questions for libclang 8, `using namespace` 8, libclang setup 29. The next work for the front is in the import loop.

The import origin. Each imported script gets a position range of its own in the position space of the translation. The first import starts at a fixed power of two (1 MiB, or the next power of two above the main script). So an edit of the main script moves no position of an import until the main script passes the power of two. A script of 2 GiB or more is an error. The import text has no blank padding: the parser gets an `origin` (`ParseInput::origin`, the `origin` argument of `parse_script`). A position in an error or in a node is the origin plus the index in the text. The `LineMap` has the origin of each import file. The tests `ImportOrigin.*` check that the refs, the source map and the diagnostics of an import do not depend on the size of the main script.

The fields of `lastUpdate` in `hot/indexStats`: `frontMs`, `lowerMs`, `checkMs`, `emitMs`, `parseMs`, `parseWaitMs`, `parses`, `passes`, `postWorkMs`, `analyzeMs`, `seq`, `importReadMs`, `importParseMs`, `importViewMs`, `foreignClassMs`, `foreignClasses`, `ownClassMs`, `ownClasses`, `checkClassMs`, `frontStep0Ms` to `frontStep12Ms` (the sections of the front phase), `foreignStep<k>Ms` and `ownStep<k>Ms` (the steps of the class loop for imported and own classes; 0 to 7 are the main steps, the others are inner steps used for measurement).

Hover and completion after an edit (BiteSizedWar `GameCore.hot`, load average 3 at the start and 8 to 10 later, so these numbers are rough): a hover just after the diagnostics took 17 ms after a literal edit and about 3036 ms after the other kinds (the native budget of clangd, `kNativeBudgetMs`, ends the wait). A completion after a `didChange` took about 2500 to 3300 ms.
