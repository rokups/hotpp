# Handoff: S++ hot-reload scripting prototype

You continue work on `/home/rk/src/spp`. S++ is a hot-reloadable scripting language for C++. The script syntax is type-first, in the style of Vala and C#. The translator (`sppc`) turns it into C++. Clang compiles the C++. The runtime reloads it and keeps the live objects.

## Read first
1. `spp-design.md`: the design. Read sections 4, 5, 7, 8 and 9. Use `grep -n '^## '` and `sed -n`.
2. `README.md`: the backends, the tools, the tests, the status table (M0 to M14).
3. `DECISIONS.md`: each decision with its reason. `TASKS.md`: the slices S0 to S21 (all done).

## Rules from the user
- Write all prose in ASD-STE100 Simplified Technical English. This includes code comments, documents, commit message bodies and subagent prompts. Maximum 20 words per sentence.
- All tests are GoogleTest. Register them with `gtest_discover_tests`.
- Fetch dependencies through CPM.cmake when they are not installed (`CPM_USE_LOCAL_PACKAGES ON`). Libclang and LLVM come from the system.
- Commit messages: short title, terse body. No attribution lines of any kind.
- Subagents: set `model` and `effort` on every Agent call. Use the cheapest suitable model.
- Precompiling a script to a `.so` is not a backend. It is a tool (`sppc`). Project S++, extension `.spp`, C++ namespace `spp`, translator namespace `sppc`.

## State at HEAD
All work is committed, except what `git status` shows. Slices S0 to S21 are done (`TASKS.md`).

| Area | State |
|---|---|
| Runtime | `minengine` with `Actor` and `World`. The script class is a real C++ subclass (shell) of the native base. State lives in a replaceable block. Done. |
| Reload | Same object address. Migration by field name, `[alias(...)]`, type key and nested value classes. Validation has no side effects. A rejected reload keeps the old generation. Done. |
| Thunks, closures | Delegates survive a reload. `spp::Closure` pins a generation. A stale closure logs one stale call and does not run. A signature change detaches a delegate with a warning. Done. |
| Unload | A retired generation unloads when the pin count is zero, a frame passed and no live object uses its code. Generation 1 unloads too (core module). Done. |
| Threads | `World::TickParallel` with a barrier. Stop-the-world commit at the barrier. A second mode, `SyncMode::EntryCounters`, blocks new entries. ASan and TSan are clean. Done. |
| Translator | `sppc_lib` works on strings. It reads the native base with libclang. It emits `#line`. A session cache cuts a warm translate to about 1.3 ms. Tolerant parse, `source_map`, `analyze`. Done. |
| Language | Classes, properties, constructors, inheritance, virtuals, `is`/`as`, enums, structs, module variables, native interface bases, interpolation. Done (syntax below). |
| Backends | `SPP_BACKEND=CLANG_FILES` (default, external `clang++`) or `JIT` (in-process Clang plus ORC). Chosen at build time. Done. |
| Tools | `sppc` (build `.so`, `--emit-cpp`, `--ship`, `--compile-commands`), `bench_backends`, `spp-ls`. Done. |
| Shipping mode | `sppc --ship`, `spp_add_shipped_scripts`, library `spp_ship` with no loader. Done (generation 1 only). |
| Diagnostics, debug | Diagnostic rewriter, `#line`, DWARF script lines, lldb formatters, natvis. gdb check done (F3, see below). lldb and Visual Studio not run. |
| Language server | `spp-ls`: a stdio LSP proxy in front of stock clangd (`src/ls/`, `DECISIONS.md` 13). VS Code extension in `editors/vscode/`. Done. |

Layout:
- `include/spp/` public headers. `src/runtime/` runtime and backends. `src/translator/` translator (`sppc_lib`). `src/translator/parser/` lexy grammars. `src/tools/` the tool `sppc`. `src/ls/` the language server.
- `example/` has `engine`, `scripts`, `imgui`, `ship` and `bench_backends.cpp`. `editors/vscode/` the extension. `tools/` has the lldb and natvis files.
- `tests/unit/translator/` (`parser`, `emit`, `lowering`, `translate`, `types`, `session`, `diagnostics`), `tests/unit/runtime/`, `tests/unit/python/`, `tests/integration/` (`backends`, `closures`, `reload`, `threading`, `ship`, `ls`), `tests/golden/`, `tests/support/`.

## Build and test
- `cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ && ninja -C build && ctest --test-dir build -j8`
- Add `-DSPP_BACKEND=JIT` for the JIT backend. Add `-DCMAKE_CXX_FLAGS=-fsanitize=address` or `=thread` for sanitizers.
- Presets: `cmake --preset linux-clang-files` (or `linux-clang-jit`), then `cmake --build --preset linux-clang-files-dbg`. Add `-asan` or `-tsan` to the name.
- Tests at S19 in `build` (CLANG_FILES): 765 tests (with the 2 new template-base tests), all pass, 2 skipped (`CoreModule.ThousandReloads` needs `SPP_STRESS=1`; one `FileWatcherTests` case skips by design in the inotify mode). The other builds (`build-asan`, `build-jit`, `build-tsan`) were not run in S19. Run them before a release.
- `StatementParseLimits.DeepNesting` can overflow the stack in a TSan build. `FileWatcherTests.SeesWriteAndRenameButNotOtherFiles` can fail under `ctest -j8` load in a TSan build. It passes alone.
- Known workaround: libclang does not find its builtin headers here, so the build passes `-resource-dir` from `clang++ -print-resource-dir`.

Measures (P1, one module after a one-line change, this machine, `-O1`, warm): JIT 808 ms, JIT with PCH 166 ms, CLANG_FILES about 1105 ms, CLANG_FILES with PCH about 350 ms. The criterion is under 2 s. See README "Measurements".

## Syntax summary
The full list is in `README.md` and `spp-design.md` section 7.
- `using "Actor.h";` and `using <vector>;`. A `using "other.spp";` imports a script (cross-script parents).
- `class Name : Base, IFace { }`. The base is optional. `[value] class` and `struct` are inline values.
- Field: `[alias("old")] [public|private|protected] Type name = init;`. The initializer is optional.
- Method: `[modifiers] Ret name(T a) { }`. Modifiers: `override`, `virtual`, `new`, access words. `void` means no return value.
- Constructor `Name(T a) : super(args) { }`. Destructor `~Name() { }`. Hooks: `void onReload(OldState old)` and `T convert_<field>(Old old)`.
- Property: `Type N { get; set; }`, `{ get; }`, `{ get; private set; }` or `{ get { } set { } }`. The setter reads `value`.
- Locals: `Type x = e;`, `var x = e;`, `const Type X = e;`, `var r = ref e;`. Loop: `foreach (Type x in c)`. Also `throw`, `try`, `catch (Type e)`, `catch (...)`, `finally`.
- Expressions: `this`, `super`, `new T(args)`, `ref e`, `out e`, `obj is T`, `obj as T`, arrow lambdas `(T a) => ...`, `Math.Min` as `Math::Min`. `.` is the only scope separator. No brace initialization.
- Strings: `"${expr}"` and `"${expr:spec}"` lower to `std::format` (default) or `fmt::format` (`--format=fmt`). The result is `std::string`. `\${` is a literal.
- Module level: `enum Name [: type] { A, B = 5 }`, `struct Name { }`, module variables `[alias("old")] Type name [= init];`, `T convert_<var>(Old old)`.
- Stubs: `[native, handle("std::shared_ptr<$T>")] class Weapon;`, `[native, value] class std.string;`, `[native, intrusive("AddRef","Release")]`, pattern stubs (`std.vector<*>`), `class *;` with `default_handle("H<$T>")`. Stubs live in the script or in a prelude (`--prelude`, `TranslateOptions::preludes`).
- Builtin types have a fixed width: `bool`, `sbyte`/`int8`, `byte`/`uint8`, `short`/`int16`, `ushort`/`uint16`, `int`/`int32`, `uint`/`uint32`, `long`/`int64`, `ulong`/`uint64`, `nint`/`intptr`, `nuint`/`uintptr`, `float`, `double`, `char`, `wchar`/`char16`. `unsigned`, `signed` and `long long` give an error with a hint.
- Reserved words: `var`, `foreach`, `in`, `using`, `override`, `virtual`, `new`, `public`, `private`, `protected`, `get`, `set`, `finally`, `ref`, `out`, `void`.
- Removed forms give a clear error: `function`, `let`, `import`, `name: Type`, `-> Ret`, `for ... of`, `@was(...)`, prefix `&e`, backtick strings.

## How it works (limits that matter)
Kinds of class:
- A class with a native base is a handle (`new Name(args)` gives the handle of the base stub). A class without a base is a plain script class and a `std::shared_ptr` handle (`spp::PlainObject`). `[value] class` lives inline in the state block, has no base, no destructor, no virtual and no lambda, and uses native types and other `[value]` classes only.
- The order of the classes does not matter. The translator sorts them by use. A cycle is an error.
- A script function used as a value (`var f = G;`) is always an error.

Constructors (S4, S16, S20a):
- The shell and the vtable live in the core module of generation 1. The canonical `ClassRecord` has a type-erased `construct`. A later generation calls it through `spp::construct_shell` and then runs its own constructor body. An unknown key throws `std::runtime_error`; a new key of a later generation gets a new core first (S27).
- A constructor is not in the slot table. A reload does not run it again. A changed body applies to new objects only.
- One plan for each construction: `Name__plan(args)` evaluates each `super(...)` argument of the chain one time. `new` and the factory both call `Name__build`. Each overload gets C++ overloads of the generated functions, keyed by `Name_SuperArgs_k`.
- Limits: one destructor for each class. Two overloads cannot differ only in the pass mode. A `super(...)` argument for a `ref` or `out` parameter of a script parent needs `ref e` or `out e` (S23); the plan keeps the reference and evaluates `e` one time. A `&&` constructor parameter works: the plan reads it as a const reference. A new overload in a later generation can have a new key type (S27): the loader makes a new core module, and its `construct` becomes canonical. The key list is `ClassRecord::construct_keys`. The old key types stay valid only while the new generation keeps the overload. If a constructor body throws, the shell is deleted and the script destructor still runs.

Typed access (S5b, S20b):
- The lowering knows the script class of a local, a parameter, a field and a `new` result. Then `obj.field`, `obj.Method(args)` and `obj.P` use the class directly.
- A `var` from a native call gets its type from libclang. An overloaded or template function gives no type. The native base of the family keeps the name rule: `N` must be a property of the current class.
- Limit of the `.` access: it lowers to `spp::deref(a).b`. A smart pointer, an iterator and `std::optional` have `operator->`, so use `.get()`. A native static data member (`Vec3.Zero.x`) needs a typed local. A null handle in `obj.field` throws `std::runtime_error`.
- `return this` as a handle becomes `spp::handle_from_this<H>(self)`. It needs `std::enable_shared_from_this` in the shell. A reload that adds the first such use (or the first script child) changes the shell layout, and the loader rejects it with a message. Use it in generation 1. A lambda does not convert `this`.
- Parameter analysis (7.10.5): a value or handle parameter is `const T&`, unless the body changes, stores or rebinds it, or the type is small. The analysis follows these calls with a parameter as an argument (S25):
  - a script function of the class and its script parents;
  - a method of a script handle (a parameter, a field or a typed local): the declared method gives the mode and the body. A `virtual` or `override` method counts as a store;
  - a call of a local lambda `var f = (T x) => {...}; f(p)`: the body of the lambda. A callable value that is not such a local lambda counts as a store. A lambda body that names the parameter is a capture and counts as a store;
  - a native function or member function: libclang gives the signature (query `@fn:KEY`, matched at the end of the qualified name, with the bases of a class). `const T&` does not store. `T&`, `T&&` and a non-const pointer change and may store. A by-value parameter and a pointer to const may store. An overload set gives the worst form. A function that libclang does not find, or a receiver with an unknown type, counts as a store (a copy).
  A result of a finished class is kept in a memo, because the lowering changes its bodies.

Inheritance (S6, S20a):
- The shell of a child derives from the shell of the parent. A family has one slot numbering. `virtual` adds a slot (`Owner__f_virt`). `override` fills it. A name that hides a parent function needs `new`. `super.f()` calls the parent directly.
- `is` and `as` walk the `super` records by name. A native type uses `dynamic_cast`. `as` of a raw pointer gives a raw shell pointer: use `var`. A `[value]` class is an error.
- A reload that changes the script parent is rejected. Multiple script inheritance is not supported. Only the root of a family can have native interface bases (design 4.5).
- A script parent in another script (`using "parent.spp";`) merges into the generation of the child. An imported script cannot have module variables or convert hooks. A class name in two scripts is an error. The source map has a file number for each imported script (`SourceSegment::file`, `SourceMap::files()`, `SourceMap::locate()`), so errors in an imported script have the real file, line and column.

Native interface bases (S8b): the first base is primary. Each virtual of each base gets one slot (primary first). The same signature in two bases is an error. `F()` and `F() const` ambiguity, `final`, a duplicate base, a script class as interface, and an interface without a default constructor are errors. A changed list of bases is rejected.

Reload hooks and module state (S3, S8a, S16, S17): `onReload` and `convert_<field>` get no slot. `OldState` shows old fields by old name. A type change never rejects a reload; the field resets or converts. Module variables live in the synthetic `[value]` class `SppModule`. The loader owns one module block and migrates it first. A native container of a `[value]` class resets with a warning when the layout hash changes. A native move failure calls `spp::request_restart`.

Unload limits (core module): the loader does not see a code pointer that native code keeps. `kMayHoldCode` is set for `std::shared_ptr`, a raw function pointer, a member function pointer and `std::function`. S26: the translator marks a trivially copyable native struct that has a function pointer field, so it counts as a holder too. The translator finds the struct by its class or alias name. It does not find a template instance such as `Cb<int>`, and it does not find a type that no script field or local names. Each generation that adds a class or a thunk loads one more copy of its module as a core. The JIT keeps an IR clone of each module until the loader takes it.

Stubs and checks (S2, S20b): order of the rules: exact stub, base stub, pattern stub, plain data (value), `class *`, raw. The checks of 7.10.7 use libclang constexpr probes, cached by the session. A borrowed pointer or `ref` in a field, return, collection or lambda capture is an error (`--no-strict-borrowed` makes it a warning). A `ref x` passed to a native function that keeps it is not detectable (the S25 signature query serves the value/handle parameter form only, not this error: a `T*` parameter is too common to be an error). A custom `spp::is_handle` is checked for `operator->` only. `intrusive` needs non-overloaded Add and Release members.

Translator cache: it hashes a header that changed in the last 2 seconds. It checks paths that hide a header earlier in the include path. The stamp has the inode, the ctime and a content hash. The prelude is not in the cache key.

Native bases (risk 8): libclang describes a plain base class, a base that derives from a plain class, an explicit template specialization and an implicit instance of a class template. The translator reads the members of the pattern and replaces the parameter names with the arguments (`implicit_pattern`, `make_subst`, `subst_text` in `src/translator/sppc_lib.cpp`). This works through a template that derives from another template (`choose_pattern`) and for a partial specialization (`bind_args` deduces the parameters from the argument pattern, for example `U*`). A script class can name an instance as its direct base: `class Foo : Tmpl<int>`. The probe file makes an alias for the instance. Limits: the deduction handles type arguments only. A non-type argument in a partial specialization pattern is not deduced. Constructor parameter types of an instance base keep the parameter names. `tests/unit/translator/translate/template_base_translation_tests.cpp` pins this.

Other limits:
- The parser stores the access words. The translator does not enforce them.
- Nesting limits: 64 nested statements, 15 nested lambdas, 256 operators in one expression.
- Shipping mode: generation 1 only, no slot file. A type error shows in the C++ compile of the generated file. Script type and class names must be unique over all shipped scripts. `onReload` and `convert` never run.
- Language server index (S28): the shard of a closed `.spp` file is made at the first rename or references scan, not at startup. The shard digest covers the file text and the texts of the scripts it imports, found by a scan for `using "x.spp";` in the script dir and the include dirs. A changed import makes the shard stale. The index dir is the first workspace folder only. Code: `src/ls/symbol_index.{h,cpp}`.
- Language server hosts (`DECISIONS.md` 19): `spp_script_host` writes `.cache/spp/hosts/<name>.json`. Code: `src/ls/hosts.{h,cpp}`. Many build dirs share one hosts dir, so the last configure wins. Symlinked workspace paths are not canonicalized in matching. Multi-Config generators are untested. The host defines, options and include dirs are in the side files `<name>.defines`, `<name>.options` and `<name>.includes` (one entry on each line), so all valid paths are safe. The JSON has `sourceDir`; `spp-ls` resolves relative paths with it and removes repeated include dirs. Unsupported: a path with a newline, and a path with `;` (CMake splits it in a list before the function sees it).
- Language server: after a translation error, clangd keeps the last good C++. Positions go through a prefix/suffix edit map (`Doc::set_edit`). A position inside the edit has no mapping. The `#line` fallback takes the column of the same word in the script line.
- Language server: clangd does not publish again when a new version gives the same diagnostics. The server drops diagnostics of an old version, so the diagnostics can be missing after an edit that gives the same C++.
- Language server: two open scripts that import the same script both publish diagnostics for it. The last publish wins. The formatter is a brace-depth indenter, not a full formatter.
- Diagnostic columns: the rewriter uses the source map for the script column. A column in text that the translator writes by itself (for example `spp::deref(`) has no segment: the rewriter then takes the next mapped column of the same line, or 1.

## Parser
The grammar uses lexy. Only the files in `src/translator/parser/` include lexy headers. Keep each grammar in its own file (they compile slowly: about 3 to 6 s each in Debug).
- `script_grammar.cpp` (structure), `expression_grammar.cpp`, `statement_grammar.cpp`. `parse_script()`, `parse_expression()` and `parse_block()` are the entry points. The design is "span, then call": a grammar stores a byte `Span`, and a later call parses it.
- After the parse, `lower_block()` and `lower_expr()` (`lower.cpp`) change script names to C++ names. The `Emitter` (`emit.cpp`) prints the AST with `#line`.
- To add syntax: add the node to `src/translator/ast.h`; add the rule and the value callback in a grammar file; add the printing in `emit.cpp` and the lowering in `lower.cpp`; add tests in `tests/unit/translator/` (`parser`, `emit`, `lowering`). The golden tests in `tests/golden/` must stay byte-identical. Set `SPP_UPDATE_GOLDEN` to write them again when a change is on purpose.

## Experiments (design 8.2)
- R0 to R6: pass (tests in `tests/integration/reload`, `closures`, `threading`; ASan and TSan clean).
- F1: pass for plain bases and explicit specializations. Pass for implicit template instances, templates over templates and partial specializations (type arguments).
- F2: pass. `tests/golden/diagnostics/` has 21 mistakes (`DiagnosticMistakes`).
- F3: partial. gdb stops at `player_v1.spp:11` in a real process, shows `Player__Tick ... at example/scripts/player_v1.spp:13` and steps by script lines. The frame name is from before phase 2. The frame is now the state member `PlayerState_g1::Tick`. This is not run again. lldb, the lldb formatters in a live session, natvis and Visual Studio are not run.
- P1: pass (see Measures).

## Not done
1. Windows and macOS: not tested. The risks are RTTI comparison, symbol export and PDB.
2. A real engine: no spike for export surface (risk 2) or class registration (risk 3). `minengine` is the only host.
3. A real lldb session (F3) and Visual Studio with natvis.
4. M9: the ORC backend exists, but the optional version with a debug-symbol resolver for Windows is not done.
5. Engine coroutines (v2): a coroutine frame needs a pin and a design. Not started.
6. The `shared_ptr` cycles are not detected (`DECISIONS.md` 6).
7. The deduction of non-type arguments in a partial specialization (risk 8).
8. The sanitizer and JIT builds need a new run after S19.

## How to work
- The main session coordinates. Give each step to one `implementer` agent (model `sonnet`) with a self-contained prompt: goal, files to read, rules above, verification commands, budget, stopping condition.
- Run implementers one at a time. They share files and the build directories.
- After each step, rebuild and run `ctest` yourself before you accept the report. Tie every claim to a command output. Say what you did not run.
- Use `engineer` (opus) only if an `implementer` fails twice at the same bounded task.
