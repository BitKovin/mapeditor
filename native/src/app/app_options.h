// app/app_options -- the command line of mapeditor_native.exe (02_BUILD_AND_TOOLING.md §6).
//
// Owns: parsing only (no Qt, no I/O), so it is unit-testable. Thread: any.
//   mapeditor_native.exe [--assets <folder>] [--map <assets-relative .map.json>] [--automation-port <port>]
//                        [--no-lighting] [--validation] [--log <file>] [--trace <file>] [--screenshot <png>]
// --trace writes a Chrome trace-event file (chrome://tracing, Perfetto) of jobs / frames / bake steps.
// --screenshot is a development aid (added with T0.4 for visual checks): after the window is shown and has painted once the
// window is saved as a PNG and the program exits with code 0.
#pragma once

#include <string>
#include <vector>

namespace native::app {

struct AppOptions {
    std::string assetsRoot;       // --assets
    std::string map;              // --map, relative to the assets root
    int automationPort = 0;       // --automation-port; 0 = off
    bool noLighting = false;      // --no-lighting
    bool validation = false;      // --validation
    std::string logFile;          // --log
    std::string traceFile;        // --trace
    std::string screenshotPath;   // --screenshot
    bool help = false;            // --help / -h
};

struct ParseResult {
    AppOptions options;
    std::string error;            // empty when the command line is valid
};

// args excludes the program name.
ParseResult parseArgs(const std::vector<std::string>& args);

// One line per option, for --help and for error messages.
const char* usageText();

} // namespace native::app
