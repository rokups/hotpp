# Hot reload of lambdas that native code holds

Status: evaluation only. No code changed.
Date: 2026-10-29.

The facts below come from a read-only code survey. The survey did not verify every reference.
Check each file reference before you rely on it.

## Question

Can the runtime hot-reload a script lambda that native code stores?

## Answer

Yes. The current closure design does not allow it. A new design that treats a lambda like a method allows it.

## Current behavior

- A script lambda becomes an `hot::Closure` (`include/hot/runtime/closure.h`).
- The closure holds the lambda in a `std::function`, a pin on its generation, and a guard.
- The translator emits `make_closure` for each arrow lambda (`src/translator/lower.cpp:655-692`).
- The lambda body and its captures are compiled into the library of one generation.
- Native code stores the closure. Example: `Actor::AddTimer` in `tests/engine/Actor.h` (the test host).
- After a reload, the guard compares the closure generation with `active_generation` (`closure.h:75`).
- A mismatch logs a stale call and returns `R()`. No lambda body runs.
- The pin keeps the old library loaded while native code holds the closure.
- The test `EscapedClosureIsStaleAfterReload` (`tests/integration/closures/generation_unload_tests.cpp:83`) checks this on purpose.
- `hot-design.md:515` records this as a decision.
- The runtime does not unregister or rebind native callbacks. Only `ClearTimers` drops timers.
- A named method already reloads. Native code holds a thunk. The thunk calls through the slot table. Slot numbers do not change (`DECISIONS.md:31`).

## Proposed design

Give each lambda the same treatment as a method.

1. **Lambda body as a slot.** The translator lowers each lambda to a hidden member of the state struct. The member gets a slot and a signature key in `ClassRecord::slots`.
2. **Invoker outside the script library.** The closure holds a capture handle and a slot. The code that native `std::function` or `Delegate` calls must live in the shell module or the runtime. No native pointer then refers to code in an old library.
3. **Captures in runtime-owned storage.** Today the captures sit inside the closure in native memory. Their layout comes from the old generation. The runtime cannot find them to migrate them. A runtime-owned capture block works like a state object. The live-object list tracks it (`include/hot/runtime/state.h:31`). The commit migrates it with the existing rule: copy fields with the same name and type key (`DECISIONS.md:48`).
4. **Stable lambda identity.** A lambda needs a key that survives edits. Options: enclosing method plus ordinal, or a hash of the signature and the capture list. If the key or the signature does not match, use the current stale behavior.
5. **No generation pin.** Calls go to the current generation. A live closure no longer needs to pin the old one. Old libraries can then unload while native code holds closures.

## Costs and open questions

- **Copy semantics.** Copies of a closure would share one capture block. This is correct for lambdas that capture only `this` or immutable values. A `mutable` lambda that native code copies changes behavior. Either forbid it or copy the block on a closure copy.
- **New captures.** A capture that did not exist in the old version has no value. Default-initialize it, or mark the closure stale.
- **Common case.** A lambda that captures only the state handle needs no capture block. It is a handle plus a slot, like a delegate.
- **Thunk lifetime.** `include/hot/runtime/loader.h:102-103` says a thunk keeps the address of its first registration. Confirm that this code does not live in a script library that must stay loaded.
- **Callback cleanup.** The runtime does not unbind native callbacks. This stays out of scope.
- **Effort.** The change touches the translator lowering, `closure.h`, the registry and the loader migration. It reverses a documented decision. Treat it as a design-level change.

## Related code

- `include/hot/runtime/closure.h`: `Closure`, `make_closure`, guard.
- `src/runtime/threads.cpp:98-116`: stale-call log and pin counters.
- `src/runtime/loader.cpp:274-299`: `frame_boundary`, unload gate.
- `include/hot/runtime/registry.h`: thunk entries.
- `include/hot/runtime/shell.h:77-94`: `hot::call` and the slot key check.
- `tests/scripts/timer_v1.hot`, `timer_v2.hot`: lambda in a World timer.

## Next step

Run a planning pass. It must check the thunk-lifetime question and the capture-migration design against the code. Then it produces a step plan.
