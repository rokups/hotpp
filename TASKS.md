# Task list: full implementation of scriptlang-design.md

Project name: S++. Script extension: `.spp`. Namespace: `spp`. Compiler: `sppc`.

Status: [ ] open, [x] done. One slice is one commit.

- [ ] S0 Rename (run after S1): project name S++; `.script` to `.spp`; namespace `script` to `spp` (`script::` in runtime and emitted code); tool `spc` becomes `sppc` (merge with the precompile tool: `sppc` translates, `--emit-cpp` prints C++, default builds a `.so`); `SPP_` macros, docs, README, design doc, tests, examples, golden files, editor ids. Search all of the repo for `script`, `spc`, `scriptlang` names.
- [x] S1 Housekeeping: update HANDOFF.md, fix spurious attribute error, cache miss cases, TSan run, lldb F3 check.
- [ ] S2 Translator checks: 7.10.7 stub checks, borrowed-pointer and ref rules, ERROR rows, prelude details, `class *;` rules.
- [ ] S3 `onReload(OldState)`, `OldState`, `convert` hook, live type change, rename warning.
- [ ] S4 Script constructors, `new` of a script class.
- [ ] S5 Value/handle slice 2: default_handle, plain-data-as-value, `[value]` script classes, script classes as handles, parameter analysis, `var r = ref`.
- [ ] S6 Script inheritance, script `virtual`/`override`, `is` and `as`.
- [ ] S7 String interpolation.
- [ ] S8 Module variables, enums, structs, native interface bases.
- [ ] S9 Runtime polish: removed-class handler, `script_stats`, entry-count debug check, watcher and command channel.
- [ ] S10 M8: module that never unloads for generation 1.
- [ ] S11 Diagnostic rewriter, LLDB formatters.
- [ ] S12 Shipping mode (M7).
- [ ] S13 Tolerant parse and `source_map` (M10).
- [ ] S14 VS Code extension (M11), `sppc --compile-commands`.
- [ ] S15 CPM fetch test.

Not testable here: Windows, macOS, a real engine, the clangd fork (M12 to M14). See DECISIONS.md.
