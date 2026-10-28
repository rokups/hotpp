# Task list: full implementation of spp-design.md

Project name: S++. Script extension: `.spp`. Namespace: `spp`. Compiler: `sppc`.

Status: [ ] open, [x] done. One slice is one commit.

- [x] S0 Rename (run after S1): project name S++; `.script` to `.spp`; namespace `script` to `spp` (`script::` in runtime and emitted code); tool `spc` becomes `sppc` (merge with the precompile tool: `sppc` translates, `--emit-cpp` prints C++, default builds a `.so`); `SPP_` macros, docs, README, design doc, tests, examples, golden files, editor ids. Search all of the repo for `script`, `spc`, `scriptlang` names. Done: the translator namespace is `sppc` (no clash with `spp`). Generic names such as `script_name`, `ScriptSource` and `SppScripts.cmake` stay.
- [x] S1 Housekeeping: update HANDOFF.md, fix spurious attribute error, cache miss cases, TSan run, lldb F3 check.
- [x] S2 Translator checks: 7.10.7 stub checks, borrowed-pointer and ref rules, ERROR rows, prelude details, `class *;` rules. Done. Borrowed-storage findings are warnings until the examples migrate (S5); see HANDOFF.md.
- [x] S3 `onReload(OldState)`, `OldState`, `convert` hook, live type change, rename warning, restart request. Done.
- [x] S4 Script constructors, `new` of a script class. Done. The limits are in `HANDOFF.md` (Known limits).
- [ ] S5 Value/handle slice 2: parameter analysis, `var r = ref`, member access on script handles. S5a done: `[value]` script classes, plain script classes, script classes as handles.
- [x] S5b Done. Open: the `var r = ref` check, the type of a `var` from a native call, and a reload check for the `enable_shared_from_this` layout. Parameter analysis (const T& vs T, store takes a copy), plain trivially-copyable native type is a value (7.10.2 rule 4), type-directed member access on script handles, `return this` handle, strict borrowed by default and migrate examples, order-independent class use (a cycle is an error).
- [x] S6 Script inheritance, script `virtual`/`override`, `is` and `as`. S6a done. The limits are in `HANDOFF.md` (Known limits). Open: a parent in another script, multiple script inheritance.
- [x] S6b Done: inherited properties, 3-level family test with a reload, test for loader rejection of a parent change, `var r = ref` tests. Open: the `super(...)` arguments of a child run two times (see HANDOFF.md).
- [x] S7 Done (`${expr}` and `${expr:spec}`, `--format=std|fmt`, see DECISIONS item 10). String interpolation: lower `${..}` to `std::format` (C++20) or `fmt::format`. The fmt library may be user-supplied: pick it by a translator option and a header check, no hard dependency.
- [ ] S8 Module variables, enums, structs, native interface bases.
- [ ] S9 Runtime polish: removed-class handler, `script_stats`, entry-count debug check, watcher and command channel.
- [ ] S10 M8: module that never unloads for generation 1.
- [ ] S11 Diagnostic rewriter, LLDB formatters.
- [ ] S12 Shipping mode (M7).
- [ ] S13 Tolerant parse and `source_map` (M10).
- [ ] S14 VS Code extension (M11), `sppc --compile-commands`.
- [ ] S15 CPM fetch test.

Not testable here: Windows, macOS, a real engine, the clangd fork (M12 to M14). See DECISIONS.md.
