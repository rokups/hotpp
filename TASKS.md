# Task list: full implementation of spp-design.md

Project name: S++. Script extension: `.spp`. Namespace: `spp`. Compiler: `sppc`.

Status: [ ] open, [x] done. One slice is one commit.

- [x] S0 Rename (run after S1): project name S++; `.script` to `.spp`; namespace `script` to `spp` (`script::` in runtime and emitted code); tool `spc` becomes `sppc` (merge with the precompile tool: `sppc` translates, `--emit-cpp` prints C++, default builds a `.so`); `SPP_` macros, docs, README, design doc, tests, examples, golden files, editor ids. Search all of the repo for `script`, `spc`, `scriptlang` names. Done: the translator namespace is `sppc` (no clash with `spp`). Generic names such as `script_name`, `ScriptSource` and `SppScripts.cmake` stay.
- [x] S1 Housekeeping: update HANDOFF.md, fix spurious attribute error, cache miss cases, TSan run, lldb F3 check.
- [x] S2 Translator checks: 7.10.7 stub checks, borrowed-pointer and ref rules, ERROR rows, prelude details, `class *;` rules. Done. Borrowed-storage findings are warnings until the examples migrate (S5); see HANDOFF.md.
- [x] S3 `onReload(OldState)`, `OldState`, `convert` hook, live type change, rename warning, restart request. Done.
- [x] S4 Script constructors, `new` of a script class. Done. The limits are in `HANDOFF.md` (Known limits).
- [x] S5 Value/handle slice 2 (done in S5a and S5b): parameter analysis, `var r = ref`, member access on script handles. S5a done: `[value]` script classes, plain script classes, script classes as handles.
- [x] S5b Done. Open: the `var r = ref` check, the type of a `var` from a native call, and a reload check for the `enable_shared_from_this` layout. Parameter analysis (const T& vs T, store takes a copy), plain trivially-copyable native type is a value (7.10.2 rule 4), type-directed member access on script handles, `return this` handle, strict borrowed by default and migrate examples, order-independent class use (a cycle is an error).
- [x] S6 Script inheritance, script `virtual`/`override`, `is` and `as`. S6a done. The limits are in `HANDOFF.md` (Known limits). Open: a parent in another script, multiple script inheritance.
- [x] S6b Done: inherited properties, 3-level family test with a reload, test for loader rejection of a parent change, `var r = ref` tests. Open: the `super(...)` arguments of a child run two times (see HANDOFF.md).
- [x] S7 Done (`${expr}` and `${expr:spec}`, `--format=std|fmt`, see DECISIONS item 10). String interpolation: lower `${..}` to `std::format` (C++20) or `fmt::format`. The fmt library may be user-supplied: pick it by a translator option and a header check, no hard dependency.
- [x] S8a Module variables, enums, structs. Done.
- [x] S8b Done: native interface bases (multiple native bases, slot conflicts, reload check of the base list), README and HANDOFF notes for S8a. Open: interface bases on a child of a script class.
  - S8a done: module variables, enums, structs (see DECISIONS.md 11). Open: native interface bases (S8b), a script convert hook for a module variable.
- [x] S9 Runtime polish: removed-class handler, `script_stats`, entry-count debug check, watcher and command channel.
- [x] S10 M8 Done: core module (`Module::make_core`), generation 1 unloads, `kMayHoldCode` unload block, stress tests in `tests/integration/reload/core_module_tests.cpp`. Open: see HANDOFF.md, Not done item 2.
- [x] S11 Diagnostic rewriter, LLDB formatters.
- [x] S12 Done (M7): `sppc --ship`, `spp_add_shipped_scripts`, `spp::ShippedRegistry` (`include/spp/shipped.h`), target `spp_ship` (no loader), example `ship_demo`, tests in `tests/integration/ship/`. Open: see the open items in README "Shipping mode" and HANDOFF.md.
- [x] S13 Tolerant parse and `source_map` (M10).
- [x] S14a VS Code extension (M11): grammar, language config, file association, thin client for spp-ls, snapshot test.
- [x] S14b Language server `spp-ls` (M12 to M14) as clangd proxy. `sppc --compile-commands` is part of it.
- [x] S15 CPM fetch test. Done.

The language server is the proxy `spp-ls`, not a clangd fork (DECISIONS.md 13). See "Not done" at the end.

## Second audit (after S15)
- [x] S16 `super(...)` args evaluate once; interface bases on a child of a script class; module variable convert hook and initializer that uses another module variable.
  - Done: one plan per construction (`new`, factory). Interface bases on a child stay an error (design 4.5), with a clearer message. Module variables: ordered initializers and `T convert_<var>(Old old)` at file level (DECISIONS.md 14).
- [x] S17 Runtime: signature change detaches native delegates with a warning; script value type in a native container resets on layout change; first handle use of `this` layout check; code pointers in native memory/function pointer fields.
- [x] S18 Rename across all `.spp` files in spp-ls; VS Code `spp/toScript` command and `${workspaceFolder}`; TSan DeepNesting stack and watcher flake; cache edge case.
- [x] S19 Docs and measures: HANDOFF, TASKS, README status table, design 4.9, 7.3, 10.1, status of F1 to F3, P1 and risk 8. P1 for CLANG_FILES with a PCH is measured (README "Measurements"). A new test, `TemplateBaseTranslation`, pins the limit for a native base that derives from an implicit class template instance.
- [x] S20 Script parent from another script; constructor overloads and ref/out ctor params; `var` from native call type; pattern-stub checks; `p.N` typed property access; deeper parameter analysis.
  - Done: S20a cross-script parents and constructor overloads (DECISIONS.md 16). S20b typing limits (DECISIONS.md 17): `var` types from libclang, typed `p.N`, pattern-stub checks, deeper parameter analysis, `Intrusive` and raw `T*` comparison test.
- [x] S21 Entry-counter blocking sync mode (design 6.3 fallback).

## Not done (cannot be tested here)
- Windows and macOS: the RTTI comparison, the symbol export and the PDB.
- A real engine: the export surface (design risk 2), the registration of shells (risk 3), the safe point hook and the first gameplay class (M4).
- A real lldb session (F3 passes with gdb), the lldb formatters in a live session, and Visual Studio with natvis.
- M9, optional part: the ORC debug-symbol resolver for Windows. The JIT backend itself works on Linux.
- Engine coroutines (version 2): the pin design for a coroutine frame.
- A real VS Code session for the extension in `editors/vscode/`. The grammar snapshot test and the LSP tests pass.

## Open work (can be tested here)
- Done: a native base that derives from an implicit class template instance now gives virtuals (design risk 8). Open: a template that derives from another template, and a partial specialization.
