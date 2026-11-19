// base/trace -- Chrome trace-event writer (chrome://tracing, Perfetto) for jobs, frames and bake steps.
//
// Owns: the optional `--trace <file>` output (02_BUILD_AND_TOOLING.md §8). Thread: any. When no trace is running a
// TraceScope costs one relaxed atomic load.
// Spec: 08_MILESTONES.md T1.1.
//
//   native::base::traceStart("trace.json");
//   { TraceScope s("bake pass", "light"); ... }
//   traceStop();               // writes the closing bracket; the file is valid JSON only after this
#pragma once

#include <string>

namespace native::base {

// Returns false when the file cannot be created. Starting while a trace runs stops the old one first.
bool traceStart(const std::string& path);
void traceStop();
bool traceRunning();

// Name the calling thread in the trace (shown in the viewer's thread list).
void traceSetThreadName(const std::string& name);

// A complete event ("ph":"X") from start to now. name / category must outlive the call only.
class TraceScope {
public:
    TraceScope(const char* name, const char* category = "app");
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    const char* name_;
    const char* category_;
    long long startMicros_;   // -1 when no trace was running at construction
};

// A counter sample ("ph":"C") drawn as a graph (e.g. pending jobs, visible stale chunks).
void traceCounter(const char* name, double value);

} // namespace native::base
