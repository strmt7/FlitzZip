#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <vector>

namespace superzip {

// Purpose: Run balanced, nonempty, disjoint ranges within a total worker budget, including the calling thread.
// Inputs: count is the item count; worker_count bounds workers; fn accepts [begin,end) and permits concurrent calls.
// Outputs: On success covers each item once; all launched tasks join before return/unwinding. Propagates failures.
template <typename Fn> void run_parallel_ranges(std::size_t count, std::uint32_t worker_count, Fn fn) {
    if (count == 0) {
        return;
    }
    const auto workers = std::min<std::size_t>(std::max(1U, worker_count), count);
    if (workers == 1) {
        fn(0, count);
        return;
    }
    const auto quotient = count / workers;
    const auto remainder = count % workers;
    std::vector<std::future<void>> futures;
    futures.reserve(workers - 1U);
    std::size_t begin = 0;
    for (std::size_t worker = 0; worker + 1U < workers; ++worker) {
        const auto end = begin + quotient + (worker < remainder ? 1U : 0U);
        futures.push_back(std::async(std::launch::async, [begin, end, &fn] { fn(begin, end); }));
        begin = end;
    }
    fn(begin, count);
    for (auto& future : futures) {
        future.get();
    }
}

}  // namespace superzip
