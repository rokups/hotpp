// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Data that the files of the runtime share. This header is not public.
#pragma once
#include "hot/runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace hot
{

// The live objects (state.cpp): a list through the members live_prev and live_next of each
// shell, in the order of creation. A walk gives the pair (shell, creator). The creator is the
// generation whose code made the object with "new" (construct_shell), or 0. That code also made
// the handle of the object, for example the control block of a std::shared_ptr. So the
// generation must stay loaded while the object lives. Do not insert or erase during a walk.
// With HOT_THREAD_SAFE=1 (the default) insert and erase take a spinlock, so any thread can create
// and destroy script objects. The order of the list is the order in which the threads get the
// lock. With HOT_THREAD_SAFE=0 the list has no lock: only one thread at a time creates or
// destroys script objects, and a debug build asserts when two changes overlap (state.cpp).
// A walk and a read of size() must hold a LiveObjects::Guard when other threads can create or
// destroy objects. The lock is not recursive: inside a Guard do not run script code, and do not
// create or destroy a script object. With HOT_THREAD_SAFE=0 the Guard does nothing. The migration
// of a commit walks the objects without a Guard, because it runs script code: no thread creates
// or destroys an object while commit_pending runs (doc/internals.md, "Threads and the safe point").
class LiveObjects
{
public:
    // Holds the lock of the list. With HOT_THREAD_SAFE=0 it does nothing.
    class Guard
    {
    public:
        explicit Guard(const LiveObjects& l) : l_(l) { l_.lock(); }
        ~Guard() { l_.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        const LiveObjects& l_;
    };

    class iterator
    {
    public:
        explicit iterator(ShellBase* s) : s_(s) {}
        // "creator" is written after the insert (note_creator), so the read is atomic.
        std::pair<ShellBase*, uint32_t> operator*() const
        {
#if defined(_MSC_VER) && !defined(__clang__)
            // A volatile load of 32 bits, then a compiler barrier.
            const auto creator =
                static_cast<uint32_t>(__iso_volatile_load32(reinterpret_cast<const volatile int*>(&s_->creator)));
            _ReadWriteBarrier();
            return {s_, creator};
#else
            return {s_, __atomic_load_n(&s_->creator, __ATOMIC_RELAXED)};
#endif
        }
        iterator& operator++()
        {
            s_ = s_->live_next;
            return *this;
        }
        bool operator!=(const iterator& o) const { return s_ != o.s_; }
        bool operator==(const iterator& o) const { return s_ == o.s_; }

    private:
        ShellBase* s_;
    };

    void insert(ShellBase* s);
    void erase(ShellBase* s); // a shell that is not in the list is ignored
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    iterator begin() const { return iterator(head_); }
    iterator end() const { return iterator(nullptr); }

private:
#if HOT_THREAD_SAFE
    // A spinlock: a change of the list is a few stores. A waiting thread spins on a read and
    // gives the CPU away after some tries. The flag is next to the list data.
    void lock() const
    {
        // Test and test-and-set: a waiting thread reads the flag and does not write the line.
        for (unsigned tries = 0;; ++tries)
        {
            if (!lock_.load(std::memory_order_relaxed) && !lock_.exchange(true, std::memory_order_acquire))
                return;
            if (tries > 64)
                std::this_thread::yield();
        }
    }
    void unlock() const { lock_.store(false, std::memory_order_release); }
    mutable std::atomic<bool> lock_{false};
#else
    void lock() const {}
    void unlock() const {}
#endif
    ShellBase* head_ = nullptr;
    ShellBase* tail_ = nullptr;
    size_t size_ = 0;
};

extern LiveObjects g_live;

// The count of commits that ran while a thread was in script code (threads.cpp).
extern std::atomic<uint64_t> g_unsafe_commits;

// Fills a new state block (migration.cpp).
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan, char* from, char* to);

} // namespace hot
