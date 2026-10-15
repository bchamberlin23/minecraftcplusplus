#pragma once
#include <cstddef>
#include <functional>

// Fixed worker pool for data-parallel batch work. The calling thread participates.
// Not re-entrant: do not call parallelFor from inside a job. Calls from several threads run at the same time (the
// workers serve the newest batch first).
namespace Jobs {
void init(unsigned threads = 0);  // 0 = hardware concurrency
void shutdown();
unsigned threadCount();
void parallelFor(size_t count, const std::function<void(size_t)>& fn);
}  // namespace Jobs
