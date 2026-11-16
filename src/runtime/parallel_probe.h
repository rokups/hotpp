// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A parallel test of many probe paths. A warm reload tests thousands of paths that must not exist
// (the shadow paths of the headers). One thread needs about 30 ms for 13000 stat calls.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace hot::detail
{

// Returns the index of a path for which "pred" is true, or "items.size()" when there is none. With
// more than one match the result can be any of the matches. "pred" must be thread safe. A short
// list runs on the calling thread.
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
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    const auto work = [&](size_t id)
    {
        for (size_t i = id; i < n && found.load(std::memory_order_relaxed) == n; i += threads)
            if (pred(items[i]))
            {
                size_t expect = n;
                found.compare_exchange_strong(expect, i);
            }
    };
    for (size_t t = 1; t < threads; ++t)
        pool.emplace_back(work, t);
    work(0);
    for (std::thread& t : pool)
        t.join();
    return found.load();
}

// Runs "fn(i)" for each i in [0, n) on up to 8 threads. "fn" must be thread safe and write only to
// its own index. A short range runs on the calling thread.
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
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    const auto work = [&](size_t id)
    {
        for (size_t i = id; i < n; i += threads)
            fn(i);
    };
    for (size_t t = 1; t < threads; ++t)
        pool.emplace_back(work, t);
    work(0);
    for (std::thread& t : pool)
        t.join();
}

} // namespace hot::detail
