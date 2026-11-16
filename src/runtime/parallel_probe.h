// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A parallel test of many probe paths. A warm reload tests thousands of paths that must not exist
// (the shadow paths of the headers). One thread needs about 30 ms for 13000 stat calls.
//
// A throw in a worker thread is kept and thrown again on the calling thread after all threads
// ended. A thread that cannot start is not an error: the threads that started finish the work,
// and the calling thread takes the part of the missing threads.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace hot::detail
{

// For tests: the number of thread starts that work before one start throws. A negative value
// (the default) never throws.
inline std::atomic<int>& probe_spawn_budget()
{
    static std::atomic<int> budget{-1};
    return budget;
}

// Runs "body(id)" for id in [0, threads) on "threads" threads. The calling thread runs id 0 and,
// for each thread that cannot start, the id of that thread. The first throw is thrown again after
// the join.
template <typename Body>
void run_ids(size_t threads, Body body)
{
    std::exception_ptr error;
    std::mutex error_mutex;
    const auto guarded = [&](size_t id)
    {
        try
        {
            body(id);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!error)
                error = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    std::vector<size_t> not_started;
    try
    {
        pool.reserve(threads - 1);
    }
    catch (...)
    {
        // No room for the handles: run all ids on this thread.
        for (size_t id = 0; id < threads; ++id)
            guarded(id);
        if (error)
            std::rethrow_exception(error);
        return;
    }
    for (size_t id = 1; id < threads; ++id)
    {
        try
        {
            int budget = probe_spawn_budget().load();
            if (budget >= 0)
            {
                if (budget == 0)
                    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
                probe_spawn_budget().store(budget - 1);
            }
            pool.emplace_back(guarded, id);
        }
        catch (...)
        {
            not_started.push_back(id);
        }
    }
    guarded(0);
    for (const size_t id : not_started)
        guarded(id);
    for (std::thread& t : pool)
        t.join();
    if (error)
        std::rethrow_exception(error);
}

// Returns the index of a path for which "pred" is true, or "items.size()" when there is none. With
// more than one match the result can be any of the matches. "pred" must be thread safe. A short
// list runs on the calling thread. A throw of "pred" is thrown again to the caller.
template <typename T, typename Pred>
size_t find_any_parallel(const std::vector<T>& items, Pred pred)
{
    constexpr size_t kSerialLimit = 512;
    const size_t n = items.size();
    const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t threads = std::min<size_t>({hw, 8, n / (kSerialLimit / 2)});
    if (n <= kSerialLimit || threads < 2)
    {
        for (size_t i = 0; i < n; ++i)
            if (pred(items[i]))
                return i;
        return n;
    }
    std::atomic<size_t> found{n};
    std::atomic<bool> stop{false};
    run_ids(threads, [&](size_t id)
    {
        for (size_t i = id; i < n && !stop.load(std::memory_order_relaxed); i += threads)
            if (pred(items[i]))
            {
                size_t expect = n;
                found.compare_exchange_strong(expect, i);
                stop.store(true, std::memory_order_relaxed);
            }
    });
    return found.load();
}

// Runs "fn(i)" for each i in [0, n) on up to 8 threads. "fn" must be thread safe and write only to
// its own index. A short range runs on the calling thread. A throw of "fn" is thrown again.
template <typename Fn>
void parallel_for(size_t n, Fn fn)
{
    const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t threads = std::min<size_t>({hw, 8, n / 256});
    if (threads < 2)
    {
        for (size_t i = 0; i < n; ++i)
            fn(i);
        return;
    }
    run_ids(threads, [&](size_t id)
    {
        for (size_t i = id; i < n; i += threads)
            fn(i);
    });
}

} // namespace hot::detail
