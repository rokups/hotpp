// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The task of a script "async" function: hot::Task<T>.
// This header is empty when the compiler has no coroutine support (a C++17 build).
//
// How the translator lowers an async method. Each async method gets an outer plain function
// and a coroutine. The origin of the code is the first parameter of the coroutine:
//
//     Task<int> X__m(self, args)
//     {
//         return X__m_co(hot::detail::TaskOriginData{GEN, &hot_live, "Class::m"}, self, args);
//     }
//
// The promise has a constructor that takes the origin as its first parameter. The compiler
// calls it with the parameters of the coroutine, before the first suspension. The promise takes
// one pin of the generation there. So a task that never ran already holds its generation.
// After a reload, the first resume sees that the code is not live and cancels the task. The
// old code does not run. A member coroutine of a ".cpp" script class gets the origin from its
// object: the state struct of the class has a static hot_task_origin(). A promise that has no
// origin (a coroutine that the host wrote) has generation 0. Generation 0 is always live and
// has no pin.
//
// Rules:
//   - A task is lazy. The first resume() or the first co_await starts it.
//   - resume() starts a task that did not start. For a started task it does nothing: a child task or a
//     host awaitable resumes the frame, and only that one.
//   - A host awaitable resumes the frame with hot::detail::resume_frame(handle). This is
//     the only resume path: it checks the generation guard. It can run on another thread.
//   - One coroutine can await a task. A second awaiter gets ScriptError("task is already
//     awaited").
//   - The result moves out once. A second result() or co_await gets ScriptError("task result was
//     already taken").
//   - A reload cancels a task that is not live: the task completes with
//     TaskReloadCancelled("task cancelled by reload: <what>"). "task destroyed while awaited" is
//     a TaskCancelled. The awaiters and result() get the dynamic type. The guard runs at each
//     resume of a task frame. The frame itself does not resume after a stale reload, so its own
//     catch blocks do not run: the exception reaches the awaiters. A coroutine that the host
//     wrote and that awaits a Task is not guarded.
//   - A body that ends with TaskCancelled counts as cancelled: report_unhandled does not see it.
//     Other errors of a task that nobody awaited and nobody read with result() go to
//     report_unhandled when the task is destroyed.
//   - The destructor of a task destroys the frame, and so releases the pin. A task that is
//     awaited gets TaskCancelled in its awaiter. The destructor waits while another thread runs
//     the frame (the frame is "running" from a resume until its next suspension).
//   - The thread that runs the frame can destroy the task too (the body, or a continuation
//     that runs inline, destroys the owner). It cannot wait for itself. So the destroy is
//     deferred: the frame is destroyed when it suspends at the next co_await or when it ends. The
//     code after the destroy runs on, and it must not use the members of the frame owner.
//   - A task that awaits itself, or that closes a cycle of awaiting tasks, gets ScriptError("task
//     awaits itself") or ScriptError("task await cycle") from the co_await. A cycle through a
//     coroutine that the host wrote is not found: destroy of a task ends it.
//   - A stale handler (stale_call) must not destroy a task that it cancels.
//   - The owner must not destroy a task while a host awaitable still holds its frame (for
//     example on another thread): the host awaitable resumes a frame that does not exist then.
//     The frame counts as not running from the start of a host co_await.
//
// Scheduler rules (hot/runtime/scheduler.h):
//   - hot::yield() and hot::sleep(d) park the frame: the frame puts one entry into a scheduler
//     and suspends. The scheduler is the one that owns the frame (hot::spawn), else
//     hot::scheduler(). Only a task can await them: another coroutine fails a static_assert.
//   - An entry is weak. The owner can destroy a parked task at any time, on any thread. The
//     destroy (retire_frame) removes the entry, so the frame does not resume.
//   - A frame parks and stops running in one step, under task_mutex() and the scheduler lock. A
//     tick pops an entry and marks the frame as running in one step, under the same two locks.
//     So a destroy on another thread waits for the frame, or it removes the entry first.
//   - Lock order: task_mutex(), then the lock of one scheduler. Code that holds a scheduler lock
//     does not take task_mutex() and does not take the lock of another scheduler.
//   - A tick resumes a frame through the generation guard, like resume_frame. The timer of a
//     stale frame is due at once. A tick pops one entry at a time, so a resume can tick again.
//   - A spawned task belongs to the scheduler. A coroutine cannot await it. The scheduler
//     destroys it when it finishes, at cancel(id) or at cancel_all(). At that time an error that
//     nobody read goes to report_unhandled.
//   - spawn, tick, run, run_until_done and cancel run on one thread (the main thread). A frame
//     can park on any thread.
//   - A scheduler that dies does not resume the frames that are parked in it. A spawn or a
//     run_until_done of such a frame throws ScriptError("task was parked in a destroyed
//     scheduler"). The owner can destroy the task.
//   - run_until_done sets the scheduler of the frame only during the call. After it, the frame
//     parks in hot::scheduler() again.
//   - The task calls on_done under task_mutex(). So a scheduler that waits for a task can die
//     right after the task ends: the destroy takes task_mutex() first.
//
// All links between tasks (continuation, awaiter, running flag, scheduler entry) are under one
// lock of the runtime (detail::task_mutex).
#pragma once
#if defined(__cpp_impl_coroutine) && defined(__has_include)
    #if __has_include(<coroutine>)
        #define HOT_HAS_TASK 1
    #endif
#endif

#if defined(HOT_HAS_TASK)

    #include "hot/runtime/closure.h"
    #include "hot/runtime/errors.h"
    #include "hot/runtime/hidden.h"
    #include "hot/runtime/threads.h"

    #include <atomic>
    #include <chrono>
    #include <condition_variable>
    #include <coroutine>
    #include <cstdint>
    #include <exception>
    #include <mutex>
    #include <optional>
    #include <string>
    #include <thread>
    #include <type_traits>
    #include <utility>

namespace hot
{

template <class T> class Task;
class Scheduler;

namespace detail
{

// The lock of the links between tasks and its condition variable (src/runtime/threads.cpp).
HOT_EXPORT std::mutex& task_mutex();
HOT_EXPORT std::condition_variable& task_cv();
// The number of threads that wait on task_cv(). A frame that stops running notifies only when
// it is not zero.
HOT_EXPORT std::atomic<int>& task_waiters();

struct TaskPromiseBase;
inline std::coroutine_handle<> complete(TaskPromiseBase& p) noexcept;
inline std::coroutine_handle<> detach_frame(TaskPromiseBase& p, bool own_run) noexcept;
inline std::coroutine_handle<> retire_frame(TaskPromiseBase& p, std::coroutine_handle<> h, bool own_run) noexcept;
inline void report_discarded(TaskPromiseBase& p) noexcept;
inline void discard_frame(TaskPromiseBase& p, std::coroutine_handle<> h) noexcept;
// Returns a coroutine that destroys "dead" and then transfers to "next" (a null "next": nothing runs).
// An awaiter of a frame returns it to end its own frame. It must not destroy the frame itself: the
// MSVC ABI writes the returned handle into the frame after await_suspend returns. The coroutine
// is one for each thread (src/runtime/frame_reaper.cpp).
HOT_EXPORT std::coroutine_handle<> end_frame_after_suspend(std::coroutine_handle<> dead, std::coroutine_handle<> next) noexcept;
// Removes the entries of a parked frame from its scheduler (hot/runtime/scheduler.h). The caller
// holds task_mutex(). The function takes the lock of the scheduler.
inline void detach_parked(TaskPromiseBase& p) noexcept;

// The origin of the code of a task: the generation, the live flag and the name.
struct TaskOriginData
{
    uint32_t gen = 0;
    const uint32_t* live = nullptr;
    const char* what = nullptr;
};

// Why a task failed without an error of its body. The text is built when a reader asks for
// the result, so the paths that must not throw do not allocate.
enum class TaskFail : uint8_t
{
    None,
    Reload,    // the generation of the code is not live
    Destroyed, // the owner destroyed the task while a coroutine awaited it
};

// The awaiter of an operand of co_await: the result of operator co_await, or the operand.
template <class A> decltype(auto) get_awaiter(A&& a)
{
    if constexpr (requires { std::forward<A>(a).operator co_await(); })
        return std::forward<A>(a).operator co_await();
    else if constexpr (requires { operator co_await(std::forward<A>(a)); })
        return operator co_await(std::forward<A>(a));
    else
        return std::forward<A>(a);
}

template <class I>
concept TaskAwaiter = requires { std::remove_cvref_t<I>::hot_task_awaiter; };

// True when the exception is a TaskCancelled. It does not allocate.
inline bool is_cancellation(const std::exception_ptr& e) noexcept
{
    try
    {
        std::rethrow_exception(e);
    }
    catch (const TaskCancelled&)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// The part of a task promise that does not depend on the result type.
// The pin is the first member, so the runtime destroys it last. The generation code stays
// loaded until the runtime destroys all other members (the same rule as GuardedFn).
// The lock task_mutex() guards: the links (continuation, parent, awaiting, awaiter_slot), on_done,
// started, awaited, sched, parked_in, orphaned. The flags finished and running are atomic. A writer sets finished under the
// lock, with release order. done() and await_ready read it with acquire order, without the lock.
// A thread sets running only when it starts to run the frame, with run_thread set first.
struct TaskPromiseBase
{
    GenerationPin pin;
    const uint32_t* live;               // null: the guard compares the generation with active_generation
    const char* what;                   // the name of the function, for the error text
    std::coroutine_handle<> continuation; // the coroutine that awaits this task
    TaskPromiseBase* parent = nullptr;  // the promise of the continuation, when it is a task
    TaskPromiseBase* awaiting = nullptr; // the task that this task awaits now
    TaskPromiseBase** awaiter_slot = nullptr; // the link field of the awaiter, cleared when the promise dies
    void (*on_done)(void*) = nullptr;   // called under the lock when no coroutine awaits this task
    void* on_done_ctx = nullptr;
    std::exception_ptr error;
    TaskFail fail = TaskFail::None;
    std::atomic<bool> finished{false};
    bool started = false;
    bool awaited = false;
    std::atomic<bool> running{false}; // a thread runs the frame: from a resume to the next suspension
    std::atomic<std::thread::id> run_thread{}; // the thread that runs the frame, valid while running
    bool destroy_requested = false; // the owner destroyed the task from its own run: see retire_frame
    bool observed = false; // a coroutine awaited the task, or result() ran
    bool taken = false;    // the result moved out
    Scheduler* sched = nullptr;     // the scheduler that owns the frame (hot::spawn), or null
    Scheduler* parked_in = nullptr; // the scheduler that holds the one entry of the parked frame, or null
    bool orphaned = false; // the frame was parked in a scheduler that died: nothing resumes it

    // A promise without an origin: generation 0, always live.
    TaskPromiseBase() : pin(0), live(nullptr), what(nullptr) {}
    explicit TaskPromiseBase(const TaskOriginData& o) : pin(o.gen), live(o.live), what(o.what) {}
    TaskPromiseBase(const TaskPromiseBase&) = delete;
    TaskPromiseBase& operator=(const TaskPromiseBase&) = delete;
    ~TaskPromiseBase()
    {
        std::lock_guard<std::mutex> lock(task_mutex());
        if (awaiter_slot)
            *awaiter_slot = nullptr;
        if (parent && parent->awaiting == this)
            parent->awaiting = nullptr;
        if (HOT_UNLIKELY(parked_in)) // retire_frame detaches first: this is only a second check
            detach_parked(*this);
    }

    // True when the code of the frame is not live now (design 6.3). Generation 0 is always live.
    bool stale() const noexcept { return live ? !*live : pin.gen != 0 && pin.gen != active_generation; }

    bool is_finished() const noexcept { return finished.load(std::memory_order_acquire); }

    // The calling thread starts to run the frame. Run it before the frame resumes.
    void begin_run() noexcept
    {
        run_thread.store(std::this_thread::get_id());
        running.store(true);
    }
    // The frame stops running. The caller holds the lock when it sets "locked".
    void end_run(bool locked) noexcept
    {
        running.store(false);
        if (task_waiters().load() > 0)
        {
            if (locked)
                task_cv().notify_all();
            else
            {
                std::lock_guard<std::mutex> lock(task_mutex());
                task_cv().notify_all();
            }
        }
    }
    // True when the calling thread runs the frame now.
    bool runs_on_this_thread() const noexcept
    {
        return running.load() && run_thread.load() == std::this_thread::get_id();
    }

    // Throws the failure of the task, if it has one. It may allocate.
    void rethrow_failure() const
    {
        if (fail == TaskFail::Reload)
            throw TaskReloadCancelled(std::string("task cancelled by reload: ") + (what ? what : "?"));
        if (fail == TaskFail::Destroyed)
            throw TaskCancelled("task destroyed while awaited");
        if (error)
            std::rethrow_exception(error);
    }
    // The failure, then the check that the result moves out once.
    void claim_result()
    {
        observed = true;
        rethrow_failure();
        if (taken)
            throw ScriptError("task result was already taken");
        taken = true;
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }
    void unhandled_exception() noexcept { error = std::current_exception(); }

    // The awaiter of the end of the frame: it completes the task and moves to the continuation.
    struct FinalAwaiter
    {
        bool await_ready() const noexcept { return false; }
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept;
        void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() const noexcept { return {}; }

    // The wrapper of each co_await in a task body. The frame stops running when it suspends,
    // so the owner can destroy it. Task awaiters do this under the lock by themselves.
    template <class Inner> struct Wrapped
    {
        template <class A>
        Wrapped(A&& a, TaskPromiseBase* s)
            : inner(get_awaiter(std::forward<A>(a)))
            , self(s)
        {
        }
        Inner inner;
        TaskPromiseBase* self;

        bool await_ready() { return inner.await_ready(); }
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h)
        {
            // The owner destroyed the task from its own run: the frame ends here.
            if (HOT_UNLIKELY(self->destroy_requested))
            {
                // The frame lives until this function returns: see end_frame_after_suspend.
                std::coroutine_handle<> next = detach_frame(*self, true);
                report_discarded(*self);
                return end_frame_after_suspend(h, next);
            }
            if constexpr (TaskAwaiter<Inner>)
                return inner.await_suspend(h);
            else
            {
                self->end_run(false); // the frame is suspended from here
                try
                {
                    using R = decltype(inner.await_suspend(h));
                    if constexpr (std::is_void_v<R>)
                    {
                        inner.await_suspend(h);
                        return std::noop_coroutine();
                    }
                    else if constexpr (std::is_same_v<R, bool>)
                    {
                        if (inner.await_suspend(h))
                            return std::noop_coroutine();
                        self->begin_run(); // the coroutine goes on
                        return h;
                    }
                    else
                    {
                        std::coroutine_handle<> r = inner.await_suspend(h);
                        if (r == std::coroutine_handle<>(h))
                            self->begin_run();
                        return r;
                    }
                }
                catch (...)
                {
                    self->begin_run(); // the exception comes out of the co_await in the body
                    throw;
                }
            }
        }
        decltype(auto) await_resume() { return inner.await_resume(); }
    };

    template <class A> auto await_transform(A&& a)
    {
        return Wrapped<decltype(get_awaiter(std::declval<A>()))>(std::forward<A>(a), this);
    }
};

// Marks "p" as finished. Returns the coroutine that must run next: the continuation, or a
// null handle. When nothing awaits the task, it calls on_done under the lock. An on_done function
// must not take task_mutex(). It can take a lock that comes after it in the lock order: the lock
// of a scheduler, or the lock of sync_wait. A stale parent is cancelled in
// a loop, so a chain of awaiting tasks does not grow the stack. The parent that continues is
// marked as running under the lock: its owner cannot destroy it until it suspends again.
// The function calls stale_call without the lock.
inline std::coroutine_handle<> complete(TaskPromiseBase& p) noexcept
{
    std::unique_lock<std::mutex> lock(task_mutex());
    TaskPromiseBase* cur = &p;
    for (;;)
    {
        cur->finished.store(true, std::memory_order_release);
        cur->end_run(true);
        std::coroutine_handle<> next = cur->continuation;
        TaskPromiseBase* parent = cur->parent;
        if (!next)
        {
            // Under the lock: the owner of on_done removes it under this lock, and the owner of
            // the task destroys it only after this lock. So "ctx" lives during the call.
            if (cur->on_done)
                cur->on_done(cur->on_done_ctx);
            return {}; // the lock drops: "cur" can be destroyed from here
        }
        if (!parent) // a coroutine that the host wrote: not guarded
            return next;
        cur->parent = nullptr; // the link ends here: the parent can die before this task
        parent->awaiting = nullptr;
        parent->begin_run(); // the parent is mine from here: its owner waits or defers
        if (!parent->stale())
            return next;
        // The parent is stale: it does not resume. It completes with an error.
        parent->fail = TaskFail::Reload;
        const char* what = parent->what;
        lock.unlock();
        stale_call(what); // the handler can use the task API
        lock.lock();
        cur = parent;
    }
}

// Runs a frame that the caller marked as running under the lock, with "stale" read under the
// same lock. The caller holds a ScriptEntry that it made before the lock. A stale frame does
// not resume: it completes with TaskFail::Reload.
inline void run_frame(TaskPromiseBase& p, std::coroutine_handle<> h, bool stale) noexcept
{
    if (HOT_UNLIKELY(stale))
    {
        stale_call(p.what);
        {
            std::lock_guard<std::mutex> lock(task_mutex());
            p.fail = TaskFail::Reload;
        }
        std::coroutine_handle<> next = complete(p);
        if (next)
            next.resume();
        return;
    }
    h.resume();
}

// The only way to start or resume a frame from outside. It checks the generation guard first.
inline void resume_frame(TaskPromiseBase& p, std::coroutine_handle<> h) noexcept
{
    ScriptEntry entry; // design 6.3
    bool stale = false;
    {
        std::lock_guard<std::mutex> lock(task_mutex());
        stale = p.stale();
        p.begin_run(); // a stale frame is mine too: its owner cannot destroy it now
    }
    run_frame(p, h, stale);
}

template <class P> inline void resume_frame(std::coroutine_handle<P> h) noexcept
{
    resume_frame(static_cast<TaskPromiseBase&>(h.promise()), std::coroutine_handle<>(h));
}

// Reports the error of a task that nobody read. The frame is finished or is about to be destroyed.
inline void report_discarded(TaskPromiseBase& p) noexcept
{
    if (p.is_finished() && p.error && !p.observed && !is_cancellation(p.error))
        report_unhandled(nullptr, p.error);
}

// Reports the error of a task that nobody read, then destroys the frame. The frame is finished
// and no thread uses it.
inline void discard_frame(TaskPromiseBase& p, std::coroutine_handle<> h) noexcept
{
    report_discarded(p);
    h.destroy();
}

// Removes all links of the frame of "p" before its destroy. "own_run" is true when the calling
// thread runs the frame and it is suspended at this point (a deferred destroy). Otherwise the
// function waits for the thread that runs the frame. A coroutine that awaits the task gets
// TaskCancelled. The function returns the coroutine that must run next, or a null handle. Only
// the caller can destroy the frame after it.
inline std::coroutine_handle<> detach_frame(TaskPromiseBase& p, bool own_run) noexcept
{
    bool cancel = false;
    {
        std::unique_lock<std::mutex> lock(task_mutex());
        if (own_run)
            p.end_run(true);
        else if (p.running.load())
        {
            // Another thread runs the frame. Wait until it suspends or finishes.
            task_waiters().fetch_add(1);
            task_cv().wait(lock, [&] { return !p.running.load(); });
            task_waiters().fetch_sub(1);
        }
        // A scheduler must not resume the frame from here. The frame parks only while it runs,
        // and a scheduler pops an entry only under this lock: so the detach is final.
        if (p.parked_in)
            detach_parked(p);
        // The frame does not run now, and the lock stays: the link to the child is final. The
        // wait comes first, because a running frame can link a new child.
        if (p.awaiting) // this task awaits a child: the child must not resume it
        {
            TaskPromiseBase* c = p.awaiting;
            p.awaiting = nullptr;
            if (!c->finished)
            {
                c->continuation = {};
                c->parent = nullptr;
                c->awaited = false;
                c->observed = false;
            }
        }
        if (!p.finished && p.awaited)
        {
            cancel = true;
            p.fail = TaskFail::Destroyed;
        }
    }
    std::coroutine_handle<> next;
    if (cancel) // a coroutine awaits the task: it gets an error and goes on
        next = complete(p);
    return next;
}

// Ends the life of the frame "h" for its owner: detach_frame, then the destroy. The function
// returns the coroutine that must run next, or a null handle. The frame is destroyed when it
// returns: the caller must not use "p" or "h" after it.
inline std::coroutine_handle<> retire_frame(TaskPromiseBase& p, std::coroutine_handle<> h, bool own_run) noexcept
{
    std::coroutine_handle<> next = detach_frame(p, own_run);
    discard_frame(p, h);
    return next;
}

// Starts the frame "h" when it did not start. For a started frame it does nothing.
inline void start_frame(TaskPromiseBase& p, std::coroutine_handle<> h) noexcept
{
    {
        std::lock_guard<std::mutex> lock(task_mutex());
        if (p.started)
            return;
        p.started = true;
    }
    resume_frame(p, h);
}

// Ends the frame "h" for its owner (the destructor of a task, or a scheduler). When the calling
// thread runs the frame, the destroy is deferred to the next suspension or the end of the frame.
inline void destroy_frame(TaskPromiseBase& p, std::coroutine_handle<> h) noexcept
{
    if (p.runs_on_this_thread())
    {
        // The body, or a continuation that runs inline, destroys its own task. This thread
        // cannot wait for itself. The frame is destroyed when it suspends or ends.
        p.destroy_requested = true;
        return;
    }
    std::coroutine_handle<> next = retire_frame(p, h, false);
    if (next)
    {
        ScriptEntry entry;
        next.resume();
    }
}

template <class P>
inline std::coroutine_handle<> TaskPromiseBase::FinalAwaiter::await_suspend(std::coroutine_handle<P> h) const noexcept
{
    // After complete() another thread can destroy the frame. Only the own request is safe to read.
    TaskPromiseBase& p = h.promise();
    const bool own_destroy = p.destroy_requested;
    std::coroutine_handle<> next = complete(p);
    if (own_destroy) // the owner destroyed the task from its own run: nobody else does
    {
        report_discarded(p);
        return end_frame_after_suspend(h, next); // the frame lives until this function returns
    }
    return next ? next : std::noop_coroutine();
}

// The result type part of the promise.
template <class T> struct TaskPromise : TaskPromiseBase
{
    std::optional<T> value;
    TaskPromise() = default;
    // The compiler calls it with the parameters of the coroutine when the first one is an origin.
    template <class... A>
    explicit TaskPromise(const TaskOriginData& o, A&&...)
        : TaskPromiseBase(o)
    {
    }
    // A member coroutine of a ".cpp" script class: the object gives the origin.
    template <class S, class... A>
        requires requires { std::remove_cvref_t<S>::hot_task_origin(); }
    explicit TaskPromise(S&& s, A&&...)
        : TaskPromiseBase(std::remove_cvref_t<S>::hot_task_origin())
    {
        (void)s;
    }
    Task<T> get_return_object() noexcept;
    template <class U> void return_value(U&& v) { value.emplace(std::forward<U>(v)); }
    // Moves the value out, or throws the failure. The task must be finished.
    T take()
    {
        claim_result();
        return std::move(*value);
    }
};

template <> struct TaskPromise<void> : TaskPromiseBase
{
    TaskPromise() = default;
    template <class... A>
    explicit TaskPromise(const TaskOriginData& o, A&&...)
        : TaskPromiseBase(o)
    {
    }
    // A member coroutine of a ".cpp" script class: the object gives the origin.
    template <class S, class... A>
        requires requires { std::remove_cvref_t<S>::hot_task_origin(); }
    explicit TaskPromise(S&& s, A&&...)
        : TaskPromiseBase(std::remove_cvref_t<S>::hot_task_origin())
    {
        (void)s;
    }
    Task<void> get_return_object() noexcept;
    void return_void() noexcept {}
    void take() { claim_result(); }
};

} // namespace detail

// The owner of a coroutine frame. It is move-only. A default task is empty.
template <class T> class Task
{
public:
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle h) noexcept : h_(h) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept
    {
        if (this != &o)
        {
            destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    ~Task() { destroy(); }

    bool valid() const noexcept { return static_cast<bool>(h_); }
    bool done() const noexcept { return h_ && h_.promise().is_finished(); }

    // Starts a task that did not start. For a started task it does nothing: a child task or a
    // host awaitable resumes the frame. It does not throw: an error of the body goes to result().
    void resume() noexcept
    {
        if (h_)
            detail::start_frame(h_.promise(), h_);
    }

    // Starts the task and runs it to its first suspension or to its end. Returns done(). A
    // started task does not resume again. An empty task throws ScriptError("empty task").
    // Defined in scheduler.h.
    bool step();

    // Starts the task and ticks the process scheduler until the task is done or the time is over.
    // Returns done(). See Scheduler::run_until_done. Defined in scheduler.h.
    bool run_until_done(std::chrono::steady_clock::duration timeout);

    // Moves the frame out: the task is empty after it. The caller owns the frame from here.
    Handle release() noexcept { return std::exchange(h_, {}); }

    // Moves the value out once, or rethrows the error of the body.
    T result()
    {
        if (!h_)
            throw ScriptError("empty task");
        promise_type& p = h_.promise();
        if (!p.is_finished())
            throw ScriptError("task is not done");
        return p.take();
    }

    // The base of the promise. sync_wait uses it.
    detail::TaskPromiseBase& base() noexcept { return h_.promise(); }

    // co_await on a task starts it (when it did not start) and resumes the awaiting coroutine
    // when it is done. The awaiter holds the promise, not the Task: a move of the Task is safe.
    class Awaiter
    {
    public:
        static constexpr bool hot_task_awaiter = true;

        explicit Awaiter(promise_type* p) noexcept : p_(p) {}
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        ~Awaiter()
        {
            // The parent frame dies while it awaits: the child must not resume it. The promise of
            // the child clears link_ under the lock when it dies, so read link_ only under the lock.
            if (registered_)
            {
                std::lock_guard<std::mutex> lock(detail::task_mutex());
                if (link_ && link_->awaiter_slot == &link_)
                {
                    if (!link_->finished && link_->awaited && !link_->parent)
                    {
                        link_->continuation = {};
                        link_->awaited = false;
                        link_->observed = false;
                    }
                    link_->awaiter_slot = nullptr;
                }
            }
        }

        bool await_ready() const noexcept { return !p_ || p_->is_finished(); }

        // After the lock drops, this function does not touch the awaiter: the owner can
        // destroy the parent frame (and so the awaiter) when the parent is not running.
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept
        {
            promise_type* cp = p_;
            detail::TaskPromiseBase* par = nullptr;
            if constexpr (std::is_base_of_v<detail::TaskPromiseBase, P>)
                par = &parent.promise();
            {
                std::lock_guard<std::mutex> lock(detail::task_mutex());
                if (cp->finished)
                    return parent; // the task finished after await_ready
                if (cp->awaited || cp->on_done)
                {
                    fail_ = "task is already awaited"; // await_resume throws it
                    return parent;
                }
                if (par) // the parent must not be in the chain of tasks that "cp" awaits
                {
                    for (detail::TaskPromiseBase* x = cp; x; x = x->awaiting)
                    {
                        if (x == par)
                        {
                            fail_ = cp == par ? "task awaits itself" : "task await cycle";
                            return parent;
                        }
                    }
                }
                cp->awaited = true;
                cp->observed = true;
                cp->continuation = parent;
                cp->parent = par;
                link_ = cp;
                registered_ = true;
                cp->awaiter_slot = &link_;
                if (par)
                {
                    par->awaiting = cp;
                    par->end_run(true); // the parent is suspended from here
                }
                if (cp->started)
                    return std::noop_coroutine(); // it runs or waits already: only its end resumes the parent
                cp->started = true;
                cp->begin_run(); // the child runs on this thread by the transfer below
            }
            if (HOT_UNLIKELY(cp->stale()))
            {
                // The code is not live: the task does not start. The parent gets the error.
                stale_call(cp->what);
                {
                    std::lock_guard<std::mutex> lock(detail::task_mutex());
                    cp->fail = detail::TaskFail::Reload;
                }
                std::coroutine_handle<> next = detail::complete(*cp);
                return next ? next : std::noop_coroutine();
            }
            return Handle::from_promise(*cp); // symmetric transfer: the child runs now
        }

        T await_resume()
        {
            if (!p_)
                throw ScriptError("empty task");
            if (fail_)
                throw ScriptError(fail_);
            if (registered_)
            {
                bool gone;
                {
                    std::lock_guard<std::mutex> lock(detail::task_mutex());
                    gone = !link_; // the promise died and cleared the link
                    if (link_ && link_->awaiter_slot == &link_)
                        link_->awaiter_slot = nullptr;
                    link_ = nullptr;
                }
                if (gone) // the owner destroyed the task: the promise does not exist now
                    throw TaskCancelled("task destroyed while awaited");
            }
            return p_->take();
        }

    private:
        promise_type* p_;
        detail::TaskPromiseBase* link_ = nullptr; // set while the awaiter is linked to the promise
        const char* fail_ = nullptr;
        bool registered_ = false; // await_suspend linked the awaiter
    };
    Awaiter operator co_await() & noexcept { return Awaiter{h_ ? &h_.promise() : nullptr}; }
    Awaiter operator co_await() && noexcept { return Awaiter{h_ ? &h_.promise() : nullptr}; }

private:
    void destroy() noexcept
    {
        Handle h = std::exchange(h_, {});
        if (h)
            detail::destroy_frame(h.promise(), h);
    }

    Handle h_;
};

namespace detail
{
template <class T> inline Task<T> TaskPromise<T>::get_return_object() noexcept
{
    return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}
inline Task<void> TaskPromise<void>::get_return_object() noexcept
{
    return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}
} // namespace detail

// Starts the task (when it did not start) and blocks the calling thread until it is done. A
// host awaitable can resume the frame on another thread. The task can finish at any time.
template <class T> T sync_wait(Task<T>& t)
{
    if (!t.valid())
        throw ScriptError("empty task");
    t.resume();
    struct State
    {
        std::mutex m;
        std::condition_variable cv;
        bool finished = false;
    } st;
    detail::TaskPromiseBase& p = t.base();
    bool wait = false;
    {
        // The completion reads on_done under the link lock. So the task finishes before the
        // check, or it calls on_done after it: no wake-up is lost.
        std::lock_guard<std::mutex> lock(detail::task_mutex());
        if (!p.finished)
        {
            if (p.awaited || p.on_done)
                throw ScriptError("task is already awaited");
            p.on_done_ctx = &st;
            p.on_done = [](void* ctx)
            {
                State& s = *static_cast<State*>(ctx);
                std::lock_guard<std::mutex> l(s.m);
                s.finished = true;
                s.cv.notify_all(); // under the lock: the waiter cannot destroy the state before
            };
            wait = true;
        }
    }
    if (wait)
    {
        {
            std::unique_lock<std::mutex> lock(st.m);
            st.cv.wait(lock, [&] { return st.finished; });
        }
        std::lock_guard<std::mutex> lock(detail::task_mutex());
        p.on_done = nullptr; // the state dies with this function
        p.on_done_ctx = nullptr;
    }
    return t.result();
}

template <class T> T sync_wait(Task<T>&& t) { return sync_wait(t); }

} // namespace hot

    #include "hot/runtime/scheduler.h"

#endif // HOT_HAS_TASK
