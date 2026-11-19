// base/trace -- see trace.h.
#include "base/trace.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "base/timing.h"

namespace native::base {

namespace {

struct TraceState {
    std::mutex mutex;
    std::FILE* file = nullptr;
    bool first = true;
};

TraceState& state() {
    static TraceState s;
    return s;
}

std::atomic<bool> g_running{false};

unsigned threadId() {
    static std::atomic<unsigned> next{1};
    thread_local unsigned id = next.fetch_add(1);
    return id;
}

// JSON string escape for names we control (no control characters expected, but be safe).
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) o.push_back(' ');
        else o.push_back(c);
    }
    return o;
}

void writeEvent(const std::string& json) {
    TraceState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.file) return;
    std::fputs(s.first ? "\n" : ",\n", s.file);
    s.first = false;
    std::fputs(json.c_str(), s.file);
}

} // namespace

bool traceStart(const std::string& path) {
    traceStop();
    TraceState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    std::fputs("{\"displayTimeUnit\":\"ms\",\"traceEvents\":[", f);
    s.file = f;
    s.first = true;
    g_running.store(true, std::memory_order_release);
    return true;
}

void traceStop() {
    TraceState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    g_running.store(false, std::memory_order_release);
    if (!s.file) return;
    std::fputs("\n]}\n", s.file);
    std::fclose(s.file);
    s.file = nullptr;
}

bool traceRunning() { return g_running.load(std::memory_order_acquire); }

void traceSetThreadName(const std::string& name) {
    if (!traceRunning()) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"args\":{\"name\":\"%s\"}}", threadId(),
                  esc(name).c_str());
    writeEvent(buf);
}

TraceScope::TraceScope(const char* name, const char* category)
    : name_(name), category_(category), startMicros_(traceRunning() ? nowMicros() : -1) {}

TraceScope::~TraceScope() {
    if (startMicros_ < 0 || !traceRunning()) return;
    const long long end = nowMicros();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\",\"ts\":%lld,\"dur\":%lld,\"pid\":1,\"tid\":%u}",
                  esc(name_).c_str(), esc(category_).c_str(), startMicros_, end - startMicros_, threadId());
    writeEvent(buf);
}

void traceCounter(const char* name, double value) {
    if (!traceRunning()) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"ph\":\"C\",\"ts\":%lld,\"pid\":1,\"tid\":%u,\"args\":{\"value\":%.17g}}",
                  esc(name).c_str(), nowMicros(), threadId(), value);
    writeEvent(buf);
}

} // namespace native::base
