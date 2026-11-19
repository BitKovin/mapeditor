// base/timing -- monotonic clock helpers and a stopwatch.
//
// Owns: the one way native code measures time (log lines, perf gates, the trace writer). Thread: any.
// Spec: 01_ARCHITECTURE.md §4, 02_BUILD_AND_TOOLING.md §8, 09_TESTING_AND_PERF.md §3.
#pragma once

#include <chrono>
#include <cstdint>

namespace native::base {

using Clock = std::chrono::steady_clock;

// Microseconds since the first call in this process (the trace writer's time base).
inline int64_t nowMicros() {
    static const Clock::time_point t0 = Clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
}

class Stopwatch {
public:
    Stopwatch() : start_(Clock::now()) {}
    void restart() { start_ = Clock::now(); }
    double elapsedMs() const { return std::chrono::duration<double, std::milli>(Clock::now() - start_).count(); }
    double elapsedSeconds() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }

private:
    Clock::time_point start_;
};

} // namespace native::base
