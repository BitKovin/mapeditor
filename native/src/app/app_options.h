// app/app_options -- the command line of mapeditor_native.exe (02_BUILD_AND_TOOLING.md §6).
//
// Owns: parsing only (no Qt, no I/O), so it is unit-testable. Thread: any.
//   mapeditor_native.exe [--assets <folder>] [--map <assets-relative .map.json>] [--automation-port <port>]
//                        [--no-lighting] [--validation] [--log <file>] [--trace <file>] [--screenshot <png>]
//                        [--layout <1|2|4>] [--log-input] [--render-mode <unlit|lit|lightingOnly>]
//                        [--no-streamline] [--no-fsr] [--upscaler <auto|dlss|fsr|off>] [--gpu-crash-dumps] [--dlss <off|ultraPerformance|performance|balanced|quality|dlaa>] [--rt-lit|--no-rt-lit] [--rt-texel <n>] [--rt-bounces <0|1>]
// --no-streamline creates the Vulkan device the normal way (no NVIDIA Streamline interposer: no Reflex, no DLSS), T6.7.
// --no-fsr does not load AMD FidelityFX (FSR, the upscaler that runs on any GPU), T15.1. --upscaler picks the temporal upscaler the --dlss / Upscaling modes use: auto (DLSS on an RTX
// GPU, FSR elsewhere), dlss, fsr or off; a choice that cannot run on the device gives way to the other one.
// --dlss starts every perspective viewport with upscaling (DLSS or FSR) off / ultraPerformance / performance / balanced / quality / dlaa instead of its saved setting (T6.8).
// The ray traced Lit path is the default on a GPU with ray queries (T14.15): the Lit and Lighting only modes of perspective viewports use it (ambient, sun, sky, local lights with
// ray traced shadows, bounces) instead of the chunked bake, which then does not run. --no-rt-lit (or --no-lighting) keeps the chunked bake; --rt-lit forces the ray traced path.
// Viewports also start in the Lit mode (once per profile: the saved choice wins afterwards). --rt-texel <n> = the lightmap look of that path: shading at the centres of a planar grid of n texels per metre, bilinearly
// weighted like the game samples its lightmaps (default 16, the game base density; 0 = smooth per-pixel light).
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
    bool noFsr = false;           // --no-fsr
    std::string upscaler;         // --upscaler auto | dlss | fsr | off; empty = auto
    bool gpuCrashDumps = false;   // --gpu-crash-dumps
    bool rtLit = false;           // --rt-lit (forces the ray traced Lit path on)
    bool noRtLit = false;         // --no-rt-lit: the chunked bake instead of the ray traced Lit path (which is the default on a ray query capable GPU)
    float rtTexel = 16.0f;        // --rt-texel: texels per metre of the lightmap look (0 = off)
    bool rtBounces = true;        // --rt-bounces 0|1: bounce light (the SHaRC radiance cache) of the ray traced Lit path
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
