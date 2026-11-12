# Hot reload of lambdas that native code holds

Status: decided (2026-11-08), updated (2026-11-12). A stored `.hot` closure runs the new body after a reload (see [Rebind at the commit](#rebind-at-the-commit)). The slot design is not built. A `.cpp` lambda gets the same guard (see [Lambdas](cpp-translation.html#lambdas)).
Date: 2026-10-29.

The facts below come from a read-only code survey. The survey did not verify every reference.
Check each file reference before you rely on it.

## Question

Can the runtime hot-reload a script lambda that native code stores?

## Answer

Yes. The current closure design does not allow it. A new design that treats a lambda like a method allows it.

## Current behavior

- A script lambda becomes a `hot::LambdaClosure<Sig>` (`include/hot/runtime/lambda.h`).
- The closure holds a reference to a `LambdaBox` and a pin on its generation.
- The box holds the entry, the captures, the `LambdaRecord` and the destroy function.
- Native code stores the closure. Example: `Actor::AddTimer` in `tests/engine/Actor.h` (the test host).
- The commit gives each box the code of the new generation, if the lambda key and the captures agree. Else it detaches the box.
- A call through a detached box logs a stale call and returns `R()`. No lambda body runs.
- The tests `EscapedClosureRunsTheNewBodyAfterReload` and `EscapedClosureOfARemovedLambdaIsStaleAfterReload` (`tests/integration/closures/generation_unload_tests.cpp`) check the two cases.
- The runtime does not unregister or rebind native callbacks. Only `ClearTimers` drops timers.
- A named method already reloads. Native code holds a thunk. The thunk calls through the slot table. Slot numbers do not change.

## Box lowering

The translator lowers each `.hot` lambda to this form:

```cpp
hot::make_lambda_deduced<__LamTag_N>(GEN, hot::caps(__self, a, __cell_b),
    [](auto& __c, int p) { auto& a = std::get<1>(__c); /* body */ })
```

- The body lambda has no captures. The tuple holds the decayed captures. `__self` is first when the body uses `this`.
- A shared mutable capture is a `hot::Cell<T>` in the tuple. Copies of the closure share the cell.
- A nested lambda captures the names of the outer tuple. It captures a closure like any other value.
- The tag struct `__LamTag_N` holds the key, the `aliases` array, the capture names and the live flag of the module.
- The tags go into an anonymous namespace at namespace scope. A local class cannot have static data members.
- The tags go on the line of `hot_live`. Thus the `#line` numbers of the output do not change.
- `LambdaReg` makes the `LambdaRecord` from the tag. The record holds the key, the signature, the entry, the destroy function and the capture names.

### The signature

The translator does not know the result type or the capture types. C++ knows them. So C++ makes the signature.

- `make_lambda_deduced` takes the address of `operator()<Caps>` of the body lambda. It removes the first `Caps&` parameter. The result is `Sig`.
- This gives declared parameter types, `void` results and value results. It needs no type text from the translator.
- A lambda with an `auto` parameter has no address before the parameter types are known. The translator emits `make_lambda_deferred` for it.
- `make_lambda_deferred` returns a `LambdaDeferred`. Its conversion to `LambdaClosure<R(A...)>` or `std::function<R(A...)>` gives the signature.
- A `LambdaClosure` converts to a `std::function`, to a native handler and to a delegate with `Bind`.

### Rebind at the commit

- `Loader::commit_locked` adds the lambda records of the new generation to the registry. The registry also maps each alias key to its key.
- Then `Loader::switch_live` sets the live flags of the replaced generation to 0. It calls `lambda_rebind_module` for each replaced flag.
- This occurs after the state migration and before the `onReload` hooks.
- For each box of the old module, the runtime finds the new record by the key or by an alias key.
- The record holds the type key and a flag for each capture. It also holds a hash of the script struct layouts of the unit.
- The runtime moves each capture with the same name and type key into a new block. It destroys a removed capture.
- Then the box gets the new captures, entry, record and destroy function. The old block goes with the old destroy function.
- The name of the box (`what`) stays. It is in the generation that the closure pins.
- These changes detach the box: the lambda is removed, the signature changed, a capture is added, or a capture changed its type.
- A capture of a script struct type also detaches the box if the layout hash changed. The type key does not show a layout change.
- The runtime logs one warning for each key: `lambda <key>: <cause>; <n> stored closures are detached`.
- A bound method group is a lambda box with the key part `m:<object>.<Method>` (see [Method groups](lambdas.html#method-groups)). It uses the same rebind.
- `__self` is a pointer to the shell object. The shell object stays at the same address over a reload. Thus the pointer moves as a value.

The rules for safety:

- A live capture block always has the layout of the active generation.
- The runtime changes a box only in the commit window. A worker release waits for the commit window before the last decrement.
- The rebind takes a reference to each box under the list lock. It does the work after the lock is released.
- The runtime does not read the record of a box after its module unloads. Each box gets a new record or a detach before its generation unloads.
- The loader detaches the boxes of a generation in the load path, at `reset`, at the frame boundary unload and in `~Loader`.
- A stored closure keeps its pin. The glue code of the closure is in the pinned generation. The generation unloads when the last closure is released.
- A capture destructor can release another box. That release takes the list lock. So no deadlock occurs.
- The commit does not destroy the old capture blocks of a rebound or detached box at once. A destructor runs script code, and it can free an object that the commit migrates. The commit keeps the blocks in a `CaptureGraveyard`. It destroys them after the old fields are freed, while the old module is still loaded, and with no lock held. A detached box has a null entry at once, so a call is stale.

Limits:

- A script cannot write the type `LambdaClosure<...>`. A lambda is not allowed in a field or module variable initializer, and a convert hook has no lambda. So no field has this type. A field holds a `std.function`. A lambda in a convert hook is an error, so each lambda has a key from its function.

- A deferred lambda with two target signatures under one key has one record in the registry. A box with the other signature detaches.
- The layout hash covers only the structs of the unit. A struct of another unit does not change the hash.

## Host re-subscribe (in use)

A host can drop the old lambdas and subscribe again after a reload. It needs no change of the closure design.
rbfx-script does this: see "Subscriptions after a reload" in its `docs/Architecture.md`.

- The script puts each subscription of an object in one method, for example `void OnSubscribe()`.
- After `commit_pending`, the host walks the live objects with `Loader::each_object`. It filters them by the class name.
- The thunk `Cls::__native` gives the native base of an object from its shell. Each script class has it.
- The host drops the recorded subscriptions of the object. Then it calls the method again through its thunk `Cls::OnSubscribe`.
- `Loader::thunk_key` gives the signature key of a thunk. The host checks it before the cast of the thunk.
- A lambda that the script gives to native code outside this method follows the rebind rule above.

## Proposed design

Give each lambda the same treatment as a method.

1. **Lambda body as a slot.** The translator lowers each lambda to a hidden member of the state struct. The member gets a slot and a signature key in `ClassRecord::slots`.
2. **Invoker outside the script library.** The closure holds a capture handle and a slot. The code that native `std::function` or `Delegate` calls must live in the shell module or the runtime. No native pointer then refers to code in an old library.
3. **Captures in runtime-owned storage.** Today the captures sit inside the closure in native memory. Their layout comes from the old generation. The runtime cannot find them to migrate them. A runtime-owned capture block works like a state object. The live-object list tracks it (`include/hot/runtime/state.h:31`). The commit migrates it with the existing rule: copy fields with the same name and type key.
4. **Stable lambda identity.** A lambda needs a key that survives edits. Options: enclosing method plus ordinal, or a hash of the signature and the capture list. If the key or the signature does not match, use the current stale behavior.
5. **No generation pin.** Calls go to the current generation. A live closure no longer needs to pin the old one. Old libraries can then unload while native code holds closures.

## Costs and open questions

- **Copy semantics.** Copies of a closure would share one capture block. This is correct for lambdas that capture only `this` or immutable values. A `mutable` lambda that native code copies changes behavior. Either forbid it or copy the block on a closure copy.
- **Shared cells.** A captured variable that a lambda writes is a `hot::Cell<T>` (a `std::shared_ptr<T>`). Copies of the closure share the cell. A runtime capture block must keep the cell handle and must not copy the value.
- **New captures.** A capture that did not exist in the old version has no value. Default-initialize it, or mark the closure stale.
- **Common case.** A lambda that captures only the state handle needs no capture block. It is a handle plus a slot, like a delegate.
- **Thunk lifetime.** `include/hot/runtime/loader.h:102-103` says a thunk keeps the address of its first registration. Confirm that this code does not live in a script library that must stay loaded.
- **Callback cleanup.** The runtime does not unbind native callbacks. This stays out of scope.
- **Effort.** The change touches the translator lowering, `closure.h`, the registry and the loader migration. It reverses a documented decision. Treat it as a design-level change.

## Related code

- `include/hot/runtime/lambda.h`: `LambdaBox`, `LambdaClosure`, `make_lambda_deduced`, `make_lambda_deferred`.
- `src/runtime/lambda.cpp`: the box list, `lambda_detach_module` and `lambda_rebind_module`.
- `src/runtime/threads.cpp:98-116`: stale-call log and pin counters.
- `src/runtime/loader.cpp:274-299`: `frame_boundary`, unload gate.
- `include/hot/runtime/registry.h`: thunk entries.
- `include/hot/runtime/shell.h:77-94`: `hot::call` and the slot key check.
- `tests/scripts/timer_v1.hot`, `timer_v2.hot`: lambda in a `World` timer.

## Decision

The slot design is not built. The reasons:

- The rebind at the commit gives the new body to a stored closure. The guard makes each detached call safe.
- The host re-subscribe gives the new code to each subscription. It needs no change of the closure design.
- The rebind uses the key, the signature and the type key of each capture. A mismatch detaches the box.

A `.cpp` lambda has the same rule. `hotc` puts the guard `hot::cpp_lambda` on each lambda that uses the object or a module variable. A stale call logs the stale call and gives the default value. The test `CppStoredLambdaOfAnOldGenerationIsSkipped` checks it under ASan with a smaller state layout in the new version.
