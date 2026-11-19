// base/log -- timestamped log lines to a file and to the debugger.
//
// Owns: the process-wide log. Default file: %LOCALAPPDATA%\mapeditor_native\log.txt (created on first line). Thread: any.
// Spec: 02_BUILD_AND_TOOLING.md §8, 01_ARCHITECTURE.md §6 (errors are logged, never silently dropped).
//
// Line format:  [  12.345] E gpu: message      (seconds since start, level letter I/W/E, free text)
// Tests install a sink with setSink() to capture lines; counts() tells how many warnings / errors were logged (the
// validation-layer checks of T1.2 use it).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace native::base {

enum class LogLevel { Info, Warning, Error };

void log(LogLevel level, std::string_view message);
inline void logInfo(std::string_view m) { log(LogLevel::Info, m); }
inline void logWarning(std::string_view m) { log(LogLevel::Warning, m); }
inline void logError(std::string_view m) { log(LogLevel::Error, m); }

// Redirects the file output (empty path = the default file). Returns false when the file cannot be opened.
bool setLogFile(const std::string& path);
// The file currently written to.
std::string logFilePath();

// Replaces the file output by a callback (for tests); an empty function restores the file.
using LogSink = std::function<void(LogLevel, const std::string& formattedLine)>;
void setSink(LogSink sink);

struct LogCounts {
    uint64_t warnings = 0, errors = 0;
};
LogCounts logCounts();
void resetLogCounts();

} // namespace native::base
