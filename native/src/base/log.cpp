// base/log -- see log.h.
#include "base/log.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "base/timing.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace native::base {

namespace {

struct LogState {
    std::mutex mutex;
    std::ofstream file;
    std::string path;
    bool opened = false;
    LogSink sink;
    LogCounts counts;
};

LogState& state() {
    static LogState s;
    return s;
}

std::string defaultPath() {
    std::string base = ".";
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "LOCALAPPDATA") == 0 && env) base = env;
    std::free(env);
#else
    if (const char* env = std::getenv("LOCALAPPDATA")) base = env;
#endif
    std::filesystem::path p = base;
    return (p / "mapeditor_native" / "log.txt").string();
}

bool openFile(LogState& s, const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (s.file.is_open()) s.file.close();
    s.file.open(p, std::ios::app);
    s.path = path;
    s.opened = true;
    return s.file.is_open();
}

char letter(LogLevel l) { return l == LogLevel::Info ? 'I' : l == LogLevel::Warning ? 'W' : 'E'; }

} // namespace

void log(LogLevel level, std::string_view message) {
    char head[48];
    std::snprintf(head, sizeof(head), "[%8.3f] %c ", static_cast<double>(nowMicros()) / 1.0e6, letter(level));
    std::string line = head;
    line.append(message);

    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (level == LogLevel::Warning) ++s.counts.warnings;
    if (level == LogLevel::Error) ++s.counts.errors;
    if (s.sink) {
        s.sink(level, line);
        return;
    }
    if (!s.opened) openFile(s, defaultPath());
    if (s.file.is_open()) {
        s.file << line << '\n';
        s.file.flush();
    }
#ifdef _WIN32
    OutputDebugStringA((line + "\n").c_str());
#endif
}

bool setLogFile(const std::string& path) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return openFile(s, path.empty() ? defaultPath() : path);
}

std::string logFilePath() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.opened ? s.path : defaultPath();
}

void setSink(LogSink sink) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.sink = std::move(sink);
}

LogCounts logCounts() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.counts;
}

void resetLogCounts() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.counts = {};
}

} // namespace native::base
