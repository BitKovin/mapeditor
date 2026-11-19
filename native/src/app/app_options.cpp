// app/app_options -- see app_options.h.
#include "app/app_options.h"

#include <cstdlib>

namespace native::app {

const char* usageText() {
    return "mapeditor_native.exe [--assets <folder>] [--map <assets-relative .map.json>] [--automation-port <port>]\n"
           "                     [--no-lighting] [--validation] [--log <file>] [--trace <file>] [--screenshot <png>]\n";
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
        else if (a == "--no-lighting") o.noLighting = true;
        else if (a == "--validation") o.validation = true;
        else if (a == "--help" || a == "-h") o.help = true;
        else {
            r.error = "unknown option: " + a;
            return r;
        }
    }
    return r;
}

} // namespace native::app
