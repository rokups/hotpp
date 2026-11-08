// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The task scheduler: hot::spawn, hot::TaskHandle, Task::step, Task::run_until_done, hot::yield
// and hot::sleep.
// This header is empty when the compiler has no coroutine support (a C++17 build).
// task.h includes it. The rules are in the comment at the top of task.h ("Scheduler rules").
//
// A scheduler has two kinds of data:
//   - The owned tasks. hot::spawn moves the frame of a task into the scheduler. The scheduler
//     destroys the frame when it finishes (at a tick), at cancel(id) or at cancel_all().
//   - The entries. A frame that awaits hot::yield() or hot::sleep(d) parks: it puts one entry
//     into a scheduler and suspends. An entry is weak: the frame can die while it is parked,
//     and its death removes the entry. A tick resumes the due entries one by one.
//
// The host calls tick() once in each frame. Before Loader::reset it calls cancel_all(): a
// spawned task pins the generation of its code.
#pragma once
#include "hot/runtime/task.h"

#if defined(HOT_HAS_TASK)

    #include <algorithm>
    #include <chrono>
    #include <condition_variable>
    #include <coroutine>
    #include <cstddef>
    #include <cstdint>
    #include <deque>
    #include <mutex>
    #include <type_traits>
    #include <vector>

namespace hot
{

using TaskId = uint64_t; // 0 is no task
using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;

struct SleepAwaiter;

namespace detail
{
// The time "d" after "now", without an overflow. A duration that is not positive gives "now".
inline Clock::time_point time_after(Clock::time_point now, Duration d) noexcept
{
    if (d <= Duration::zero())
        return now;
    if (d >= Clock::time_point::max() - now)
        return Clock::time_point::max();
    return now + d;
}
} // namespace detail

class Scheduler
{
public:
    Scheduler() = default;
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    // Destroys the owned tasks. A frame that is parked here and that the scheduler does not own
    // stays suspended: it does not resume. Its owner can destroy it.
    ~Scheduler();

    // Moves the frame of "t" into the scheduler: "t" is empty after it. The body does not run
    // here. The next tick starts a task that did not start. The result is discarded. An error
    // of the body goes to report_unhandled when the scheduler destroys the frame. A
    // TaskCancelled is silent. An empty task, or a task that a coroutine awaits, throws.
    template <class T> TaskId spawn(Task<T>& t);
    template <class T> TaskId spawn(Task<T>&& t) { return spawn(t); }

    // Moves the due timers to the ready queue, resumes each ready entry once, then destroys
    // the owned tasks that are finished. A timer of a stale frame is due at once. An entry that
    // a frame adds during the tick waits for the next tick. Returns the number of resumes.
    size_t tick() { return tick(Clock::now()); }
    size_t tick(Clock::time_point now);

    // Ticks until no owned task remains or the time is over. Between ticks it waits for the
    // next timer, a new entry or the end of a task. Returns true when no owned task remains.
    bool run(Duration timeout);

    // Starts "t" and ticks until "t" is done or the time is over. Returns true when "t" is done.
    // On a timeout the task stays alive and parked: the caller owns it. During the call the
    // frame parks in this scheduler. After the call a new park goes to hot::scheduler(). A
    // timeout that is not positive starts the task and checks it once. A task that a coroutine
    // awaits throws ScriptError("task is already awaited"). The caller must keep "t" alive
    // during the call: a tick runs other tasks, and the call reads the task after each tick.
    template <class T> bool run_until_done(Task<T>& t, Duration timeout);

    // Destroys an owned task, like the destructor of a task. Returns false when the scheduler
    // does not own "id". A task that cancels itself ends at its next suspension.
    bool cancel(TaskId id) noexcept;
    void cancel_all() noexcept;
    // True while the scheduler owns "id" and the task is not finished.
    bool alive(TaskId id) const noexcept;
    // The number of owned tasks.
    size_t size() const noexcept;
    // The time of the next due entry: time_point::min() when an entry is ready now,
    // time_point::max() when no entry exists.
    Clock::time_point next_wake() const noexcept;

    // Parks the frame "h" in this scheduler until "when" (time_point::min(): the next tick). The
    // calling thread runs the frame, and the frame suspends after the call. The frame stops
    // running in the call.
    void park(detail::TaskPromiseBase& p, std::coroutine_handle<> h, Clock::time_point when);
    // Removes the entry of the frame. The caller holds detail::task_mutex().
    void detach(detail::TaskPromiseBase& p) noexcept;
    // Wakes a thread that waits in run() or run_until_done().
    void notify() noexcept;

private:
    friend struct SleepAwaiter;

    struct Entry
    {
        detail::TaskPromiseBase* p;
        std::coroutine_handle<> h;
        Clock::time_point when;
        uint64_t seq; // the order of the parks: a tick resumes only the entries before its start
    };
    struct Owned
    {
        TaskId id;
        detail::TaskPromiseBase* p;
        std::coroutine_handle<> h;
    };

    // The caller holds detail::task_mutex().
    void park_locked(detail::TaskPromiseBase& p, std::coroutine_handle<> h, Clock::time_point when);
    void adopt_locked(detail::TaskPromiseBase& p);
    bool find_entry(const detail::TaskPromiseBase& p, Entry& out) const noexcept;
    void push_entry_locked(Entry e);
    // The caller holds m_.
    Clock::time_point next_wake_locked() const noexcept;
    uint64_t wake_seq() const noexcept;
    // Waits until a park or a notify after "seq", the next timer or "deadline".
    void wait_work(uint64_t seq, Clock::time_point deadline) const;
    void reap();
    static void on_task_done(void* ctx) noexcept { static_cast<Scheduler*>(ctx)->notify(); }

    // The lock of the scheduler. Lock order: detail::task_mutex(), then m_. Code that holds m_
    // never takes task_mutex() and never takes the lock of another scheduler.
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    uint64_t wake_ = 0; // incremented by each park and notify
    uint64_t seq_ = 0;
    TaskId next_id_ = 1;
    std::deque<Entry> ready_;
    std::vector<Entry> timers_;
    std::vector<Owned> owned_;
};

// The scheduler of the process. The first call makes it. The runtime never destroys it.
// All modules of the process (the host, each script module) share this one object through the
// exported slot. So each module must use the same Scheduler layout: the same version of this
// header and the same standard library (std::mutex, std::deque, std::vector) as the host.
inline Scheduler& scheduler()
{
    return *static_cast<Scheduler*>(detail::default_scheduler([]() -> void* { return new Scheduler; }));
}

// The awaiter of hot::yield() and hot::sleep(d). Only a task can await it. The frame parks in
// the scheduler that owns it (hot::spawn), else in hot::scheduler().
struct SleepAwaiter
{
    // The frame stops running in park_locked, under the lock: not in the wrapper of the co_await.
    static constexpr bool hot_task_awaiter = true;

    Duration delay{};
    bool next_tick = false; // hot::yield(): the next tick resumes the frame
    std::exception_ptr thrown; // the throw of the park: the co_await throws it

    bool await_ready() const noexcept { return false; }
    template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept
    {
        static_assert(std::is_base_of_v<detail::TaskPromiseBase, P>,
            "hot::yield() and hot::sleep() work only inside a task (an async method)");
        return detail::after_suspend(&SleepAwaiter::park<P>, this, h); // see detail::after_suspend
    }
    // The work of the co_await. It runs in the trampoline.
    template <class P> static std::coroutine_handle<> park(void* ctx, std::coroutine_handle<> h) noexcept
    {
        SleepAwaiter& a = *static_cast<SleepAwaiter*>(ctx);
        detail::TaskPromiseBase& p = std::coroutine_handle<P>::from_address(h.address()).promise();
        try
        {
            Scheduler& fallback = scheduler();
            const Clock::time_point when = a.next_tick ? Clock::time_point::min() : detail::time_after(Clock::now(), a.delay);
            std::lock_guard<std::mutex> lock(detail::task_mutex());
            Scheduler& s = p.sched ? *p.sched : fallback;
            s.park_locked(p, h, when); // a throw leaves the frame running: the co_await throws
            // From here another thread can resume or destroy the frame: do not touch "a".
        }
        catch (...)
        {
            a.thrown = std::current_exception();
            return h;
        }
        return {};
    }
    void await_resume()
    {
        if (HOT_UNLIKELY(thrown))
            std::rethrow_exception(std::exchange(thrown, nullptr));
    }
};

// The frame waits until the next tick.
inline SleepAwaiter yield() noexcept
{
    return SleepAwaiter{Duration::zero(), true};
}
// The frame waits until "d" is over, then until the next tick.
inline SleepAwaiter sleep(Duration d) noexcept
{
    return SleepAwaiter{d, false};
}

// A copyable name of a spawned task. It does not own the task. It refers to the id in the
// process scheduler (hot::scheduler()). A handle of a finished or cancelled task is not alive.
struct TaskHandle
{
    TaskId id = 0; // 0 is no task

    explicit operator bool() const noexcept { return id != 0; }
    uint64_t value() const noexcept { return id; }

    // Destroys the task. Returns false when the scheduler does not know the id.
    bool cancel() const noexcept { return scheduler().cancel(id); }
    // True while the scheduler owns the task and the task is not finished.
    bool alive() const noexcept { return scheduler().alive(id); }
};

// Moves the task into the process scheduler. See Scheduler::spawn.
template <class T> TaskHandle spawn(Task<T>& t)
{
    return TaskHandle{scheduler().spawn(t)};
}
template <class T> TaskHandle spawn(Task<T>&& t)
{
    return TaskHandle{scheduler().spawn(t)};
}

// Runs the task once, on the calling thread: it starts the frame, and the frame runs to its
// first suspension or to its end. A started task does not resume again. Returns done(). The
// caller keeps the task.
template <class T> bool Task<T>::step()
{
    if (!valid())
        throw ScriptError("empty task");
    resume();
    return done();
}

// Runs the task in the process scheduler. See Scheduler::run_until_done.
template <class T> bool Task<T>::run_until_done(std::chrono::steady_clock::duration timeout)
{
    return scheduler().run_until_done(*this, timeout);
}

// ---------------------------------------------------------------------------- implementation

namespace detail
{
inline void detach_parked(TaskPromiseBase& p) noexcept
{
    p.parked_in->detach(p);
}
} // namespace detail

inline Scheduler::~Scheduler()
{
    cancel_all();
    std::lock_guard<std::mutex> tl(detail::task_mutex());
    std::lock_guard<std::mutex> sl(m_);
    // The owned frames are gone. A frame that is parked here belongs to another owner: no
    // scheduler resumes it from here, and it must not use this scheduler again.
    auto orphan = [this](Entry& e)
    {
        e.p->parked_in = nullptr;
        e.p->orphaned = true;
        if (e.p->sched == this)
            e.p->sched = nullptr;
    };
    std::for_each(ready_.begin(), ready_.end(), orphan);
    std::for_each(timers_.begin(), timers_.end(), orphan);
    ready_.clear();
    timers_.clear();
}

inline void Scheduler::push_entry_locked(Entry e)
{
    if (e.when == Clock::time_point::min())
        ready_.push_back(e);
    else
        timers_.push_back(e);
    ++wake_;
    cv_.notify_all();
}

inline void Scheduler::park_locked(detail::TaskPromiseBase& p, std::coroutine_handle<> h, Clock::time_point when)
{
    std::lock_guard<std::mutex> lock(m_);
    push_entry_locked(Entry{&p, h, when, seq_++}); // a throw changes nothing
    p.parked_in = this;
    // In the same step as the park: a destroy on another thread waits for the frame until here,
    // and then it sees the entry and removes it.
    p.end_run(true);
}

inline void Scheduler::park(detail::TaskPromiseBase& p, std::coroutine_handle<> h, Clock::time_point when)
{
    std::lock_guard<std::mutex> lock(detail::task_mutex());
    park_locked(p, h, when);
}

inline void Scheduler::detach(detail::TaskPromiseBase& p) noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    auto mine = [&](const Entry& e) { return e.p == &p; };
    ready_.erase(std::remove_if(ready_.begin(), ready_.end(), mine), ready_.end());
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), mine), timers_.end());
    p.parked_in = nullptr;
}

inline bool Scheduler::find_entry(const detail::TaskPromiseBase& p, Entry& out) const noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    for (const Entry& e : ready_)
    {
        if (e.p == &p)
        {
            out = e;
            return true;
        }
    }
    for (const Entry& e : timers_)
    {
        if (e.p == &p)
        {
            out = e;
            return true;
        }
    }
    return false;
}

// The frame parks in this scheduler from here. An entry in another scheduler moves here. The
// caller holds task_mutex(), so no scheduler pops the entry in the move. The two scheduler
// locks are taken one after the other, never together.
inline void Scheduler::adopt_locked(detail::TaskPromiseBase& p)
{
    // The entry of the frame died with its scheduler. Nothing can resume the frame.
    if (p.orphaned)
        throw ScriptError("task was parked in a destroyed scheduler");
    p.sched = this;
    Scheduler* other = p.parked_in;
    if (!other || other == this)
        return;
    Entry e{};
    if (other->find_entry(p, e))
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            e.seq = seq_++;
            push_entry_locked(e); // a throw leaves the entry in the other scheduler
        }
        other->detach(p); // it clears parked_in
        p.parked_in = this;
    }
}

template <class T> TaskId Scheduler::spawn(Task<T>& t)
{
    if (!t.valid())
        throw ScriptError("empty task");
    detail::TaskPromiseBase& p = t.base();
    const std::coroutine_handle<> h = Task<T>::Handle::from_promise(static_cast<typename Task<T>::promise_type&>(p));
    TaskId id = 0;
    {
        std::lock_guard<std::mutex> tl(detail::task_mutex());
        // A coroutine that awaits the task reads its promise after the end: the frame must live.
        if (p.awaited || p.on_done)
            throw ScriptError("task is already awaited");
        adopt_locked(p);
        std::lock_guard<std::mutex> sl(m_);
        owned_.reserve(owned_.size() + 1);
        if (!p.started) // the next tick starts the frame
        {
            push_entry_locked(Entry{&p, h, Clock::time_point::min(), seq_++});
            p.parked_in = this;
        }
        // No throw from here.
        id = next_id_++;
        owned_.push_back(Owned{id, &p, h});
        if (!p.finished)
        {
            p.on_done = &Scheduler::on_task_done; // a frame that ends on another thread wakes run()
            p.on_done_ctx = this;
        }
        (void)t.release();
        ++wake_;
        cv_.notify_all();
    }
    return id;
}

inline size_t Scheduler::tick(Clock::time_point now)
{
    uint64_t limit = 0;
    {
        std::lock_guard<std::mutex> lock(m_);
        const size_t first = ready_.size();
        for (size_t i = 0; i < timers_.size();)
        {
            const Entry& e = timers_[i];
            if (e.when <= now || e.p->stale()) // a stale frame is cancelled at once
            {
                ready_.push_back(e);
                timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
                ++i;
        }
        std::sort(ready_.begin() + static_cast<std::ptrdiff_t>(first), ready_.end(),
            [](const Entry& a, const Entry& b) { return a.when != b.when ? a.when < b.when : a.seq < b.seq; });
        limit = seq_;
    }
    size_t resumed = 0;
    for (;;)
    {
        ScriptEntry entry; // before the guard check, like resume_frame (design 6.3)
        Entry e{};
        bool stale = false;
        {
            // One entry at a time, and no iterator lives across a resume: a resume can tick again,
            // park, spawn or cancel. The pop and the start of the run are one step under the lock.
            std::lock_guard<std::mutex> tl(detail::task_mutex());
            std::lock_guard<std::mutex> sl(m_);
            if (ready_.empty() || ready_.front().seq >= limit)
                break;
            e = ready_.front();
            ready_.pop_front();
            e.p->parked_in = nullptr;
            e.p->started = true;
            stale = e.p->stale();
            e.p->begin_run(); // the frame is mine: its owner waits until it suspends
        }
        detail::run_frame(*e.p, e.h, stale);
        ++resumed;
    }
    reap();
    return resumed;
}

inline void Scheduler::reap()
{
    std::vector<Owned> done;
    {
        std::lock_guard<std::mutex> lock(m_);
        done.reserve(owned_.size()); // a throw here changes nothing; no throw after it
        for (size_t i = 0; i < owned_.size();)
        {
            if (owned_[i].p->is_finished())
            {
                done.push_back(owned_[i]);
                owned_.erase(owned_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
                ++i;
        }
    }
    for (Owned& o : done) // reports an error that nobody read, and releases the pin
        detail::destroy_frame(*o.p, o.h);
}

inline uint64_t Scheduler::wake_seq() const noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    return wake_;
}

inline void Scheduler::wait_work(uint64_t seq, Clock::time_point deadline) const
{
    std::unique_lock<std::mutex> lock(m_);
    auto work = [&] { return wake_ != seq || !ready_.empty(); };
    if (work())
        return;
    const Clock::time_point until = std::min(deadline, next_wake_locked());
    if (until == Clock::time_point::max())
        cv_.wait(lock, work);
    else
        cv_.wait_until(lock, until, work);
}

inline bool Scheduler::run(Duration timeout)
{
    const Clock::time_point deadline = detail::time_after(Clock::now(), timeout);
    for (;;)
    {
        const uint64_t seq = wake_seq(); // before the tick: a task that ends after it wakes the wait
        tick();
        if (size() == 0)
            return true;
        if (timeout <= Duration::zero() || Clock::now() >= deadline)
            return false;
        wait_work(seq, deadline);
    }
}

template <class T> bool Scheduler::run_until_done(Task<T>& t, Duration timeout)
{
    if (!t.valid())
        throw ScriptError("empty task");
    const Clock::time_point deadline = detail::time_after(Clock::now(), timeout);
    detail::TaskPromiseBase& p = t.base();
    {
        // The completion reads on_done under the link lock (the protocol of sync_wait).
        std::lock_guard<std::mutex> lock(detail::task_mutex());
        if (p.finished)
            return true;
        if (p.awaited || p.on_done)
            throw ScriptError("task is already awaited");
        adopt_locked(p);
        p.on_done = &Scheduler::on_task_done;
        p.on_done_ctx = this;
    }
    struct Unlink
    {
        ~Unlink()
        {
            // The caller keeps the task: a later spawn or await of it must work. A frame that
            // parks after the call parks in hot::scheduler(): this scheduler can die.
            std::lock_guard<std::mutex> lock(detail::task_mutex());
            p.on_done = nullptr;
            p.on_done_ctx = nullptr;
            if (p.sched == s)
                p.sched = nullptr;
        }
        detail::TaskPromiseBase& p;
        Scheduler* s;
    } unlink{p, this};
    t.resume();
    for (;;)
    {
        if (p.is_finished())
            return true;
        if (timeout <= Duration::zero() || Clock::now() >= deadline)
            return false;
        const uint64_t seq = wake_seq();
        tick();
        if (p.is_finished())
            return true;
        wait_work(seq, deadline);
    }
}

inline bool Scheduler::cancel(TaskId id) noexcept
{
    Owned o{};
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = std::find_if(owned_.begin(), owned_.end(), [&](const Owned& x) { return x.id == id; });
        if (it == owned_.end())
            return false;
        o = *it;
        owned_.erase(it);
    }
    detail::destroy_frame(*o.p, o.h); // without m_: the destroy takes task_mutex(), then m_
    return true;
}

inline void Scheduler::cancel_all() noexcept
{
    for (;;)
    {
        std::vector<Owned> all;
        {
            std::lock_guard<std::mutex> lock(m_);
            all.swap(owned_);
        }
        if (all.empty())
            return;
        for (Owned& o : all) // a destructor in a frame can spawn again: the loop takes those too
            detail::destroy_frame(*o.p, o.h);
    }
}

inline bool Scheduler::alive(TaskId id) const noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    for (const Owned& o : owned_)
        if (o.id == id)
            return !o.p->is_finished();
    return false;
}

inline size_t Scheduler::size() const noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    return owned_.size();
}

inline Clock::time_point Scheduler::next_wake_locked() const noexcept
{
    if (!ready_.empty())
        return Clock::time_point::min();
    Clock::time_point t = Clock::time_point::max();
    for (const Entry& e : timers_)
    {
        if (e.p->stale())
            return Clock::time_point::min();
        t = std::min(t, e.when);
    }
    return t;
}

inline Clock::time_point Scheduler::next_wake() const noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    return next_wake_locked();
}

inline void Scheduler::notify() noexcept
{
    std::lock_guard<std::mutex> lock(m_);
    ++wake_;
    cv_.notify_all();
}

} // namespace hot

#endif // HOT_HAS_TASK
