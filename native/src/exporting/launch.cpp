#include "exporting/launch.h"

namespace native::exporting {

namespace fs = std::filesystem;

namespace {

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

std::vector<std::string> splitArgs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (c == ' ' && !quoted) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
            any = true;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

} // namespace

LaunchCommand launchCommand(const LaunchSettings& s, const fs::path& assetsRoot, const std::string& bspRelative) {
    const std::string rel = fs::path(bspRelative).generic_string();
    const std::string name = fs::path(rel).stem().string();
    const std::string absolute = (assetsRoot / fs::path(rel)).make_preferred().string();
    const std::string game = assetsRoot.string();
    const auto expand = [&](std::string t) {
        t = replaceAll(t, "{mapname}", name);
        t = replaceAll(t, "{map}", rel);
        t = replaceAll(t, "{bsp}", absolute);
        return replaceAll(t, "{game}", game);
    };
    LaunchCommand c;
    c.exe = s.exe;
    c.args = splitArgs(expand(s.args));
    c.workingDir = s.workingDir.empty() ? fs::path(s.exe).parent_path().string() : expand(s.workingDir);
    return c;
}

} // namespace native::exporting
