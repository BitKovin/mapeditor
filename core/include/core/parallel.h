#pragma once
#include <cstddef>
#include <functional>

namespace core {

// Number of helper threads parallelFor() uses besides the calling thread. Default: hardware concurrency - 1 (0 on a single core machine),
// or the environment variable MAPEDITOR_THREADS (total threads including the caller) when it is set.
unsigned parallelHelpers();
// Overrides the helper count (0 = run everything on the calling thread). Takes effect for the next parallelFor().
void setParallelHelpers(unsigned helpers);

// Splits [0, n) into ranges of about `grain` items and runs fn(begin, end) for them on the helper threads and on the caller, which returns when
// every range is done. The first exception thrown by any range is rethrown on the caller. Calls from inside a range run inline (no nesting),
// and concurrent calls from different threads are serialised. Ranges are handed out dynamically, so uneven work balances itself.
void parallelFor(size_t n, size_t grain, const std::function<void(size_t begin, size_t end)>& fn);

}  // namespace core
