// app/app_options -- see app_options.h.
#include "app/app_options.h"

#include <cctype>
#include <cstdlib>

#include "gpu/upscaler.h"

namespace native::app {

const char* usageText() {
    return "mapeditor_native.exe [--assets <folder>] [--map <assets-relative .map.json>] [--automation-port <port>]\n"
           "                     [--no-lighting] [--validation] [--log <file>] [--trace <file>] [--screenshot <png>]\n"
           "                     [--no-streamline] [--no-fsr] [--upscaler <auto|dlss|fsr|off>] [--gpu-crash-dumps]\n";
}

ParseResult parseArgs(const std::vector<std::string>& args) {
    ParseResult r;
    auto& o = r.options;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= args.size()) {
                r.error = "missing value for " + a;
                return false;
            }
            dst = args[++i];
            return true;
        };
        if (a == "--assets") { if (!value(o.assetsRoot)) return r; }
        else if (a == "--map") { if (!value(o.map)) return r; }
        else if (a == "--log") { if (!value(o.logFile)) return r; }
        else if (a == "--trace") { if (!value(o.traceFile)) return r; }
        else if (a == "--screenshot") { if (!value(o.screenshotPath)) return r; }
        else if (a == "--automation-port") {
            std::string v;
            if (!value(v)) return r;
            char* end = nullptr;
            const long port = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || port < 1 || port > 65535) {
                r.error = "invalid port: " + v;
                return r;
            }
            o.automationPort = static_cast<int>(port);
        }
        else if (a == "--layout") {
            std::string v;
            if (!value(v)) return r;
            if (v != "1" && v != "2" && v != "4") {
                r.error = "invalid layout (use 1, 2 or 4): " + v;
                return r;
            }
            o.layoutViewports = std::atoi(v.c_str());
        }
        else if (a == "--render-mode") {
            if (!value(o.renderMode)) return r;
            if (o.renderMode != "unlit" && o.renderMode != "lit" && o.renderMode != "lightingOnly") {
                r.error = "invalid render mode (use unlit, lit or lightingOnly): " + o.renderMode;
                return r;
            }
        }
        else if (a == "--dlss") {
            if (!value(o.dlss)) return r;
            std::string lower = o.dlss;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower != "off" && lower != "quality" && lower != "dlaa" && lower != "ultraperformance" && lower != "performance" && lower != "balanced") {
                r.error = "invalid DLSS mode (use off, ultraPerformance, performance, balanced, quality or dlaa): " + o.dlss;
                return r;
            }
        }
        else if (a == "--upscaler") {
            if (!value(o.upscaler)) return r;
            bool ok = false;
            gpu::upscalerPrefFromName(o.upscaler, &ok);
            if (!ok) {
                r.error = "invalid upscaler (use auto, dlss, fsr or off): " + o.upscaler;
                return r;
            }
        }
        else if (a == "--no-fsr") o.noFsr = true;
        else if (a == "--log-input") o.logInput = true;
        else if (a == "--no-lighting") o.noLighting = true;
        else if (a == "--validation") o.validation = true;
        else if (a == "--no-streamline") o.noStreamline = true;
        else if (a == "--gpu-crash-dumps") o.gpuCrashDumps = true;
        else if (a == "--rt-lit") o.rtLit = true;
        else if (a == "--no-rt-lit") o.noRtLit = true;
        else if (a == "--rt-texel") {
            std::string v;
            if (!value(v)) return r;
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (v.empty() || *end != '\0' || !(d >= 0.0 && d <= 1024.0)) {
                r.error = "invalid texel density (0 .. 1024): " + v;
                return r;
            }
            o.rtTexel = static_cast<float>(d);
        }
        else if (a == "--rt-bounces") {
            std::string v;
            if (!value(v)) return r;
            if (v != "0" && v != "1") {
                r.error = "invalid --rt-bounces (0 or 1): " + v;
                return r;
            }
            o.rtBounces = v == "1";
        }
        else if (a == "--help" || a == "-h") o.help = true;
        else {
            r.error = "unknown option: " + a;
            return r;
        }
    }
    return r;
}

} // namespace native::app
