// tools/texturing/browser -- see browser.h.
#include "tools/texturing/browser.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace native::tools::tex {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool endsWith(const std::string& s, const std::string& suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }
} // namespace

bool isSkyTexture(const std::string& path) {
    if (path.empty()) return false;
    const std::string last = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    const std::string l = lower(path);
    return endsWith(last, "cube") || endsWith(l, "cube") || l.find("sky") != std::string::npos;
}

std::string folderOf(const std::string& path) {
    const size_t i = path.rfind('/');
    return i == std::string::npos ? std::string() : path.substr(0, i);
}

std::vector<std::string> folderEntries(const std::vector<std::string>& all) {
    std::set<std::string> folders;
    bool sky = false;
    for (const std::string& p : all) {
        const std::string f = folderOf(p);
        if (!f.empty()) folders.insert(f);
        if (!sky && isSkyTexture(p)) sky = true;
    }
    std::vector<std::string> out = {""};
    if (sky) out.push_back(kSkyFolder);
    out.insert(out.end(), folders.begin(), folders.end());
    return out;
}

std::vector<std::string> filterTextures(const std::vector<std::string>& all, const std::string& folder, const std::string& query) {
    const std::string q = lower(query);
    const std::string prefix = folder + "/";
    std::vector<std::string> out;
    for (const std::string& p : all) {
        const bool inFolder = folder.empty() || (folder == kSkyFolder ? isSkyTexture(p) : p.compare(0, prefix.size(), prefix) == 0);
        if (inFolder && (q.empty() || lower(p).find(q) != std::string::npos)) out.push_back(p);
    }
    return out;
}

std::string folderLabel(const std::string& folder) { return folder.empty() ? "All textures" : folder == kSkyFolder ? "Skyboxes" : folder; }

} // namespace native::tools::tex
