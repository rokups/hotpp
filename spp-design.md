# S++: a hot-reloadable scripting language with C++ semantics for a game engine

Design study. Date: 2026-10-27.

## 1. Executive recommendation

**Recommendation.** Build the language as a translator to C++ source, not as a Clang frontend. Keep the hot-reload model in a small runtime library that does not depend on Clang internals.

The recommended architecture has four parts.

1. **Translator.** A custom parser reads the script syntax. It emits C++ source with `#line` directives. The translator resolves only names that the language defines. Clang resolves every native name, overload, conversion and template.
2. **Unmodified toolchain.** The engine's own Clang compiles the generated C++ against a precompiled header of the engine API. The compiler runs as a separate process. The engine does not link LLVM.
3. **Stable shell objects.** Each script class becomes a real C++ subclass of its native base. The shell compiles once per session. It stubs every virtual of the base and dispatches through a per-class table. Script fields live in a separate state block.
4. **Generation loader and migration.** Each reload compiles one shared library. The runtime validates it, migrates state by field name and type at a frame boundary, and swaps the tables. Every generation function starts with a generation guard. Stale code fails safely instead of reading a migrated state block.

**Why this and not the proposed Clang AST frontend.** Clang's Sema has no stable API. A parser that drives Sema inherits the full internal surface of Clang and breaks on each LLVM release. Carbon and Swift pay that cost with dedicated teams. The gain is only better diagnostics. Source translation with `#line` gives the same C++ semantics and the same ABI. It gives normal debugging and a shipping path with no LLVM in the game. The local experiment in section 2.1 confirms that `#line` reaches both DWARF and CodeView line tables.

**Why not a JIT first.** The requirement is hot reload, not JIT. A shared library per generation gives real debug symbols on every platform. It gives crash symbolication and the same code path as a shipping build. ORC adds value only for link latency or for symbol resolution in an engine that cannot export symbols. The loader interface keeps ORC as a later backend.

**Why the shell model and not vtable replacement.** Vtable or vptr replacement on live objects works only with a fixed layout. Every surveyed C++ hot-reload system confirms this limit. The shell separates the stable parts from the changing parts. The address and the vtable stay. The code and the fields change.

**Largest unresolved risk.** References to script code that escape through native APIs. Closures stored by native timers, delegates, and later coroutine frames. The generation guard and pinned closures answer this on paper. Experiment R3 in section 8 tests it.

**First experiment.** R0 in section 8. Hand-write a shell, a state block and a generation module. Compile them into a shared library. Load it into the harness. Tick the object through a native base pointer. It tests the export surface and the toolchain compatibility before any translator work.

**What the system does not promise.** A change to the native base class, to script inheritance, or to native headers does not preserve live objects. Section 5 lists each change with its class: live, reconstruct, restart or error.
## 2. Evidence-based survey

Each claim has a status. Verified: read in a primary source or reproduced locally. Partial: a secondary source. Assumed: not found.

### 2.1 Clang frontend and Sema

| Claim | Status | Source |
|---|---|---|
| The Clang parser drives Sema through `ActOn*` methods. The parser sees only opaque `ExprResult` and `StmtResult` handles. | Verified | [Internals Manual](https://clang.llvm.org/docs/InternalsManual.html) |
| Sema has `ActOnXXX` functions "called directly from the parser" and `BuildXXX` functions that do the analysis | Verified | Same |
| The parser "stopped supporting general Action clients" when C++ support arrived. It "now always talks to the Sema library". | Verified | Same |
| LLVM gives no stability promise for its C++ API. The Developer Policy covers only the C API as "best effort". | Verified | [Developer Policy](https://llvm.org/docs/DeveloperPolicy.html) |
| Carbon calls `Sema::LookupQualifiedName`, `AddOverloadCandidate`, `BestViableFunction`, `CheckTemplateIdType` and `SubstType`. It builds Clang AST nodes directly. It does not drive `ActOn*` from its own parser. It uses Clang CodeGen through `CreateLLVMCodeGen` and `GetAddrOfGlobal`. | Verified | [import.cpp](https://raw.githubusercontent.com/carbon-language/carbon-lang/trunk/toolchain/check/cpp/import.cpp), [overload_resolution.cpp](https://raw.githubusercontent.com/carbon-language/carbon-lang/trunk/toolchain/check/cpp/overload_resolution.cpp), [file_context.cpp](https://raw.githubusercontent.com/carbon-language/carbon-lang/trunk/toolchain/lower/file_context.cpp) |
| Carbon's design doc: interop "must, therefore, operate at the semantic analysis level, not just at the linking (ABI) level" | Verified | [Carbon interop README](https://raw.githubusercontent.com/carbon-language/carbon-lang/trunk/docs/design/interoperability/README.md) |
| Swift's ClangImporter creates a `clang::Parser` with `instance.getSema()`, and calls `InstantiateFunctionDeclaration` and `InstantiateFunctionDefinition`. Swift builds on a fork of llvm-project. | Verified | [ClangImporter.cpp](https://raw.githubusercontent.com/swiftlang/swift/main/lib/ClangImporter/ClangImporter.cpp), [swiftlang/llvm-project](https://github.com/swiftlang/llvm-project) |
| ClangJIT "required modifying Clang's semantic-analysis and code-generation components in non-trivial ways" | Verified | [arXiv 1904.08555](https://arxiv.org/abs/1904.08555) |
| Clang reads `#line` into `PresumedLoc`. `CGDebugInfo` uses the presumed location for `DIFile` and `DILocation`. CodeView line tables use the same metadata. | Verified | [CGDebugInfo.cpp](https://raw.githubusercontent.com/llvm/llvm-project/main/clang/lib/CodeGen/CGDebugInfo.cpp) |
| cppfront emits `#line` directives so that errors and debuggers map to `.cpp2` sources | Verified | [to_cpp1.h](https://raw.githubusercontent.com/hsutter/cppfront/main/source/to_cpp1.h), [options](https://raw.githubusercontent.com/hsutter/cppfront/main/docs/cppfront/options.md) |
| PCH loads lazily. The initial load cost is independent of the PCH size. One PCH per TU, at the start. | Verified | [PCHInternals](https://clang.llvm.org/docs/PCHInternals.html) |

**Local experiment.** A file with `#line 1 "player.spp"` compiled with Clang 23.1.3 on this machine. `llvm-dwarfdump --debug-line` lists `player.spp` in the file table. The CodeView build with `--target=x86_64-pc-windows-msvc -gcodeview` lists `/tmp/lineexp/player.spp` in the file checksums. Both line tables point at the script file. This reproduces the cppfront technique for both debug formats.

**Conclusion for option B.** A custom parser can call Sema. Carbon and Swift prove it. Both pay with a team that tracks LLVM at head, and Swift keeps a fork. No public documentation describes the invariants that a non-Clang parser must keep around `Scope`, `DeclContext`, delayed parsing and template instantiation. This is substantial integration work on internal APIs, not a supported path.

### 2.2 Cling and clang-repl

| Claim | Status | Source |
|---|---|---|
| `clang::Interpreter` has `Parse`, `Execute`, `ParseAndExecute`, `Undo(N)`, `getSymbolAddress`, `LoadDynamicLibrary` | Verified locally in LLVM 23 headers and on main | [Interpreter.h](https://raw.githubusercontent.com/llvm/llvm-project/main/clang/include/clang/Interpreter/Interpreter.h) |
| Each `PartialTranslationUnit` gets its own ORC `ResourceTracker`. `removeModule` calls `RT->remove()`. `Undo` removes the module and cleans the PTU from the AST. | Verified | [OrcIncrementalExecutor.cpp](https://raw.githubusercontent.com/llvm/llvm-project/main/clang/lib/Interpreter/OrcIncrementalExecutor.cpp), [Interpreter.cpp](https://raw.githubusercontent.com/llvm/llvm-project/main/clang/lib/Interpreter/Interpreter.cpp) |
| clang-repl supports out-of-process execution. Not on Windows. | Verified | [OrcIncrementalExecutorBuilder.cpp](https://raw.githubusercontent.com/llvm/llvm-project/main/clang/lib/Interpreter/OrcIncrementalExecutorBuilder.cpp) |
| `-fincremental-extensions` enables statements at global scope. It does not enable redefinition. A redefinition without `%undo` is a normal Clang error. | Verified | [ClangRepl](https://clang.llvm.org/docs/ClangRepl.html), [code-undo.cpp](https://raw.githubusercontent.com/llvm/llvm-project/main/clang/test/Interpreter/code-undo.cpp) |
| Cling requires the fork `root-project/llvm-project`. The fork carries patches such as "Allow `cling::DeclUnloader` access to `CXXRecordDecl` and `Sema` internals". | Verified | [cling README](https://raw.githubusercontent.com/root-project/cling/master/README.md), [cling-llvm22 commits](https://github.com/root-project/llvm-project/commits/cling-llvm22) |
| Cling `DefinitionShadower` moves each redefinition into a new inline namespace `__cling_N5...` and hides the old declaration from lookup. A redefined class is a new type. | Verified | [DefinitionShadower.cpp](https://raw.githubusercontent.com/root-project/cling/master/lib/Interpreter/DefinitionShadower.cpp) |
| Cling `TransactionUnloader` unloads JIT symbols, clears pending instantiations, and runs `DeclUnloader` in reverse order | Verified | [TransactionUnloader.cpp](https://raw.githubusercontent.com/root-project/cling/master/lib/Interpreter/TransactionUnloader.cpp) |
| Cling has no grammar hook. Extensions subclass `ASTTransformer` and run after Clang parses C++. | Verified, negative | [ASTTransformer.h](https://raw.githubusercontent.com/root-project/cling/master/lib/Interpreter/ASTTransformer.h) |

**Conclusion for option D.** Cling and clang-repl give incremental compilation and undo of a compile. Neither gives a syntax hook. Cling's redefinition creates a new C++ type each time, which conflicts with stable object identity. Cling needs a patched LLVM. A syntax layer in front of either is source translation with a different backend.
### 2.3 LLVM ORC

| Capability | Status | Source |
|---|---|---|
| Remove code with `ResourceTracker::remove()` and `JITDylib::clear()` | Verified | [ORCv2 doc](https://llvm.org/docs/ORCv2.html), [Core.h](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/ExecutionEngine/Orc/Core.h) |
| ORC tracks in-flight calls into removed code | Not present. The docs and `Core.h` have no such text. | Same |
| Second strong definition of a symbol in one JITDylib | Error `DuplicateDefinition`. Separate JITDylibs may define the same symbol. | [ORCv2 doc](https://llvm.org/docs/ORCv2.html), [Core.cpp](https://github.com/llvm/llvm-project/blob/main/llvm/lib/ExecutionEngine/Orc/Core.cpp) |
| Stable call targets with updatable pointers | Verified: `IndirectStubsManager::createStub`, `updatePointer`, `findStub`, `findPointer` | [IndirectionUtils.h](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/ExecutionEngine/Orc/IndirectionUtils.h), local LLVM 23 header |
| Host symbols | `DynamicLibrarySearchGenerator` uses `dlsym` and `GetProcAddress`. Linux needs `-rdynamic`. Windows needs exported symbols. | [ExecutionUtils.h](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/ExecutionEngine/Orc/ExecutionUtils.h), [Kaleidoscope ch. 4](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl04.html) |
| Exceptions in JIT code | ELF and MachO through `EHFrameRegistrationPlugin`. COFF x86-64 through `COFFPlatform` and the ORC runtime with `RtlAddFunctionTable`. | [JITLink doc](https://llvm.org/docs/JITLink.html), [coff_platform.cpp](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/orc/coff_platform.cpp) |
| Exception from a native callback into a JIT handler on Windows | The ORC runtime source has a FIXME: "not supported yet" | [coff_platform.cpp](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/orc/coff_platform.cpp) |
| Debugging JIT code | ELF: GDB JIT interface through `ELFDebugObjectPlugin`. MachO: `GDBJITDebugInfoRegistrationPlugin`. LLDB and GDB. | [DebuggingJITedCode](https://llvm.org/docs/DebuggingJITedCode.html), local header |
| Debugging JIT code on Windows with PDB | No upstream support. One third-party project exists. | [Discourse](https://discourse.llvm.org/t/orc-jit-weekly-13-mcjit-orc-pdb-debugging/55185) |
| Profiling | `PerfSupportPlugin`, `VTuneSupportPlugin` | local headers |
| Out-of-process execution | `ExecutorProcessControl`, `SimpleRemoteEPC`, `llvm-jitlink-executor` | [JITLink doc](https://llvm.org/docs/JITLink.html) |

Lang Hames stated this on the mailing list: a function body cannot change at the same address while the function is on the stack. He pointed to a safe-point concept as the client's job ([llvm-dev 2015](https://lists.llvm.org/pipermail/llvm-dev/2015-March/083651.html)). This confirms the working principle: a swapped pointer does not make unloading safe.

**RTTI identity across modules.** libstdc++ compares type names with `strcmp` by default. MSVC compares the decorated name. libc++ on Linux compares pointers by default ([libc++ typeinfo](https://github.com/llvm/llvm-project/blob/main/libcxx/include/typeinfo), [libstdc++ typeinfo](https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/typeinfo)). A class that exists in two modules breaks `dynamic_cast` under libc++ unless one `type_info` symbol wins. The shell model avoids this. A shell exists in one module for the session.

### 2.4 Unreal Engine Live Coding and Hot Reload

| Fact | Status | Source |
|---|---|---|
| Live Coding is built on Live++. It patches functions in the running process. | Verified | [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-live-coding-to-recompile-unreal-engine-applications-at-runtime) |
| "Modifying class layouts while the engine is running is not supported" (4.22) | Verified | [4.22 notes](https://forums.unrealengine.com/t/unreal-engine-4-22-released/124018) |
| UE5 reinstancing "replaces existing instances of objects throughout the application". Users must clean up pointers in `ReloadReinstancingCompleteDelegate`. | Verified | Epic docs above |
| Reinstancing creates new objects and copies properties. The old object dies. | Verified for the result. The exact code path through `FBlueprintCompileReinstancer` is partial. | [FReload](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FReload) |
| Constructor defaults in `.cpp` do not update existing instances | Verified | Epic docs above |
| Only one destructor version can exist. Shutdown after Live Coding can crash. | Verified | Epic docs above |

Live Coding solves implementation replacement and class layout change. It does not preserve object identity. Reinstancing is the opposite of the requirement here. The requirement is a stable address with migrated state. Live Coding can do this only for method-body changes. For layout changes it replaces the object.

### 2.5 Other C++ hot-reload systems

| System | Mechanism | Layout change | Object identity | Source |
|---|---|---|---|---|
| Live++ | Hot-patch jump at function entry. New compilands link against the process. Globals redirect to the original instance. | Not supported. Adding members, bases or virtuals needs user migration in hooks. | Kept, but layout changes are the user's problem | [Live++ docs](https://liveplusplus.tech/docs/documentation.html) |
| RuntimeCompiledC++ | Factory objects. On change: serialize all instances, construct new objects, deserialize, delete old. | Supported through serialization | Addresses change. Pointers go through object ids. | [RCC++ wiki](https://github.com/RuntimeCompiledCPlusPlus/RuntimeCompiledCPlusPlus/wiki/How-it-works) |
| cr.h | Reload a plugin DLL. `CR_STATE` saves and restores statics. | Growing only in the default mode | Plugin state only | [cr](https://github.com/fungos/cr) |
| jet-live | Link new object files into a shared library. Hook functions. Redirect to old globals. | "your app will probably crash" | Kept | [jet-live](https://github.com/ddovod/jet-live) |
| Blink | Link a COFF object into the process. Jump at function entry. | Not addressed | Kept | [blink](https://github.com/crosire/blink) |
| Our Machinery | C API function tables. Replace the pointers. Plugin state is one struct copied with `memcpy`. | Not supported | Kept. Objects "cannot be relocated". | [archive](https://ruby0x1.github.io/machinery_blog_archive/post/dll-hot-reloading-in-theory-and-practice) |

Two patterns repeat. Function-level patching keeps objects but cannot change layouts. Reinstancing changes layouts but replaces objects. The requirement needs a third pattern: a stable object with a separate replaceable state. The Our Machinery API table is the closest precedent for stable dispatch. No surveyed C++ system migrates per-object state behind a stable object.

### 2.6 Precedents for an explicit reload contract

| System | Contract | Source |
|---|---|---|
| .NET Hot Reload | A list of supported edits and "rude edits". Rude edits: rename, delete a member, add a virtual or override, change a signature. | [Microsoft docs](https://learn.microsoft.com/en-us/visualstudio/debugger/supported-code-changes-csharp) |
| Java JVMTI `RedefineClasses` | Method bodies only. "must not add, remove or rename fields or methods". Active frames keep the old bytecode. | [JVMTI spec](https://docs.oracle.com/en/java/javase/21/docs/specs/jvmti.html) |
| DCEVM and JetBrains Runtime | Adds field, method and supertype changes to HotSpot | [DCEVM](https://ssw.jku.at/dcevm/) |
| Erlang | Old and current code run together. A third load purges the old code and kills processes in it. `code_change` migrates state explicitly. | [Erlang docs](https://www.erlang.org/doc/system/code_loading.html) |

Erlang gives the closest model for state migration: an explicit callback with old and new state, and a two-version limit. Section 5 follows this precedent with a table of change classes.

### 2.7 Scripting systems on top of Unreal

| System | Class model | Reload | Source |
|---|---|---|---|
| Unreal AngelScript (Hazelight) | Script classes inherit C++ classes through the Unreal reflection system. Not native vtables. Constructors are avoided because they run at unpredictable times during reload. | Non-structural changes reload in play mode. | [angelscript.hazelight.se](https://angelscript.hazelight.se/scripting/cpp-differences/) |
| SkookumScript | Binds through the Blueprint meta tagging | Live update | [skookumscript.com](https://skookumscript.com/unreal/) |

Both reuse the engine reflection system instead of native subclasses. The requirement here forbids that path. The AngelScript rule about constructors supports the choice to run script constructors once and never during a reload.

### 2.8 Languages that map custom syntax to C++ semantics

| System | Approach | Reload | Source |
|---|---|---|---|
| cppfront | Syntax 2 to C++ source. Full C++ semantics through the regular compiler. | None | [cppfront](https://github.com/hsutter/cppfront) |
| Jakt | Transpiles to C++. Has inheritance and inline `cpp` blocks. | None | [jakt](https://github.com/SerenityOS/jakt) |
| Circle | A full C++ compiler with extensions. Closed source. | None | [circle](https://github.com/seanbaxter/circle) |
| Carbon | Own frontend. Uses Clang as a library for C++ interop. Plans inheritance from C++ types. | None | [Carbon interop goals](https://github.com/carbon-language/carbon-lang/blob/trunk/docs/design/interoperability/philosophy_and_goals.md) |
| Cling | C++ with declaration shadowing for redefinition | Redefinition only. No object migration. | [cling blog](https://root.cern/blog/cling-declshadow/) |

No surveyed system combines custom syntax, native inheritance, runtime replacement and preserved object identity. The design in section 4 combines parts of cppfront, Our Machinery and Erlang.
## 3. Architecture comparison

### 3.1 Options

| Id | Option |
|---|---|
| A | Custom syntax translated to C++ source. Unmodified Clang compiles it. |
| B | Custom parser that drives Clang Sema and reuses Clang CodeGen. |
| C | Own frontend that emits LLVM IR. Clang supplies declarations and ABI information. |
| D | Cling or clang-repl with a syntax layer in front. |
| E0 | No new language. Restricted C++ plus Live++ or Live Coding. |
| E1 | A reflection-based script layer such as Unreal AngelScript. |

The backend choice is separate from the frontend choice. Shared library per generation or ORC JIT applies to A, B and C. Section 3.3 covers it.

### 3.2 Decision matrix

Scores: 5 is best. The rightmost column weights the criterion.

| Criterion | A: source | B: Sema | C: own IR | D: Cling | E0: C++ | E1: reflection | Weight |
|---|---|---|---|---|---|---|---|
| C++ API fidelity and overload resolution | 5 | 5 | 1 | 5 | 5 | 2 | high |
| Genuine C++ subclasses | 5 | 5 | 3 | 5 | 5 | 1 | high |
| Templates, inheritance, virtual dispatch | 5 | 4 | 1 | 5 | 5 | 2 | high |
| Hot reload with preserved objects | 4 with the runtime model | 4 with the runtime model | 4 with the runtime model | 3. Redefinition makes a new type. | 2. Method bodies only. | 4 | high |
| No wrappers, adapters or compiler forks | 5 | 2. Unstable internal API. | 2 | 2. Cling needs a patched LLVM. | 5 | 1 | high |
| Engineering complexity | 4 | 1 | 1 | 2 | 5 | 3 | high |
| Diagnostics | 3. `#line` plus a rewriter. | 4 | 4 | 3 | 5 | 3 | medium |
| Debugging | 5 with shared libraries | 4 | 4 | 2 | 5 | 2 | medium |
| Compiler-version coupling | 5. Driver flags only. | 1 | 2 | 1 | 5 | 5 | high |
| Build time per reload | 3. PCH plus link. | 3 | 3 | 4 | 3 | 5 | medium |
| Runtime performance | 5 | 5 | 4 | 4 | 5 | 2 | high |
| Licensing and shipping | 5. No LLVM in the shipped game. | 3. LLVM in the editor. | 3 | 2. Cling license and fork. | 4. Live++ is commercial. | 5 | medium |
| Meets the syntax goal | 5 | 5 | 5 | 4 | 1 | 4 | high |

### 3.3 Backend: shared library per generation versus ORC JIT

| Criterion | Shared library per generation | ORC JIT in process |
|---|---|---|
| Symbol resolution | Normal linker. Needs engine exports. | `DynamicLibrarySearchGenerator`. Needs the same exports. A custom generator can use the engine PDB or ELF symbol table. |
| Reload latency | Compile plus link. Hundreds of milliseconds. | Compile only. Saves the link step. |
| Debugging on Windows | Real PDB. Visual Studio works. | No upstream PDB support. |
| Debugging on Linux and macOS | Normal | GDB JIT interface through plugins |
| Crash symbolication and profilers | Normal | Needs perf or VTune plugins |
| Exceptions | Normal | Supported on ELF, MachO and COFF x86-64. A FIXME for native-to-JIT propagation on Windows. |
| Unloading | `dlclose` or `FreeLibrary`. Same safety rules. | `ResourceTracker::remove`. Same safety rules. |
| Engine dependency | None. The compiler is a separate process. | The engine links LLVM. |
| Shipping | Same generated C++ builds into the binary. | A second code path for shipping. |
| Stable stubs for delegates | Generated thunk modules | `IndirectStubsManager` with `updatePointer` |

The shared library backend wins for version 1. ORC wins only when the link step dominates latency or when the engine cannot export symbols. The loader interface hides the choice. The runtime model is identical in both.

### 3.4 Option assessments

**A. Source translation.** The translator owns the script grammar and the runtime model. Clang owns every C++ question. The translator never needs to resolve a native name, an overload or a template. The one exception is shell generation, which needs the list of base virtuals from libclang. Failure modes: diagnostics in C++ terms, and sugar that needs type information. Section 7.9 lists which sugar needs type information and defers it. `#line` maps errors and debug info to script lines. The local experiment in section 2.1 confirms this for DWARF and CodeView.

**B. Custom parser on Clang Sema.** Clang's own parser drives Sema through `ActOn*` calls. A second parser can do the same. The cost is a dependency on an internal C++ API with no stability promise. Carbon calls Sema lookup, overload and template APIs and builds AST nodes directly. Swift calls Sema for template instantiation and keeps a fork of LLVM. Both have a team that tracks LLVM at head. The gain over A is better diagnostics and no generated text. The loss is permanent version coupling and a large unfamiliar API surface. Clang Sema also expects the parser to manage `Scope` objects and delayed parsing. The engineering cost is at least ten times option A for the same language.

**C. Own frontend with LLVM IR.** The frontend must reimplement name lookup, overload resolution and implicit conversions. It must also reimplement template instantiation. Clang CodeGen can supply ABI lowering through `CodeGenModule`, but only for declarations that Sema already built. This option cannot meet the template requirement. Reject.

**D. Cling or clang-repl with a syntax layer.** Cling has no grammar hook. All Cling extensions transform the AST after Clang parses C++. A syntax layer in front of Cling is option A with Cling as the backend. Cling adds declaration shadowing and transaction undo. Shadowing gives a new type for each redefinition, which breaks object identity. Cling needs a patched LLVM. clang-repl is upstream and gives `Parse`, `Execute` and `Undo`, but has no redefinition and no Windows PDB debugging. Neither adds value over A plus a shared library loader for this problem. Transaction undo is useful only for a REPL.

**E0. Restricted C++ plus Live++ or Live Coding.** This is the honest baseline. It gives every goal except syntax and state migration. Live++ patches method bodies and keeps objects. Layout changes need user migration in hooks. A team that only wants fast iteration should start here. The custom language adds syntax and automatic migration. The design in section 4 can also host plain C++ classes written against the same runtime model. This keeps E0 as a fallback inside the plan.

**E1. Reflection-based script layer.** Script classes become engine reflection classes, not native subclasses. This conflicts with the requirement. Reject, but note that it is the proven path in Unreal.

### 3.5 Conclusion

Option A with the shell runtime model and a shared library backend has the highest weighted score. Its weak point is diagnostics. Its strong points are no compiler coupling, exact C++ semantics, normal debugging and a shipping path with no LLVM. Option B is the only credible alternative and costs an order of magnitude more for a better error message.
## 4. Proposed architecture

### 4.1 Summary

The architecture has four parts.

1. A translator. It parses the script syntax and emits C++ source with `#line` directives.
2. The engine toolchain. An unmodified Clang compiles the generated C++ against a precompiled header of the engine API.
3. A generation loader. It loads each compiled script generation as a shared library.
4. A script runtime. It owns class records, state blocks, dispatch tables, thunks, and the reload sequence.

The translator resolves only the names that the script language defines. Clang resolves all other names. This rule keeps the translator small and keeps C++ semantics exact.

### 4.2 Compiler pipeline

```
 player.spp ──► Translator ──► player.gen.cpp  (#line "player.spp")
                     │              shells/Player.shell.cpp   (once per class)
                     │              thunks/Player.thunks_3.cpp (append-only)
                     ▼
               libclang query  ◄── engine.pch  (virtual methods of the base,
               (read-only)                       record layout hash)
                     │
                     ▼
   clang -O1 -g -include-pch engine.pch ... ──► scripts_gen42.dll / .so
                     │
                     ▼
   Generation loader: dlopen / LoadLibrary ──► register(ScriptRuntime&)
                     │
                     ▼
   Runtime: validate ──► safe point ──► migrate ──► switch tables ──► retire
```

The translator needs type information from Clang in one place only. It needs the list of virtual methods of each native base class to emit a shell. The libclang C API gives this list. The libclang API has a stable ABI, unlike the C++ Sema API.

The pipeline runs Clang as a separate process. The engine does not link LLVM. A crash in the compiler does not stop the game. The same generated C++ compiles into shipping builds with a constant dispatch table.

### 4.3 Three kinds of generated code

Each generated artifact exists for one reason.

| Artifact | Lifetime | Reason |
|---|---|---|
| Generation module (`player.gen.cpp`) | Replaced on each reload | Holds the method bodies, state struct, field initializers and reflection table of one version |
| Shell (`Player.shell.cpp`) | Session. Compiled once per (script class, base class) | Gives the object a stable vtable, stable type_info and stable address |
| Thunk module (`Player.thunks_N.cpp`) | Session. Append-only | Gives native delegates a stable function address per script method |

None of these artifacts wraps the engine API. Script code calls engine functions directly as C++.

### 4.4 Class representation

A script class `Player : Actor` becomes three C++ entities.

```
 Native object (stable address, owned by the engine)
 ┌──────────────────────────────────────────┐
 │ Actor  (native base, unchanged layout)   │
 ├──────────────────────────────────────────┤
 │ Player_Shell                             │
 │   vptr ──► Player_Shell vtable (stable)  │
 │   ClassRecord* cls  ──► runtime record   │
 │   void*        state ─┐                  │
 └───────────────────────┼──────────────────┘
                         ▼
 ┌────────────────────────────┐    ┌──────────────────────────────┐
 │ PlayerState_g42 (gen 42)   │    │ ClassRecord "Player"         │
 │   float health             │    │   gen = 42                   │
 │   float speed              │    │   FnPtr* table ──► gen 42 fns│
 │   TArray<Item*> inventory  │    │   StateLayout* layout        │
 └────────────────────────────┘    │   ClassRecord* super         │
                                   │   ShellInfo* shell           │
                                   └──────────────────────────────┘
```

The shell is a real C++ subclass of `Actor`. The engine sees a normal object. `dynamic_cast<Actor*>`, `typeid`, and every native registration work on the shell.

The shell overrides every virtual method of the base, not only the overridden ones. Each override reads one slot of the current dispatch table. A slot that the script does not override points to a forwarding function. The forwarding function calls the base implementation. This choice makes the shell independent of the set of overrides. The script can add or remove an override without a new shell.

```cpp
// Generated shell (compiled once)
struct Player_Shell final : Actor, spp::ShellBase {
    template <class... A> explicit Player_Shell(spp::Construct, A&&... a)
        : Actor(std::forward<A>(a)...) { spp::init_state(this, &Player_record); }
    ~Player_Shell() override { spp::destroy_state(this); }

    void Tick(float dt) override
    { return spp::call<void(Actor*, float)>(cls, SLOT_Tick, this, dt); }
    void TakeDamage(float a) override
    { return spp::call<void(Actor*, float)>(cls, SLOT_TakeDamage_f, this, a); }
    void TakeDamage(Actor& src, float a) override
    { return spp::call<void(Actor*, Actor&, float)>(cls, SLOT_TakeDamage_Af, this, src, a); }
    // one slot for each virtual of Actor and of each interface base
};
```

Pure virtual functions in the base have no forwarding target. The translator reports an error when the script does not override them.

A `final` base method cannot get a slot. The translator reports an error when the script overrides it.

### 4.5 Script inheritance

A script class can derive from a script class. `Enemy : Player` produces `Enemy_Shell : Player_Shell`. The derived shell adds no overrides. It sets `cls` to the `Enemy` record. The state struct derives from the parent state struct. Script virtual methods use extra slots after the native slots. The derived record copies the parent slots and overrides some of them.

### 4.6 Dispatch

| Call | Path | Cost over plain C++ |
|---|---|---|
| Native code calls a native virtual on a script object | vtable → shell stub → table slot → generation function | one extra indirect call |
| Script calls a native method | direct C++ call, resolved by Clang | none |
| Script calls a non-virtual script method in the same generation | direct call to a generation function | none |
| Script calls a script virtual method | table slot | one indirect call |
| Script reads a script field | `state->field` through the state pointer | one load |
| Native delegate calls a script method | thunk → table slot → generation function | one extra indirect call |

Each generation function starts with a generation guard.

```cpp
void Player__heal(Player_Shell* self, float amount) {
    if (SPP_UNLIKELY(spp::active_generation != 42)) return spp::stale_call("Player::heal");
    auto* st = static_cast<PlayerState_g42*>(self->state);
    st->health = std::min(st->health + amount, 100.0f);
}
```

The guard costs one load, one compare and one branch. The guard makes stale code fail safely. Stale code cannot read a migrated state block with an old layout. This guard is the main defense against escaped references, not the unloading policy.

### 4.7 Script state and migration

Each generation module exports a reflection table for each class.

```cpp
struct FieldInfo {
    const char* name;        // stable field identifier
    const char* alias;       // previous name from [alias("...")], or null
    const char* type_key;    // mangled C++ type name of the field
    uint32_t    offset, size, align;
    uint8_t     flags;       // trivially_copyable, nothrow_move, script_struct
    void (*init)(void* self, void* field);     // runs the field initializer expression
    void (*move)(void* dst, const void* src);  // move-construct
    void (*destroy)(void* field);
};
```

The migration plan for one class compares the old and the new tables.

| Old field | New field | Action |
|---|---|---|
| name X, type T | name X, type T | move or memcpy |
| name X, type T | name Y, type T, `[alias("X")]` | move or memcpy |
| name X, type T | name X, type U | run `convert` hook if defined, else `init` with a warning |
| name X | absent | destroy |
| absent | name Y | run `init` |
| script struct S | script struct S | recurse with the same rules on S |

The type key is the mangled C++ type name. Two generations agree on the mangled name of each native type because they compile against the same headers.

A class can define an optional hook `void onReload(OldState old)`. The hook runs after the structural migration. `OldState` is a reflection view. The hook can read any old field by name. The hook runs in the new generation and can fail. A failed hook logs an error and leaves the defaults in place.

`OldState` offers `has("name")`, `has<T>("name")` and `get<T>("name")`. `get` throws `std::runtime_error` for an absent field or a wrong type. The view shows every field of the old generation by its old name, also a field that the migration moved.

The `convert` hook of a field `X` is a class function `T convert_X(Old old)`. `T` is the type of the new field. `Old` is the old type. The hook has no `this`. The runtime uses it when the old field type equals `Old`. A rename without `[alias]` logs a warning when the removed and the added field have the same type.

### 4.8 Reload sequence

```
 main thread                          compile thread
 ────────────                         ──────────────
 frame N  ......                      translate + compile + link gen 42
 frame N+1 .....                      load dll, run register() → descriptors
 frame N+2 .....                      validate: build plans or reject
 ── frame boundary, job system idle ──────────────────────────────────
 [safe point]
   1. allocate new state blocks for all live objects of changed classes
   2. migrate each object: move fields, init new fields, destroy removed
   3. run onReload hooks
   4. swap cls->table and cls->layout for each class
   5. active_generation = 42
   6. free old state blocks
   7. mark generation 41 as retired
 frame N+3 ..... (runs gen 42)
```

Step 1 runs before any move. An allocation failure aborts the reload before any object changes. Steps 2 to 6 cannot fail except in a user hook or a throwing native move constructor. A throwing native move constructor during migration is a fatal error. The runtime logs it and requests a restart.

### 4.9 Generation retirement

A retired generation stays loaded in version 1. The guard makes its code harmless. Memory grows by the size of one module per reload. A development session with 500 reloads of a 1 MB module uses 500 MB. The runtime reports this and the developer restarts.

Version 2 unloads a retired generation when three conditions hold.

1. The generation pin count is zero. Closures that leave script code and coroutine frames hold a pin.
2. A full frame boundary passed after retirement.
3. No thread was inside a function of that generation at retirement. The safe point guarantees this.

Raw function pointers to script code do not exist in the language. Only thunks and pinned closures leave script code. See section 5.

The core module (M8). The canonical class record, the factory, the shell constructor (and so the vtables) and the thunks must stay valid for ever. So the loader loads a second instance of a generation that adds a class or a thunk: the core module. The core never unloads. The code module of the generation gives the slots, the layout and the constructor bodies. So every retired generation can unload, generation 1 too. CLANG_FILES loads a private copy of the library. JIT adds an IR clone to a separate JITDylib.

A live object keeps three generations loaded: the generation of its layout, the generation of its record when no core exists, and the generation that ran `new` for it. A state field of a type that can hold code (`kMayHoldCode`, for example `std::shared_ptr` with its control block) blocks each unload while it lives. The runtime does not see a code pointer that native code keeps.

### 4.10 Compile and load of a generation

The compile runs off the main thread. One generation is one shared library with all script modules. Cross-module script calls are direct calls inside the library. The module exports one symbol `script_register`. The loader calls it. The function returns class descriptors, dispatch tables and the reflection tables.

The shared library resolves engine symbols through the normal dynamic linker. The engine must export the symbols that scripts use. Section 8 lists the fallback when the engine cannot export symbols.

The shared library links against the engine import library on Windows and against the executable on Linux and macOS. Template instantiations and inline functions compile into the generation module. Each generation instantiates its own copies. This is normal for shared libraries.

### 4.11 Thunks and closures

A native delegate needs a function address that stays valid across reloads. The thunk module provides it.

```cpp
// Player.thunks_3.cpp (append-only, compiled when a new bindable method appears)
void Player__onHit__thunk(Player_Shell* self, Actor& src, float a)
{ return spp::call<void(Player_Shell*, Actor&, float)>(self->cls, SLOT_onHit, self, src, a); }
```

A closure that leaves script code becomes a `spp::Closure<Sig>` object. The object holds a generation pin. Its call operator checks the guard and calls the lambda. The object converts to `std::function` and to engine delegate types. The translator applies this conversion when a lambda converts to a non-script type.

### 4.12 Component inventory

| Component | Owner | Size estimate | Reuse |
|---|---|---|---|
| Parser and translator | new | 10 to 20 k lines | none |
| libclang query for shells | new | 1 k lines | libclang |
| Compile driver and PCH management | new | 1 k lines | clang, lld |
| Generation loader | new | 500 lines | dlopen / LoadLibrary |
| Runtime: records, tables, state, migration | new | 3 to 5 k lines | none |
| Diagnostic rewriter | new | 1 k lines | clang `-fdiagnostics-format` output |
| Debugger support (natvis, LLDB formatters) | new | small | none |
| Optional ORC backend | later | 2 k lines | LLVM ORC, JITLink |
## 5. Reload compatibility specification

### 5.1 Definition: the same object survives a reload

An object survives a reload when all of these hold.

1. The object address does not change.
2. The vptr and the `type_info` of the object do not change.
3. Every native pointer, reference, handle and delegate to the object stays valid.
4. Every native registration of the object stays valid. Examples: scene graph entries, tick lists, physics bodies.
5. The native base subobject is untouched. The runtime does not run any native constructor or destructor.
6. The script state keeps the values of all unchanged fields.

Only the `state` pointer and the `cls->table` pointer change.

### 5.2 Change classes

Each change falls into one of four classes.

| Class | Meaning |
|---|---|
| LIVE | Supported. Live objects survive. |
| RECONSTRUCT | The shell changes. Live objects of the class cannot survive. Version 1 rejects the reload. A later version can offer destroy-and-recreate through an engine-specific hook. |
| RESTART | Needs a new process. |
| ERROR | The translator rejects the script. |

### 5.3 The contract

| Change | Class | Reason |
|---|---|---|
| Method body | LIVE | New table, same shell |
| Add or remove a non-virtual script method | LIVE | Table slots are per generation |
| Add or remove a script virtual method | LIVE | Script slots live after the native slots. The record rebuilds. |
| Add or remove an override of a native virtual | LIVE | The shell stubs every native virtual |
| Change a method signature | LIVE for script methods. Native delegates bound to the old signature detach with a warning. | Thunks are per signature |
| Add a script field | LIVE | `init` runs the field initializer |
| Remove a script field | LIVE | `destroy` runs on the old block |
| Rename a script field with `[alias(...)]` | LIVE | Migration by old name |
| Rename a script field without `[alias(...)]` | LIVE with data loss and a warning | Treated as remove plus add |
| Change a field type | LIVE with `convert` hook or `onReload`. Without a hook: default value and a warning | No implicit conversion between arbitrary types |
| Reorder fields | LIVE | Migration uses names, not offsets |
| Change a field initializer | LIVE. Existing objects keep their values. | Initializers run only for new fields |
| Change a script constructor | LIVE. Existing objects do not rerun it. | Constructors run once |
| Change a script destructor | LIVE. The current generation runs at destruction. | Dispatch through the table |
| Add or remove script-level inheritance | RECONSTRUCT | The shell hierarchy changes |
| Change the native base class | RECONSTRUCT | The shell changes |
| Add a native interface base | RECONSTRUCT | The shell changes |
| Change a script value type (struct) layout stored in a field | LIVE | Recursive migration |
| Change a script value type stored inside a native container | LIVE only when the layout is unchanged. Otherwise the container resets with a warning. | The runtime cannot enumerate elements of arbitrary native containers |
| Change a script enum | LIVE. Values migrate as integers. | Enums are integers |
| Change module-level script variables | LIVE | Module state block migrates like fields |
| Change a native header | RESTART | The PCH, shells and the engine binary change together |
| Change a native class layout | RESTART | Native code must rebuild |
| Change a native virtual signature | RESTART | The shell and the engine must rebuild |
| Instantiate a new native template from script | LIVE | Instantiation lives in the generation module |
| Change which native templates a script instantiates | LIVE | Same |
| Script takes the address of a script function | ERROR | No raw function pointers to replaceable code |
| Script passes a lambda to a native API | LIVE. The lambda becomes a pinned closure. | Pin keeps the generation loaded |
| Script stores a pointer or reference to a script field across calls | ERROR when detectable, else undefined after reload | State blocks move |
| Script object holds a pointer to another script object | LIVE | Shell addresses are stable |

### 5.4 Native machinery

| Topic | Behavior |
|---|---|
| vtables | Live in the shell module. Never change after shell load. |
| RTTI, `typeid`, `dynamic_cast` | The shell has one `type_info` for the session. Native `dynamic_cast` to any native base works. `dynamic_cast` to a script class works inside script code through the generated shell header. The `is` operator uses class records, not RTTI. |
| Exceptions | A script function can throw any native exception type. The frames unwind through shells and thunks as normal C++ frames. Script-defined exception types are not in version 1. The engine toolchain flags decide `-fexceptions`. |
| Function pointers | Not expressible in script. Thunks give stable addresses for delegates. |
| Delegates | Bind to a thunk plus the object. The thunk reads the current table. |
| Native code that stores a `std::function` from script | Receives a pinned closure. After reload the closure calls old code. The guard rejects the call and logs it. The runtime reports the pin at retirement. |
| Coroutines and async tasks | Not in version 1. Version 2 pins the generation from the coroutine frame. |
| Native code retaining a `Player_Shell*` | Works. Native code sees the stable shell type through the generated header. |

### 5.5 Unsupported in version 1 and the reason

| Feature | Reason |
|---|---|
| Destroy-and-recreate for RECONSTRUCT changes | Engine-specific. The engine must know how to recreate an object and rebind references. |
| Script coroutines | Large translator work. Separate milestone. |
| Script templates and generics | Possible as C++ templates. Deferred to keep the first translator small. |
| Script exception types | type_info of a script type lives in a generation module. Catch sites across generations would not match. |
| Reload while a worker thread runs script code | The safe point needs a quiescent job system. |
## 6. Runtime lifecycle

### 6.1 Entities and their states

**Script module** (one source file)

```
 edited ──► translated ──► compiled into generation G ──► active (G current)
                                                      └──► superseded (G retired)
```

A module has no runtime identity of its own. The generation holds it.

**Script class**

```
 declared in G ──► shell compiled (once) ──► record created ──► active table G
                                                             ──► active table G+1 ...
                                                             ──► record retired (no instances, class removed)
```

The record lives for the session. A class that disappears from the source keeps its record while instances exist. Its table slots point to a "removed class" handler that logs and returns.

**Live native object**

```
 engine new ──► shell ctor ──► init_state ──► script constructor
   ──► [reload: migrate state, keep object] *
   ──► engine delete ──► shell dtor ──► script destructor (current gen) ──► destroy_state
```

The shell constructor registers the object in the per-class live set. The destructor removes it. The live set is a lock-free intrusive list or a per-class vector with a mutex. The reload reads it at the safe point.

**Script state block**

```
 allocated by init_state ──► owned by the shell ──► migrated at reload ──► destroyed with the shell
```

**Generation (compiled implementation)**

```
 compiling ──► loaded ──► validated ──► active ──► retired ──► (v2) unloaded
                   └──► rejected ──► unloaded immediately
```

A rejected generation ran only `script_register`. That function allocates descriptors and does nothing else. Unloading it is safe.

**Closure or asynchronous task from script**

```
 created in G (pin G) ──► stored by native code ──► called (guard checks G is active) ──► destroyed (unpin G)
```

### 6.2 Safe point

The runtime commits a reload at one point in the frame. The engine calls `spp::Runtime::Commit()` at that point. The requirements for that point are:

1. The main thread is not inside any script function.
2. Every worker thread that can run script code is idle or blocked outside script code.
3. The engine does not hold references to script state fields.

Most engines have a frame boundary after the job system drains. The commit runs there. The commit takes the time of the migration. For 10 000 objects with 20 fields this is under one millisecond.

### 6.3 Synchronization options

| Option | Mechanism | Cost | Risk |
|---|---|---|---|
| Stop-the-world at frame boundary (recommended) | Engine guarantees quiescence. No instrumentation. | Zero per call | Depends on an engine guarantee |
| Entry counters (implemented, `SyncMode::EntryCounters`) | Every slot call, thunk, closure call and script destructor is a script entry. The outermost entry of a thread increments a global count. A nested entry increments only a thread-local depth. The commit closes a gate and waits until the count is zero. New entries wait at the gate. | One flag check per entry in the default mode. One atomic increment and decrement per outermost entry in this mode. | Deadlock when a script frame waits for the commit or for a thread at the gate |
| Epochs or RCU | Readers publish an epoch. The commit waits for a grace period. | Small | Complex. Needed only for lock-free reload during execution. |
| Per-object locks | Each shell has a lock. | High | Deadlocks, no benefit |

The recommendation is stop-the-world at the frame boundary. It is the default mode. The debug counter `ScriptTickScope` checks it: the runtime asserts that the counter is zero at commit.

The second mode is for an engine with no frame boundary (risk 9, open question 3). The engine selects it at run time with `Loader::set_sync_mode(SyncMode::EntryCounters)`. The protocol:

1. A script entry (`ScriptEntry`) reads one global flag. In the default mode, the entry does nothing more.
2. In the second mode, the outermost entry of a thread increments the active count. Then it reads the gate. When the gate is closed, the entry decrements the count, waits until the gate opens and tries again.
3. A nested entry on the same thread (script, native, then script) increments only a thread-local depth. So a call back into script code never waits for a commit that waits for the outer call.
4. The commit (`CommitWindow`) closes the gate. Then it waits until the active count is zero. The commit thread gets depth one, so it can run hooks. At the end, the gate opens.
5. All operations on the count and the gate are sequentially consistent. So an entry sees the closed gate, or the commit sees the entry. The last decrement before the commit is a release, and the commit reads zero with acquire. So the commit sees all writes of the script code before it. The entry reads the open gate with acquire, so it sees all writes of the commit.

The second mode does not count object creation. Native code that creates a script object or reads its fields holds a `ScriptEntry`. A commit on a thread that is in script code fails with an error and does not wait. A script frame that waits for the commit, or for a thread at the gate, deadlocks. Change the mode only when no thread is in script code. `SPP_NO_ENTRY_COUNTERS` removes the mode at compile time.

### 6.4 In-flight call tracking

The safe point removes in-flight calls by construction. The guard handles the cases that the safe point cannot see. Those cases are closures retained by native code and coroutine frames.

### 6.5 Reload atomicity

The commit is atomic from the point of view of script code. No script code runs between the first migration and the table swap. Native code can run during the commit on other threads when it does not call script code. The engine must not call a script virtual during the commit. The frame boundary gives that.

### 6.6 Error recovery

| Failure | When | Effect |
|---|---|---|
| Translator error | Before compile | Report with script line. Old generation continues. |
| Clang error | Before load | Report with script line through `#line`. Old generation continues. |
| Link error, unresolved engine symbol | Before load | Report the symbol. Old generation continues. |
| `script_register` fails | After load | Unload. Old generation continues. |
| Validation rejects a RECONSTRUCT change | After load | Unload. Report the class and the change. Old generation continues. |
| Allocation fails in commit step 1 | Commit | Free allocated blocks. Old generation continues. |
| `onReload` hook throws | Commit | Log. The object keeps defaults for affected fields. Reload completes. |
| Native move constructor throws | Commit | Fatal. The runtime cannot undo partial moves. Request a restart. |
| Script code crashes at runtime | Any time | Same as a native crash. Debug builds get a script stack through `#line`. |
| Stale closure called | Any time | The guard logs the call and returns a default value. |

Rollback after the commit does not exist. The developer reverts the source. The next reload migrates back with the same rules.
## 7. Minimal language specification

### 7.1 Design rules

1. Every script construct has one C++ meaning. The document states it.
2. Native types keep their native names. `TArray<T>`, `FString`, `Vector3` appear as in C++. A prelude can add type aliases. The language adds no public wrapper types.
3. The translator resolves script names only. Clang resolves native names, overloads, conversions and templates.
4. Numeric literals: `5` is `int`. `5.0` is `float`. `5.0d` is `double`. `5L` is `int64_t`.
5. `null` is `nullptr`. Storage is by value or by handle. The type decides. `ref` and `out` mark reference parameters. Section 7.10 has the rules.
6. No garbage collection. A handle is a smart pointer or an intrusive pointer.

### 7.1.1 Builtin types

The script has fixed-width builtin type names. A C++ `long` is 32 bit on Windows and 64 bit on Linux. A script must mean the same on each platform, so `long` in a script is always 64 bit. Each alias maps to one canonical C++ type. The translator changes the name at parse time. The emitted C++, the override key and the reload type key all use the canonical name. A script that renames `ulong x;` to `uint64 x;` reloads with no type-change rejection.

| Script name | Alias | C++ type |
|---|---|---|
| `bool` | - | `bool` |
| `sbyte` | `int8` | `int8_t` |
| `byte` | `uint8` | `uint8_t` |
| `short` | `int16` | `int16_t` |
| `ushort` | `uint16` | `uint16_t` |
| `int` | `int32` | `int32_t` |
| `uint` | `uint32` | `uint32_t` |
| `long` | `int64` | `int64_t` |
| `ulong` | `uint64` | `uint64_t` |
| `nint` | `intptr` | `intptr_t` |
| `nuint` | `uintptr` | `uintptr_t` |
| `float` | - | `float` |
| `double` | - | `double` |
| `char` | - | `char` (8 bit) |
| `wchar` | `char16` | `char16_t` (16 bit) |

- `void` stays.
- The C++ multi-word spellings are errors: `unsigned`, `unsigned int`, `long long`, `unsigned long`, `unsigned long long`, `unsigned char`, `unsigned short`, `signed char` and the like. The message names the script type: "use 'ulong'".
- A native name such as `int32_t`, `size_t` or `char16_t` is a normal native name. It is not changed.
- A native virtual method with `int`, `unsigned` or `unsigned long long` parameters matches a script override with `int`, `uint` or `ulong`. The override key maps the C++ spelling to the fixed-width name, with the size of `long` of the host.

### 7.2 Example: a script class with native base, fields, overrides and overloads

```csharp
// player.spp
using "Engine/Actor.h";
using "Engine/Combat.h";

class Player : Actor {
    float health = 100.0;
    float speed = 5.0;
    Actor* target = null;
    TArray<Item*> inventory;
    [alias("hp")] float maxHealth = 100.0;

    Player(FString name) : super(name) {      // planned: the parser does not support it yet
        this.inventory.Reserve(8);
    }

    override void Tick(float dt) {
        if (this.target != null) {
            var dir = (this.target.Position - this.Position).Normalized();
            this.Move(dir * this.speed * dt);
        }
        super.Tick(dt);
    }

    override void TakeDamage(float amount) {
        this.health -= amount;
        if (this.health <= 0.0) this.Destroy();
    }

    override void TakeDamage(Actor& source, float amount) {
        this.target = ref source;
        this.TakeDamage(amount * 1.5);
    }

    void heal(float amount) {
        this.health = Math.Min(this.health + amount, this.maxHealth);
    }

    bool pickUp(Item* item) {
        if (this.inventory.Num() >= 8) return false;
        this.inventory.Add(item);
        return true;
    }

    FString describe() {
        return `${this.Name} hp=${this.health}`;   // planned: string interpolation
    }
}
```

### 7.3 Mapping of each construct

| Script | Generated C++ | Kind |
|---|---|---|
| `using "Engine/Actor.h";` | `#include "Engine/Actor.h"` in the generation TU. The PCH already has it. `using <vector>;` gives `#include <vector>`. | sugar |
| `class Player : Actor { ... }` | `Player_Shell` (once) plus `PlayerState_gN`, free functions and a reflection table | runtime model |
| `float health = 100.0;` | `float health;` in the state struct plus `init_health(self, field)` that runs `100.0f` | runtime model |
| `[alias("hp")]` | `alias = "hp"` in the FieldInfo | runtime model |
| `Player(FString name) : super(name)` | Done (S4). The canonical record of generation 1 builds the shell from the `super` arguments (`spp::construct_shell`). Then `Player__ctor(self, name)` runs. The `super` arguments can use parameters only. | runtime model |
| `override void Tick(float dt)` | `void Player__Tick_gN(Player_Shell* self, float dt)` installed in `SLOT_Tick`. The translator matches the name and the parameter types against the libclang list. A match makes the function an override, and `override` is required. | runtime model |
| `public`, `private`, `protected` | The parser reads and stores the access word. The translator does not enforce it. | none |
| `virtual`, `new` | The parser reads and stores the word. It has no effect yet. | none |
| `super.Tick(dt)` | `self->Actor::Tick(dt)` | sugar |
| `this.health` | `st->health` where `st` is the typed state pointer | runtime model |
| `this.Position`, `this.Move(...)` | `self->Position`, `self->Move(...)`. Clang resolves the member. | sugar |
| `this.TakeDamage(amount * 1.5)` | `self->TakeDamage(amount * 1.5f)`. Clang resolves the overload. The call goes through the vtable and the shell. | sugar |
| `ref source` | `spp::ref_arg(source)` | sugar |
| `void heal(float amount)` | `void Player__heal_gN(Player_Shell* self, float amount)`. A call `p.heal(1.0)` becomes `Player__heal_gN(p, 1.0f)`. | runtime model |
| `bool pickUp(Item* item)` | The return type stands before the name. `void` means no return value. | sugar |
| `float Hp { get; set; } = 50.0;` | A property. The state struct gets a hidden field `__p_Hp`. The translator makes the script methods `get_Hp` and `set_Hp`, each with its own slot. `{ get; }` has no setter. `{ get; private set; }` stores the access word of the setter. | runtime model |
| `float Shield { get { ... } set { ... } }` | A custom accessor. The setter body uses the name `value` for the new value. A read `Shield` or `this.Shield` calls the getter. A write calls the setter. | runtime model |
| `var dir = ...`, `int x = 5;` | `auto dir = ...;`, `int x = 5;` | sugar |
| `const int X = 5;`, `const var x = ...` | `const int X = 5;`, `const auto x = ...;` | sugar |
| `` `${a} hp=${b}` `` | Planned. `spp::format("{} hp={}", a, b)` which calls the engine string format. The parser gives an error now. | sugar |
| `if`, `while`, `for (int i = 0; ...)`, `foreach (Type x in xs)` | same C++ statements; `foreach` is a range-for. `foreach (var x in xs)` gives `auto`. | sugar |
| `throw e;`, `throw;`, `try { ... } catch (T e) { ... }`, `catch (...) { ... }`, `finally` | `throw e;`, `throw;`, `try { ... } catch (T e) { ... }`, `catch (...) { ... }`. A catch clause needs a name, except `catch (...)`. | sugar |
| `Math.Min(...)` | `Math::Min(...)`. `.` on a type name means `::`. | sugar |

The parser reports a clear error for each removed form: `function`, `let`, `import`, `name: Type`, `-> Ret` and `for ... of`. The words `var`, `foreach`, `in`, `using`, `override`, `virtual`, `new`, `public`, `private`, `protected`, `get`, `set`, `finally` and `void` are reserved. A read or a write of `p.N` on another object of the class calls `get_N` or `set_N` of that object. A write gives the assigned value, so `a = p.N = 1` works. The rewrite uses the type of `p` when it is known. A known native type gives the native member. For the native base of the class or an unknown type, the rewrite uses the name only. If `p` is then not an object of the class, the access throws `std::runtime_error`.

### 7.4 Constructing native objects and ownership

```csharp
class Level : Actor {
    Player* spawn(World& world) {
        var p = new Player("Bob");            // spp::construct<Player_Shell>(FString("Bob"))
        world.Register(p);                    // engine owns p from here
        return p;
    }

    TUniquePtr<Effect> makeEffect() {
        var fx = make_unique<Effect>(2.0);    // the native factory as-is
        fx.Attach(this);
        return fx;                            // move
    }

    void localWork() {
        Vector3 v = Vector3(1.0, 2.0, 3.0);   // value on the stack
        var list = TArray<int>();             // native container on the stack
        list.Add(3);
    }
}
```

`new T(args)` for a script class calls the shell constructor and the script constructor. `new T(args)` for a native class is C++ `new`. The result type follows section 7.10: the handle of the class, or a value. Ownership follows the handle. The examples above that show raw pointers use the engine's intrusive handle. In the prototype (S5a), `new T(args)` of a script class gives the handle of the class, or the value for a `[value]` class. The limits are in `HANDOFF.md`.

### 7.5 Lambdas

```csharp
class Level : Actor {
    void arm(TimerSystem& timer) {
        timer.After(2.0, () => { this.heal(10.0); });
    }
}
```

A lambda is `(T a) => ...` or `(a) => ...`. A lambda parameter without a type is `auto`. A lambda captures by value and captures `this` as a pointer. This is a semantic choice. JavaScript captures variables by reference with a garbage collector. C++ cannot give that safely. When the lambda converts to a native callable type, the translator wraps it in `spp::Closure`. The closure pins the generation and checks the guard. The lambda body is a C++ lambda inside the generation function.

### 7.6 Script-to-script inheritance and script virtual methods

```csharp
class Enemy : Player {
    virtual float threat() { return 1.0; }
}

class Boss : Enemy {
    override float threat() { return 10.0; }
}
```

A `virtual` method adds a script slot. An `override` method in the derived class with the same name and parameter types replaces the slot in the derived record. An `override` method that matches a native virtual method overrides it. The translator requires `override` for a native virtual method. The parser reads and stores `virtual` and `new`, but they have no effect yet. Script inheritance is not done.

### 7.7 Type tests

```csharp
if (actor is Player) { Player p = actor as Player; p.heal(5.0); }
```

`is` walks class records from the shell. `as` returns the shell pointer or `null`. Both work only on script classes. Native downcasts use `dynamic_cast<T*>(x)` as in C++. The parser does not support `is` and `as` yet.

### 7.8 Reload example

Before:

```csharp
class Player : Actor {
    float hp = 100.0;
    void heal(float amount) { this.hp += amount; }
}
```

After:

```csharp
class Player : Actor {
    [alias("hp")] float health = 100.0;
    float shield = 25.0;
    void heal(float amount) { this.health = Math.Min(this.health + amount, 100.0); }
    void onReload(OldState old) {
        if (old.has("hp") && old.get<float>("hp") > 100.0) this.health = 100.0;
    }
}
```

The reload keeps every `Player` object at its address. The migration moves `hp` into `health`. It runs the initializer of `shield` for each object. It runs `onReload`. The table slot for `heal` points to the new function. Native delegates bound to `heal` call the new function through the thunk.

### 7.9 Sugar versus new semantics

| Straightforward sugar | New semantics or substantial compiler work |
|---|---|
| keywords `var`, `const`, `foreach`, `using`, the type before the name | script fields in a separate state block |
| `this.` for native members | `[alias(...)]`, `onReload`, `OldState` |
| `super.` | override matching against libclang data |
| template arguments `T<U>` passed through | `is` and `as` on script classes |
| string interpolation | pinned closures |
| `foreach`, `null`, literals | script virtual slots |
| `.` on type names as `::` | generation guard |
| C++ operators and precedence kept | coroutines (deferred) |
| | optional chaining `?.` (deferred; needs a helper template) |
| | properties with getters and setters (done, with limits: see 7.3) |

Everything in the left column is a local textual rewrite. Everything in the right column needs the runtime model or type information.
### 7.10 Value and handle semantics

**Goal.** The user does not think about by-value, by-pointer and by-reference in most code. The type decides the storage. Syntax for the details is optional and rare. This section replaces the pointer and reference rules of section 7.1 (rules 5 and 6) and the ownership text of section 7.4 where they differ.

#### 7.10.1 Two storage kinds

| Kind | Storage | Copy | C++ form |
|---|---|---|---|
| value | Inline: local, field, array element | Real copy | `T` |
| handle | Heap, behind a smart pointer | Copies the handle | `std::shared_ptr<T>` or a custom handle |

- The kind belongs to the type. The user writes plain `Weapon`. The use site has no marker.
- A handle is always a smart pointer. A custom smart pointer is valid. An intrusive raw `T*` is valid (see 7.10.4).
- Assignment of a handle copies the handle. Both names then share the object.
- Every handle can be null. The script tests it with `== null` and `!= null`. There is no `?` syntax.
- The script uses `.` for member access on values and handles. The translator emits `.` or `->`.

#### 7.10.2 How the translator picks the kind

For a native type `T`, the translator uses the first rule that matches:

1. An exact stub: `[native] class Weapon;`
2. A stub on a base class. The attributes `handle` and `intrusive` are inherited.
3. A pattern stub, for example `std.vector<*>`. The longest pattern wins.
4. The built-in rule: a trivially copyable, standard-layout type is a value.
5. The stub `class *;`, which supplies the handle.

A script class is a handle, unless it has `[value]`. A script class that derives from a native class uses the handle of that class.

#### 7.10.3 Configuration is script code

Configuration has no separate file. Builtin scripts (preludes) hold it.

- The host gives a list of prelude scripts to the translator. `sppc --emit-cpp` takes `--prelude file`. The embedding API takes files or strings.
- The translator parses all preludes, in list order, before each script. The declarations are visible with no `using`.
- A later declaration of the same type overrides an earlier one. Two conflicting declarations in one file are an error.
- A script can override a prelude stub for itself. The translator gives a warning.
- The prelude hash is part of the translator cache key. A prelude edit invalidates all cached translations.

A native type is declared by a class stub. The stub has no body.

```csharp
// engine.spp (prelude)
[native, default_handle("std::shared_ptr<$T>")] class *;      // fallback for native classes
[native, value] class std.string;                             // forced value, not trivially copyable
[native, value] class std.vector<*>;
[native, value] class Weapon;
[native, handle("$T*"), intrusive("AddRef", "Release")] class Object;
[native, handle("TexPtr<$T>")] class Texture;                 // custom smart pointer
```

| Attribute | Meaning |
|---|---|
| `native` | The stub names a C++ type. Libclang checks that the type exists. |
| `value` | Inline storage with a real copy. It also works on a script class: `[value] class Stats { ... }`. |
| `handle("tpl")` | The handle type. `$T` is the class. |
| `default_handle("tpl")` | The handle for native classes with no other rule. Valid only on `class *`. |
| `intrusive("add", "release")` | The type counts references itself. A raw `T*` is a valid handle. |

`*` in a stub name matches a template family or a namespace. Examples are `std.vector<*>` and `physics.*`.

**The stub `class *;`.** It stands for every native class that has no stub of its own. It has the lowest priority.

- Only one `class *;` is active. A later prelude overrides an earlier one.
- It accepts only `native` and `default_handle`. `value` on `*` is an error. It would break polymorphic types.
- Step 4 of 7.10.2 comes before it. `Vec3` is a value and `class *;` never applies to it.
- With no `class *;`, a native class that is not a value and not intrusive has no handle. Scripts cannot store it. This is a strict mode.

#### 7.10.4 Engine-owned objects and borrowed pointers

An engine object is a handle when its type is intrusively reference counted. The prelude declares this with `intrusive`. A raw `Actor*` is then a full handle. The translator emits the add and release calls through a handle wrapper.

A native signature can use `T*` or `T&` for a type that is not a value, not intrusive and has no `handle`. The pointee is then borrowed.

- A script can use a borrowed pointer in a local or a parameter. It follows the rules of `ref` (7.10.6).
- A script cannot store it. It cannot be a field, a closure capture, a collection element or a return value.
- The translator reports the error at the script line. The message names the type and suggests a stub with `handle` or `intrusive`.
- The fallback `class *;` does not change this. A script can create an object with `new Mesh(...)`. The translator then builds the default handle. A foreign raw pointer cannot become a `shared_ptr` safely.

#### 7.10.5 Parameters and performance

- A value parameter has by-value meaning. The translator emits `const T&` when the body only reads it. It emits `T` for small trivially copyable types (16 bytes or less) and when the body mutates or stores it.
- A handle parameter is passed as `const Handle&` when the body does not store it. This avoids a reference-count change. The body that stores it takes a copy.
- A call to a native function needs no marker. The translator reads the signature. It binds a `ref` argument to a native `T&` of a value type. It uses the handle for a native `T*` or `T&` of a handle type.

#### 7.10.6 `ref` and `out`

The keywords `ref` and `out` stay. They are the only syntax for the details of passing.

| Script | C++ |
|---|---|
| `ref Vec3 p` | `Vec3& p` |
| `out Vec3 p` | `Vec3& p`. The caller passes an uninitialized `Vec3 v;`. |
| `ref Actor p` | `Handle<Actor>& p`. The callee can rebind the caller's handle. |
| `var r = ref this.pos` | `auto& r = st->pos;` |

- Changing the pointee of a handle needs no `ref`. A handle already shares the object.
- **Restriction in version 1.** A `ref` is not storable. It cannot be a field, a closure capture, a collection element or a return type. A field such as `ref Weapon last;` is an error.
- **Reason.** A field lives in the state block. The reload moves this block. A stored reference would dangle. With this rule the translator emits raw C++ references with no runtime check.
- A later version can relax the rule for references to objects with a stable address.
- **Implementation.** The prefix `&e` is removed. The parser gives the error "use 'ref' instead of '&'". A call argument `ref e` or `out e` becomes `spp::ref_arg(e)`. This wrapper converts to `T&` and to `T*`, so one form fits both a reference parameter and a pointer parameter. The not-storable rule and the borrowed-storage check are errors by default (`--no-strict-borrowed` makes them warnings).

#### 7.10.7 Checks on a stub

- `value` needs a copy constructor and a nothrow move constructor. The reload needs the move to migrate a state block.
- `value` on a polymorphic type that is not `final` is an error. It prevents slicing.
- `handle` needs `operator->`. Libclang infers `make` and `null`. A missing function gives an error that names it.
- `intrusive` needs both named methods on the type.

#### 7.10.8 Effect on reload

- A `value` field lives in the state block. The migration moves it. The `FieldInfo` flags `trivially_copyable` and `nothrow_move` apply.
- A handle field migrates as a handle. The reload does not change the pointee.
- A change of kind for a type changes the layout of all fields of that type. The type key detects this. The reload follows the type-change row of section 5.3: the `convert` hook converts the value, else the field gets its default value and the runtime logs a warning.

#### 7.10.9 Known limits

- A `shared_ptr` cycle leaks. A custom handle can give weak links. Version 1 does not detect cycles.
- A plain handle to an engine object can dangle if the engine destroys the object. The `intrusive` count prevents this. A non-intrusive custom handle must provide its own safety.

#### 7.10.10 Example

```csharp
// prelude: [native, default_handle("std::shared_ptr<$T>")] class *;
//          [native, handle("$T*"), intrusive("AddRef", "Release")] class Object;
//          [native, value] class std.string;
[value] class Stats { float hp; float armor; }

class Player : Actor {                       // Actor : Object, so Actor* is a handle
    Actor mate;                              // raw Actor*, intrusive
    Mesh mesh;                               // std::shared_ptr<Mesh>
    Vec3 home;                               // POD, inline
    std.string name;                         // forced value, inline
    Stats stats;                             // script value class, inline

    void Hit(Actor src, float dmg) {         // handle, passed by const handle
        this.mate = src;
    }
    void Fill(out Vec3 v) {}                 // Vec3&
    void Swap(ref Vec3 a, ref Vec3 b) {}
}
```

## 8. Proof-of-concept plan

### 8.1 Harness

Build a standalone executable `minengine`. It has:

- `class Actor` with three virtuals: `Tick(float)`, `TakeDamage(float)` and `TakeDamage(Actor&, float)`. It has a `std::string Name`, a `Vec3 Position` and `void Move(Vec3)`.
- `class World` with a vector of `Actor*`, a tick loop, a `Delegate<void(float)>` list, and a timer that stores `std::function<void()>`.
- A file watcher and a command channel. Tests drive the harness through the channel.
- The executable exports its symbols. Linux: `-rdynamic`. Windows: `__declspec(dllexport)` on `Actor` and `World`.

Build the harness with the same compiler and flags as the target engine. Use clang-cl on Windows when the engine uses MSVC. Keep the harness under 800 lines.

### 8.2 Experiment order

Step R0 comes before the translator. It proves the runtime model with hand-written C++. The hand-written code is the exact output that the translator must produce later. This order isolates the risky part.

| Step | Work | Pass criterion | Fail criterion | Riskiest assumption tested |
|---|---|---|---|---|
| R0 | Hand-write `Player_Shell`, `PlayerState_g1`, generation functions, reflection table and `script_register`. Compile into `gen1.so`. Load it. | A `Player` ticks through `Actor*` from the world loop. `typeid(*a).name()` names the shell. | Unresolved engine symbols or ABI mismatch. | Export surface and toolchain compatibility. |
| R1 | Hand-write `gen2.so` with a changed `Tick` body, a renamed field and a new field. Implement migration and commit. | The same address ticks with new behavior. `hp` arrives in `health`. `shield` is 25. Old block freed. | Any address change or value loss. | Migration by name and type key works with native field types. |
| R2 | Bind a delegate to `heal` through a thunk before reload. Call it after reload. | The delegate calls the gen 2 body. | The delegate calls gen 1 or crashes. | Thunks give stable addresses. |
| R3 | Store a lambda from gen 1 in the harness timer. Reload. Fire the timer. | The guard logs a stale call. No crash. No memory corruption under ASan. | Crash or corrupted state. | The guard catches escaped closures. |
| R4 | Make `gen3.so` fail to compile. Make `gen4.so` change the base class. | Both reloads are rejected. Gen 2 keeps running. The object still ticks. | Any state change after a rejected reload. | Validation runs with no side effects. |
| R5 | Run the tick loop on two threads with a barrier. Trigger a reload from a third thread. | Commit runs only at the barrier. ASan and TSan are clean. | Data race or commit during a tick. | The safe point design. |
| R6 | Unload gen 1 after R3 with pin count zero (after the timer fired and dropped the closure). | `dlclose` succeeds. Later ticks are clean. | Crash after unload. | Pin counts are sufficient for unloading. |
| F1 | Write the translator for the subset in section 7.2. Generate the code from R0 and R1 from `player.spp`. | `diff` of generated code against the hand-written code is empty except for names. R0 to R4 pass with generated code. | The translator needs type information outside libclang. | The translator resolves script names only. |
| F2 | Introduce ten common mistakes into the script. | Each Clang diagnostic points to the script line. The rewritten message mentions script names. | A diagnostic points into generated code. | `#line` plus rewriting is enough for diagnostics. |
| F3 | Set a breakpoint in `heal` in lldb or Visual Studio. Step. Inspect `this.health`. | The debugger shows `player.spp` lines. The natvis or formatter shows state fields. | The debugger shows generated code. | `#line` is enough for debugging. |
| P1 | Measure translate, compile and link time with a PCH of a large header set. Use the real engine headers when available. | Under two seconds for one module after a one-line change. | Over five seconds. | Iteration speed is acceptable without a JIT. |

### 8.3 Mapping to the requested prototype steps

| Requested step | Experiment |
|---|---|
| 1. Compile a custom-syntax script that derives from a native class | F1 |
| 2. Override a virtual callable through a base pointer | R0, F1 |
| 3. Call native APIs including an overloaded method | R0, F1 |
| 4. Create a live instance | R0 |
| 5. Change the implementation and reload | R1 |
| 6. Preserve the address and identity | R1 |
| 7. Migrate state | R1 |
| 8. Retire the old implementation safely | R3, R6 |
| 9. A failed reload leaves the previous version running | R4 |

### 8.4 Tools in the harness

- AddressSanitizer in every experiment. It detects stale reads of freed state blocks.
- ThreadSanitizer in R5.
- A `script_stats` command that prints generations, pin counts, live objects and table slots.
## 9. Risk register

| # | Risk | Likelihood | Impact | Test | Fallback |
|---|---|---|---|---|---|
| 1 | Escaped references reach migrated state. Closures, coroutine frames or native-stored callables call old code. | High | Memory corruption | R3 | Generation guard at every function entry. Pinned closures. No raw function pointers. Never unload in version 1. |
| 2 | The engine cannot export its API symbols. A monolithic executable with internal symbols cannot link a generation module. | Medium. Engine-specific. | Blocks the shared-library loader | R0 with the real engine | Use ORC with a custom definition generator that resolves addresses from the engine PDB or ELF symbol table. Or add an export list to the engine build. |
| 3 | Engine class systems reject runtime-defined subclasses. Reflection, GC, serialization or factories need registration at static init. | Medium. Engine-specific. | Blocks the shell model | Engine spike before M1 | Shells run the engine registration macros in the shell module at load. If the engine forbids late registration, restrict script classes to bases without registration. |
| 4 | Toolchain ABI mismatch. The engine builds with MSVC, scripts with clang-cl. | Low | Crashes at the boundary | R0 on Windows | clang-cl is ABI compatible with MSVC by design. Match CRT, `/EH`, `/GR`, iterator debug level. |
| 5 | Compile latency with large engine headers. | Medium | Slow iteration | P1 | PCH. Parallel TUs. Keep the compiler process warm. ORC backend later for link-free loading. |
| 6 | Diagnostics quality. Clang errors speak C++. | High | Poor developer experience | F2 | `#line`. A diagnostic rewriter. Translator-side checks for common script errors before Clang. |
| 7 | Debugging quality on Windows. | Medium | Poor developer experience | F3 | Shared libraries get real PDBs. natvis for state blocks. Avoid ORC on Windows until JIT debug support exists. |
| 8 | libclang cannot describe a base class. Templated bases, dependent types, private virtuals, deleted constructors. | Medium | Shell generation fails for some bases | F1 with templated bases | Use the C++ LibTooling API behind one small adapter. Accept version coupling in that adapter only. |
| 9 | Reload on a non-quiescent job system. | Medium. Engine-specific. | Data races | R5, S21 (`EntryCounters` tests) | Entry counters plus blocking of new entries as a second mode. Implemented: `SyncMode::EntryCounters` (section 6.3). |
| 10 | Native move constructors throw during migration. | Low | Fatal reload | R1 with a throwing type | Require nothrow move for field types. The translator warns on other types. |
| 11 | Template instantiations differ between the engine and a generation module. ODR with function-local statics in headers. | Low | Duplicated singletons | R0 | Same as any plugin DLL. Export singleton accessors from the engine. |
| 12 | Scope creep in the language. Coroutines, generics, properties, optional chaining. | High | Delays | Roadmap review | Version 1 is section 7 only. |

The highest risk is number 1. The guard and the pin design answer it on paper. R3 and R6 test it.
## 10. Implementation roadmap

| Milestone | Content | Depends on | Exit criterion |
|---|---|---|---|
| M0 Spikes (2 to 3 weeks) | R0, P1 and the engine spike for risk 2 and 3 on the real engine | none | Generation module loads in the engine. Compile time measured. Export and registration answers known. |
| M1 Runtime spine (3 to 4 weeks) | Runtime library: records, state blocks, migration, commit, thunks, closures, guard. R1 to R6 with hand-written C++. | M0 | R1 to R6 pass under ASan and TSan. The generated-code contract is written as a document. |
| M2 Translator v0 (4 to 6 weeks) | Parser, AST, C++ emitter, libclang shell generator, `#line`, compile driver, PCH. Section 7.2 subset. | M1 | F1 passes. Generated code matches the contract. |
| M3 Diagnostics and debugging (2 weeks) | Diagnostic rewriter, natvis and LLDB formatters, script stack in crash reports. | M2 | F2 and F3 pass. |
| M4 Engine integration (3 to 4 weeks) | Safe point hook, allocators, registration of shells, file watcher, editor command. First gameplay class in the real engine. | M1, M2 | One real actor class runs from script with reload. |
| M5 Contract enforcement (2 weeks) | Validation of every row in section 5.3. Error texts. `[alias(...)]`, `convert`, `onReload`. | M4 | Each row has a test. |
| M6 Language v1 (4 weeks) | Script inheritance, script virtuals, `is` and `as`, lambdas and pinned closures, module variables, enums, structs. | M5 | Section 7 complete. |
| M7 Shipping mode (2 weeks) | Build generated C++ into the shipping binary. Constant tables. No loader. | M6 | Shipping build has no LLVM and no file watcher. |
| M8 Unloading (2 weeks) | Pin counts, retirement and `dlclose`. | M6 | R6 passes in the engine with 1000 reloads. |
| M9 Optional ORC backend (4 weeks) | In-process JIT behind the loader interface. Debug-symbol resolver. | M8 | Reload latency improves by a measured factor. Debugging still works on the target platform. |
| M10 Source map and tolerant parse | `TranslateResult::source_map`. A parse that recovers from a bad statement. | M2 | Each identifier maps to the same text in both directions. Other methods stay byte-identical. |
| M11 VS Code extension | Syntax highlighting, language configuration, file association and a thin client for the forked clangd. | none | The extension packages. A grammar snapshot test passes. |
| M12 Language server stage 1 | A clangd fork with script diagnostics at script positions. | M10 | A type error and a parse error show at the script line. |
| M13 Language server stage 2 | Completion, hover, go-to-definition, references, document symbols, semantic tokens. | M12, M11 | One test per feature. `this.` lists native base members. |
| M14 Language server stage 3 | Background index, C++ to script navigation, rename across both, `sppc --compile-commands`. | M13 | Go-to-definition from a `.cpp` into a `.spp` works. |

Estimates assume one engineer with Clang experience. M9 runs only when P1 shows a latency problem or risk 2 forces it.
### 10.1 Language server and editor support

**Server.** The server is a fork of clangd. It keeps full C++ support and adds script support.
- The fork translates a `.spp` document to C++ in process. It maps every position with a source map.
- `Emitter::put` already receives the script offset of each leaf. A side table of segments (script offset, generated offset, length) gives both directions. Synthesized text, for example `spp::deref(`, has no segment. A result in such text is dropped.
- Clang Sema gives types, overloads, native base members, templates and includes. A native script front end inside clangd needs a new Sema and takes months.
- A translation stops at a parse error today. Completion after `this.` needs a tolerant parse (M10). A bad statement is skipped to the next `;` or `}`. A trailing `.` gives an empty member name. The result keeps `ok = false` and a non-empty `cpp`.
- The `#line` directives stay in the generated text. clangd uses file locations, so this should be safe. This is unverified. M12 checks it. If it fails, a translator option omits `#line` for the server.
- A proxy server in front of stock clangd is the alternative with no fork. It costs less to maintain, but the index does not see scripts. It is not chosen.

**Fork layout and maintenance.**
- Fork `llvm/llvm-project` as `spp-llvm`. Branch `spp/release-23.x` comes from `release/23.x`.
- New code lives in `clang-tools-extra/clangd/spp/`. Hooks in upstream files stay under `#if CLANGD_SPP`, in at most 5 files.
- `sppc_lib` comes in with `FetchContent` at a pinned commit.
- Rebase on each LLVM major release, about every 6 months. `UPSTREAM.md` holds the procedure. After a rebase, run `ninja check-clangd` and the tests in `clangd/test/spp/`.
- A cold build takes 30 to 60 minutes with `LLVM_TARGETS_TO_BUILD=host`. Use `ccache`.
- A `.spp` file gets its compile flags from a `.clangd` fragment (`PathMatch: .*\.spp`, `Add: [-xc++, ...]`). Stage 3 adds `sppc --compile-commands`.

**VS Code extension.** It lives in `editors/vscode/`. The extension does not fork the clangd extension.
- It provides the language id `spp` for `.spp`, a TextMate grammar, a language configuration and a command that sets `clangd.path` to the forked binary.
- The official clangd extension serves only the language ids `c`, `cpp`, `cuda-cpp`, `objective-c` and `objective-cpp`. No setting adds an id. This comes from the extension sources and is not tested here.
- So the extension starts a thin `vscode-languageclient` client for `spp`. It uses the same binary as the official extension. Both processes share one index directory. This is unverified.
- Bootstrap: `files.associations` maps `*.spp` to `cpp`. One process serves both, but the grammar does not apply. Use it until the thin client works.
- A later option is a setting for extra language ids in the official extension, sent upstream.

**Order.** M10, M12, M13, M14 form one chain. M11 runs in parallel and joins M13. The language slices of section 7.10 and M6 run after M10, because both touch the parser.

**Open decisions.** These need an answer before M12:
1. File extension: decided. The extension is `.spp` and the language id is `spp` (see `DECISIONS.md`).
2. Fork layout: `spp-llvm` fork as above, a patch series in this repository, or a submodule.
3. Extension location: `editors/vscode/` here, or a separate repository.
4. Build order: language server first (M10 to M13) or language v1 (M6) first.

## 11. Open questions

Each question changes the architecture.

1. Does the engine export its API symbols, or can the build add an export list? A "no" selects the ORC path with a debug-symbol resolver instead of shared libraries.
2. Which engine systems must know a new subclass? Reflection, GC, serialization, networking, factories. The answer decides whether shells can register at load time.
3. Does a frame boundary exist where no thread runs gameplay code? A "no" selects entry counters instead of stop-the-world.
4. Which compiler and flags build the engine? MSVC or Clang. Exceptions on or off. RTTI on or off. The script toolchain must match.
5. Must shipping builds precompile scripts into the binary? A "yes" confirms source translation and excludes a runtime JIT in shipping.
6. Does version 1 need coroutines or async? A "yes" adds a translator milestone and the coroutine pin design before M6.
7. Are there engine base classes with pure virtuals that take or return engine template types? This decides whether libclang is enough for shell generation.
8. Which engine types are intrusively reference counted, and which smart pointers does the engine use? These need stubs in the prelude (section 7.10). Other engine pointers are borrowed and scripts cannot store them.
std.vector<int> history = std.vector<int>();