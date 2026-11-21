// app/app_options -- the command line of mapeditor_native.exe (02_BUILD_AND_TOOLING.md §6).
//
// Owns: parsing only (no Qt, no I/O), so it is unit-testable. Thread: any.
//   mapeditor_native.exe [--assets <folder>] [--map <assets-relative .map.json>] [--automation-port <port>]
//                        [--no-lighting] [--validation] [--log <file>] [--trace <file>] [--screenshot <png>]
//                        [--layout <1|2|4>] [--log-input] [--render-mode <unlit|lit|lightingOnly>]
//                        [--no-streamline] [--gpu-crash-dumps] [--dlss <off|quality|dlaa>] [--rt-lit]
// --no-streamline creates the Vulkan device the normal way (no NVIDIA Streamline interposer: no Reflex, no DLSS), T6.7.
// --dlss starts every perspective viewport with DLSS Super Resolution off / Quality / DLAA instead of its saved setting (T6.8; there is no lower preset).
// --rt-lit shows the Lit and Lighting only modes of perspective viewports with the ray traced Lit path (R2: ambient occlusion only until R3) instead of the chunked bake, which then does not run.
// --gpu-crash-dumps enables the NV device diagnostics so a lost device writes gpu_crash_<time>.txt next to the log (T6.7).
// --trace writes a Chrome trace-event file (chrome://tracing, Perfetto) of jobs / frames / bake steps.
// --layout starts with 1, 2 or 4 viewports instead of the saved layout (T1.4).
// --screenshot is a development aid (added with T0.4 for visual checks): after the window is shown and has painted once the
// window is saved as a PNG and the program exits with code 0. That run neither reads nor writes the saved layout, so a visual
// check is always the default layout (or the one named by --layout).
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
    bool noStreamline = false;    // --no-streamline
    bool gpuCrashDumps = false;   // --gpu-crash-dumps
    bool rtLit = false;           // --rt-lit
    std::string logFile;          // --log
    std::string traceFile;        // --trace
    std::string screenshotPath;   // --screenshot
    bool logInput = false;        // --log-input
    std::string dlss;             // --dlss off | quality | dlaa (every viewport starts in it); empty = the saved one
    std::string renderMode;       // --render-mode unlit | lit | lightingOnly (every viewport starts in it); empty = the saved one
    int layoutViewports = 0;      // --layout; 0 = the saved layout (default 1 viewport)
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
