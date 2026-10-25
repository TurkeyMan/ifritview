#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

// f(i) for each i in [0, n), on `threads` threads (0: one a core), each
// taking a run of consecutive i: what neighbouring i write stays together.
// Each f(i) independent of the others; on this thread when one is enough.
template <typename F>
void parallel(size_t n, F f, unsigned threads = 0)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    const size_t t = std::min<size_t>(threads, n);
    if (t <= 1)
    {
        for (size_t i = 0; i < n; ++i)
            f(i);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(t);
    for (size_t k = 0; k < t; ++k)
        pool.emplace_back(
            [&, k]
            {
                for (size_t i = n * k / t, end = n * (k + 1) / t; i < end; ++i)
                    f(i);
            });
    for (std::thread& th : pool)
        th.join();
}
