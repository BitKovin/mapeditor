#include "core/settings.h"
#include <fstream>
#include <nlohmann/json.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

namespace core {

std::string pathToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

Settings loadSettings(const fs::path& file) {
    Settings s;
    std::ifstream in(file, std::ios::binary);
    if (!in) return s;
    try {
        const json j = json::parse(in);
        if (j.is_object()) {
            if (j.contains("assetsRoot") && j["assetsRoot"].is_string()) s.assetsRoot = j["assetsRoot"].get<std::string>();
            if (j.contains("lastMap") && j["lastMap"].is_string()) s.lastMap = j["lastMap"].get<std::string>();
        }
    } catch (const std::exception&) {
        // fall through with defaults
    }
    return s;
}

bool saveSettings(const fs::path& file, const Settings& s) {
    try {
        if (file.has_parent_path()) fs::create_directories(file.parent_path());
        fs::path tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << json{{"assetsRoot", s.assetsRoot}, {"lastMap", s.lastMap}}.dump(2) << "\n";
            out.flush();
            if (!out) return false;
        }
        fs::rename(tmp, file);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace core
