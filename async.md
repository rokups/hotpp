---
layout: default
title: Async and await
nav_order: 15.5
group: Hot script
---

An `async` method is a coroutine. It can wait for another task or for a host event, and it gives control back to the caller while it waits. The `await` operator does the wait.

## Syntax

Write `async` before the return type of a class method. Write the value type. The method returns a `hot::Task` of that type.

| Script | C++ result |
|---|---|
| `async int Load()` | `hot::Task<int>` |
| `async void Save()` | `hot::Task<void>` |

```hot
using <cstdio>;

class Worker {
    int total = 0;

    async int Double(int x) {
        return x * 2;
    }

    async int Sum(int a, int b) {
        var left = await Double(a);
        var right = await Double(b);
        this.total = left + right;
        return this.total;
    }

    async void Bump() {
        this.total = this.total + await Double(1);
    }
}
```

An `async void` method cannot return a value. Use a plain `return;` to end it early.

A method can be `virtual`. An `override` must have the same `async` as the base method.

```hot
class Base {
    virtual async int Compute(int x) {
        return x + 1;
    }
}

class Derived : Base {
    override async int Compute(int x) {
        var b = await base.Compute(x);
        return b * 10;
    }
}
```

## Run a task

A call of an `async` method returns a task. It does not run the body. A task is lazy: it starts at the first `await` or at `sync_wait`.

- Inside an `async` method, write `await task`. The method waits for the result and goes on.
- In host code, call `hot::sync_wait(task)`. The call blocks the thread and returns the value.

```cpp
hot::Task<int> t = call_sum(worker, 3, 4);  // the body did not run yet
int value = hot::sync_wait(t);              // 14
```

You can keep the task in a local variable and await it later:

```hot
async void Chain() {
    var t = Double(21);
    var v = await t;
    std.printf("%d\n", v);
}
```

A task has one awaiter. Its result can be taken once. A second `await` or a second read of the result is an error. If you destroy a task that did not finish, the runtime destroys its frame.

## Spawn a task

A script can start a task and not wait for it. Call `hot.spawn(task)`. The scheduler (see below) takes the task and runs it. The call returns a handle of type `hot.TaskHandle`.

```hot
async void Ticker(int n) {
    for (int i = 0; i < n; i = i + 1) {
        this.ticks = this.ticks + 1;
        await hot.yield();
    }
}

hot.TaskHandle Start() {
    return hot.spawn(this.Ticker(3));
}
```

- `hot.spawn` does not run the body. The next tick of the scheduler starts it.
- The variable that held the task is empty after the call. Do not use it again.
- A spawned task has no awaiter and no result. The scheduler discards the result.
- An error of a spawned task goes to the handler of unhandled script errors. `hot.TaskCancelled` is silent.
- `hot.spawn` returns a `hot.TaskHandle`. The handle does not own the task. You can copy it. The default handle means no task.
- `h.cancel()` destroys the spawned task. It returns `false` when the scheduler does not know the task.
- `h.alive()` returns `true` while the scheduler owns the task and the task is not finished.
- `h.value()` returns the id of type `uint64_t`. Use `if (h)` to test that the handle is not empty.

```hot
hot.TaskHandle Start() {
    return hot.spawn(this.Ticker(3));
}

void Stop(hot.TaskHandle h) {
    h.cancel();
}

bool IsAlive(hot.TaskHandle h) {
    return h.alive();
}
```

Host code uses `hot::scheduler()` for the same calls and for `tick()`.

## Run a task from sync code

A sync method cannot use `await`. Use one of these calls instead.

- `t.step()` starts the task and runs it to its first suspension, or to its end. It returns `true` when the task is done. A second `t.step()` of the same task does nothing.
- `t.run_until_done(timeout)` runs the task and the scheduler until the task is done. It returns `true` when the task is done. It returns `false` when the time is over.

```hot
bool Once(int x) {
    var t = this.Slow(x);
    return t.step();
}

bool Finish(int x) {
    var t = this.Slow(x);
    if (t.run_until_done(std.chrono.milliseconds(100))) {
        return true;
    }
    hot.spawn(t);
    return false;
}
```

After a timeout, the task stays alive. You still own it. Spawn it to let the scheduler finish it, or destroy it. A timeout that is zero or less starts the task and checks it once. While `run_until_done` waits, the scheduler also runs the other spawned tasks. Keep the task alive during the call: do not let another task destroy it.

## The scheduler

One scheduler exists for the whole process. The host drives it in the loop of the frame. It calls `tick()` and then `frame_boundary()`:

```cpp
#include "hot/runtime/task.h"

hot::scheduler().tick();   // run the ready tasks and the tasks with a due timer
loader.frame_boundary();
```

- `tick()` resumes each ready task once. It then destroys the tasks that are finished.
- `run(timeout)` ticks until no spawned task remains or the time is over. It returns `true` when no task remains.
- `cancel_all()` destroys all spawned tasks. Call it before `Loader::reset`. A spawned task pins its generation, and `reset` fails while a task lives.
- `size()` gives the number of spawned tasks. `alive(id)` tells if a task is still running.

Use the scheduler from one thread, usually the main thread. A task that waits for a host awaitable can resume on another thread. When it then calls `hot.yield()` or `hot.sleep(d)`, the next tick on the main thread resumes it.

Do not call `run_until_done` from a spawned task without a timeout. The call blocks the tick that runs the task, and the other tasks cannot run.

## Wait inside a task

Two awaitables work inside an `async` method of a script.

- `await hot.yield()` suspends the task. The next tick resumes it.
- `await hot.sleep(duration)` suspends the task. A tick resumes it when the time is over. Write the time as `std.chrono.milliseconds(n)`.

```hot
async int Slow(int x) {
    await hot.sleep(std.chrono.milliseconds(5));
    return x * 2;
}
```

The tick that resumes a task does not resume it twice. A task that yields in a loop runs once in each tick. Several tasks run in the order of their yields.

## Await a host awaitable

`await e` works with any C++20 awaitable, not only with a task of a script. The host can give a script an awaitable, for example a timer or a gate that the host opens later. The script writes `await` and the name of the awaitable.

```hot
async int Gated(int x) {
    var v = await asynctest.wait_gate();
    return x + v;
}
```

The script waits. When the host event occurs, the awaitable resumes the frame. Do not resume the frame with a raw `coroutine_handle`. Use `hot::detail::resume_frame` from `hot/runtime/task.h`. This call is the safe way to resume, and it also works from another thread. The example `tests/integration/async/async_gate.h` shows a full awaitable.

## Cancellation

The runtime has two exception types:

| Type | Script name | Meaning |
|---|---|---|
| `hot::TaskCancelled` | `hot.TaskCancelled` | A task ends without a result. |
| `hot::TaskReloadCancelled` | catch it as `hot.TaskCancelled` | Derives from `TaskCancelled`. A reload cancelled the task. |

A script can throw and catch `hot.TaskCancelled`. A task that ends with `TaskCancelled` is a normal cancellation. The runtime does not report it as an unhandled error.

```hot
async int Cancel() {
    throw hot.TaskCancelled("cancelled by script");
}

async int CatchCancel() {
    try {
        var v = await Cancel();
        return v;
    } catch (hot.TaskCancelled e) {
        return -7;
    }
}
```

`TaskReloadCancelled` has the message `task cancelled by reload: <what>`. A script `catch (hot.TaskCancelled e)` also catches it. If your code must not hide it, rethrow it.

If the owner destroys a task that another task awaits, the awaiter gets `TaskCancelled`.

## Reload

A reload replaces the code of a class (see [Hot reload](hot-reload.html)). The rules for tasks are these:

- A suspended task of a replaced generation ends with `TaskReloadCancelled` at its next resume. The old code of that task does not run again.
- A task that did not start, and that comes from an old generation, ends the same way when it starts.
- A sleeping spawned task of a replaced generation does not wait for its timer. The next tick cancels it. A ready spawned task ends the same way when it is resumed.
- A task keeps its generation alive. A retired generation unloads only after the task is destroyed.
- `Loader::reset` fails with `generation N is pinned (a closure or a task is alive)` while a task lives. Destroy all tasks before `reset`. For spawned tasks, call `hot::scheduler().cancel_all()`.

To keep the work across a reload, start a new task with the new code. Do not keep a suspended task of the old code.

## Build requirements

`async` needs C++20. Under C++17 the translator stops with this error:

```
'async' needs C++20: set CMAKE_CXX_STANDARD 20 before hot_add_scripts (hotc: --std=c++20)
```

| Build | What to set |
|---|---|
| `hot_add_scripts` | `set(CMAKE_CXX_STANDARD 20)` before the call. A target property is not enough. See [Options and standard](cmake-options.html). |
| `hotc` | `--std=c++20` |
| JIT and clang files backends | `DefaultBackendOptions::std_version = "c++20"` (see [Interface](embedding-interface.html)) |

Include `hot/runtime/task.h` in host code that uses `hot::Task` or `hot::sync_wait`.

## Limits

- An `async` method cannot have a reference or a pointer parameter. Pass a value.
- A class or a struct of a script cannot have a `Task<T>` field. Keep the task in a local variable or in a host object.
- `await` is not valid in a `catch` block, in a `finally` block, on the right side of `??` or in a lambda.
- A hook (for example `onReload`) cannot be `async`.
- `async` is only for class methods. There are no free `async` functions.
- A task must not live longer than its object.
- A task has one awaiter, and its result can be taken once.
- A spawned task has no awaiter and no result. An error of a spawned task goes to the handler of unhandled script errors.
- `hot.yield()` and `hot.sleep(d)` are only valid in an `async` method.
